#ifndef BROOK_H
#define BROOK_H

#include "ktypes.h"
#include "error.h"
#include "klib.h"
#include "atomics.h"
#include "cabin_layout.h"
#include "boxos_magic.h"

/*
 * Brook — single-producer/single-consumer (SPSC) ordered streaming
 * primitive. Tag-driven discovery, refcounted lifetime, fixed-size
 * frames. Sits at the same architectural tier as Touch and Bay.
 *
 * Discovery
 * ---------
 * Both writer and reader call brook_open(tag, frame_size, frame_count,
 * flags) with the SAME tag string. The kernel resolves the tag via the
 * TagFS registry and binds the caller into the BrookObject backing
 * those pages. flags carries the role (BROOK_WRITER | BROOK_READER —
 * exactly one) and optionally BROOK_CREATE (create the Brook if the
 * tag is unbound). Hard SPSC: a second open with the same role returns
 * ERR_BUSY.
 *
 * Layout (per Brook)
 * ------------------
 *   - 1 header page (4 KiB) — BrookHeader. Mapped read-write into BOTH
 *     cabins. Two cachelines of real data (consumer cursor on line 0,
 *     producer cursor on line 1) to avoid false sharing.
 *   - slot region — frame_size * frame_count bytes, rounded up to page
 *     granularity. Mapped read-write into both cabins. 4 KiB pages by
 *     default; 2 MiB huge pages when total ≥ 2 MiB (implicit huge-page
 *     policy mirrors Bay).
 *
 * Hot path
 * --------
 * brook_push / brook_pop run lock-free entirely in userspace:
 *
 *   writer: tail = atomic_load(tail); head = atomic_load_acquire(head);
 *           if (tail - head < cap) {
 *             memcpy(slot[tail % cap], frame, frame_size);
 *             atomic_store_release(tail, tail + 1);
 *             return OK;
 *           }
 *           // full — fall back to wait
 *
 *   reader: mirror.
 *
 * Wait path (no syscall)
 * ----------------------
 * brook_push (block) on full ring loops via pause / UMWAIT / yield on
 * BrookHeader. As soon as reader pops, head advances and the cache
 * invalidation lets writer re-check + push. brook_pop (block) on empty
 * ring — symmetric. No wake-up syscall is needed because the
 * producer-consumer cursor IS the synchronization point — the same
 * model Touch uses (touch_wait_umwait / touch_wait_pause in boxlib).
 *
 * Peer death
 * ----------
 * When one side releases (or its cabin is destroyed), the kernel flips
 * the matching `*_alive` flag to 0 in the shared header. The surviving
 * peer's wait-loop re-reads it on every iteration:
 *   - writer: full ring + reader_alive==0 + reader_ever_attached==1
 *             → -ERR_PROCESS_TERMINATED
 *   - reader: empty ring + writer_alive==0 + writer_ever_attached==1
 *             → -ERR_END_OF_FILE
 *   - reader: frames still in ring after writer left → drain normally;
 *             EOF only on the empty re-check that follows.
 *
 * The `*_ever_attached` history bits prevent a reader that opens before
 * any writer from returning EOF on its first pop. Set on first attach,
 * never cleared.
 *
 * Lifecycle
 * ---------
 * BrookObject.ref_count counts how many roles are still claimed
 * (writer + reader). When BOTH peers have released (ref_count == 0),
 * the BrookObject unlinks from the bucket and its backing chunks
 * return to PMM. Either side releasing alone keeps the Brook alive so
 * the surviving peer can still drain residual frames.
 *
 * process_destroy auto-releases every Brook claim the cabin holds
 * (BrookCleanupProcess) so a crashing app never leaks backing pages
 * AND its peer is notified within one wait-cycle.
 *
 * Memory ordering
 * ---------------
 * head is consumer-write/producer-read; tail is producer-write/consumer-
 * read. Both use acquire on load + release on store so the payload
 * memcpy at slot[tail % cap] is published before tail increment
 * (writer) and visible after tail load (reader). The *_alive flags
 * are RELEASE-stored in the kernel and ACQUIRE-loaded in userspace
 * so peer-death is observable.
 */

/* Role + flag bits for brook_open. */
#define BROOK_WRITER       0x01u
#define BROOK_READER       0x02u
#define BROOK_CREATE       0x10u
/* BROOK_STREAM — opt-in streaming semantics. Default behaviour is
 * single-session: reader returns ERR_END_OF_FILE on writer-leave, writer
 * returns ERR_PROCESS_TERMINATED on reader-leave; once that signal is
 * emitted, the Brook session is FROZEN — no further attaches succeed.
 * This eliminates the classic re-attach race (peer transition observed
 * by the survivor as an EOF that "missed" the new attach) via an
 * atomic CAS on the *_alive flag.
 *
 * With BROOK_STREAM the survivor NEVER emits EOF/PROCESS_TERMINATED on
 * peer transitions — pop/push block through writer-leave/reader-leave
 * cycles, waiting for re-attach. Suitable for long-running daemons that
 * legitimately swap producers/consumers over a stream's lifetime. */
#define BROOK_STREAM       0x20u
#define BROOK_FLAGS_MASK   (BROOK_WRITER | BROOK_READER | BROOK_CREATE | BROOK_STREAM)

/* Sticky terminal sentinel for the *_alive flag. Set by the surviving
 * peer via CAS-from-0 when it decides EOF/PROCESS_TERMINATED in single-
 * session mode. Subsequent kernel-side attach attempts CAS expected=0
 * and fail with ERR_INVALID_STATE — guaranteeing the EOF decision is
 * race-free with concurrent re-attach attempts. */
#define BROOK_ALIVE_FROZEN 0xFFFFFFFFu

/* frame_size bounds. Min 8 B (atomic-aligned), max 64 KiB (above that
 * use Bay for bulk-data and Brook for control events). */
#define BROOK_FRAME_SIZE_MIN     8u
#define BROOK_FRAME_SIZE_MAX     (64u * 1024u)

/* frame_count bounds. Must be a power of two for cheap (pos % cap)
 * modulo (encoded as pos & (cap - 1) in the hot path). */
#define BROOK_FRAME_COUNT_MIN    2u
#define BROOK_FRAME_COUNT_MAX    16384u

/* Aggregate cap: frame_size * frame_count ≤ 1 GiB per Brook. Bay is
 * the right tool above that ceiling. */
#define BROOK_MAX_TOTAL_SIZE     (1ULL * 1024 * 1024 * 1024)

typedef struct BrookClaim BrookClaim;
typedef struct BrookObject BrookObject;
struct process_t;

/* ─────────────────────────────────────────────────────────────────────
 * BrookHeader — shared (mapped read-write into both cabins).
 *
 * Two-cacheline layout with per-side state co-located with that side's
 * cursor. The choice matters for UMWAIT: a peer that monitors the
 * other side's cacheline is woken on ANY write to it, so co-locating
 * alive/ever_attached with the cursor means a peer-death (kernel write
 * to alive=0) wakes the waiter immediately — no need for periodic
 * polling to discover peer state changes.
 *
 *   Cacheline 0 (writer-state) — watched by reader:
 *     - tail [writer writes; reader reads]
 *     - writer_alive [kernel writes]
 *     - writer_ever_attached [kernel writes]
 *     - frame_size, frame_count, magic [immutable]
 *
 *   Cacheline 1 (reader-state) — watched by writer:
 *     - head [reader writes; writer reads]
 *     - reader_alive [kernel writes]
 *     - reader_ever_attached [kernel writes]
 *
 * head/tail are monotonic uint64 counters; modulo against frame_count
 * (power-of-2) gives the slot index.
 *
 * Userspace EOF/PROCESS_TERMINATED semantics:
 *   alive==0 && ever_attached==1 → peer left; emit terminal error.
 *   alive==0 && ever_attached==0 → no peer YET; keep waiting (lets a
 *                                  reader open before any writer attaches).
 * ───────────────────────────────────────────────────────────────────── */
typedef struct {
    /* Cacheline 0 (64 B) — writer state, watched by reader */
    volatile uint64_t tail;                  /* writer writes; reader reads */
    volatile uint32_t writer_alive;
    volatile uint32_t writer_ever_attached;
    uint32_t          frame_size;            /* immutable after create */
    uint32_t          frame_count;           /* immutable, power-of-2 */
    uint64_t          magic;                 /* BROOK_HEADER_MAGIC */
    uint8_t           _pad_line0[32];

    /* Cacheline 1 (64 B) — reader state, watched by writer */
    volatile uint64_t head;                  /* reader writes; writer reads */
    volatile uint32_t reader_alive;
    volatile uint32_t reader_ever_attached;
    uint8_t           _pad_line1[48];
} BrookHeader;

_Static_assert(sizeof(BrookHeader) == 128,
               "BrookHeader must be exactly two cachelines (128 B)");
_Static_assert(__builtin_offsetof(BrookHeader, tail) == 0,
               "BrookHeader.tail must be at offset 0 (CL0 = writer state)");
_Static_assert(__builtin_offsetof(BrookHeader, head) == 64,
               "BrookHeader.head must be at offset 64 (CL1 = reader state)");
_Static_assert(__builtin_offsetof(BrookHeader, writer_alive) < 64,
               "writer_alive must share CL0 with tail (UMWAIT correctness)");
_Static_assert(__builtin_offsetof(BrookHeader, reader_alive) >= 64,
               "reader_alive must share CL1 with head (UMWAIT correctness)");

/* ─────────────────────────────────────────────────────────────────────
 * BrookObject — kernel-side per-tag record. Identified by tag_id.
 * Hash-bucketed (see brook.c).
 *
 * frame_size, frame_count, slot allocation are immutable after create.
 * The bucket lock serialises writer_pid/reader_pid/ref_count mutations
 * and the create-or-attach race.
 * ───────────────────────────────────────────────────────────────────── */
struct BrookObject {
    BrookObject  *bucket_next;         /* hash-bucket collision chain */

    uint16_t      tag_id;              /* TagFS-interned id */
    uint16_t      slot_page_class;     /* 12 (4 KiB) or 21 (2 MiB) */
    uint32_t      flags;               /* sticky creation flags */
    uint32_t      frame_size;          /* immutable */
    uint32_t      frame_count;         /* immutable, power-of-2 */

    /* Physical backing. */
    uint64_t      header_phys;         /* 4 KiB BrookHeader page */
    uint64_t      slot_total_size;     /* frame_size * frame_count, page-rounded */
    uint64_t      slot_chunk_size;     /* PMM_PAGE_SIZE or VMM_LARGE_PAGE_2M_SIZE */
    uint32_t      slot_chunk_count;    /* ceil(slot_total_size / slot_chunk_size) */
    uint32_t      _pad0;
    uint64_t     *slot_chunks;         /* phys addrs, slot_chunk_count entries */

    /* Role ownership. 0 = vacant. Hard SPSC enforced at open(). All
     * mutations happen under the bucket lock. */
    uint32_t      writer_pid;
    uint32_t      reader_pid;
    uint32_t      ref_count;           /* (writer_pid?1:0) + (reader_pid?1:0) */
    uint32_t      _pad1;

    uint64_t      create_pid;
    uint64_t      create_tsc;
};

/* ─────────────────────────────────────────────────────────────────────
 * BrookClaim — one per cabin-open of a Brook. Lives in two places:
 *   - bucket-side: BrookObject->{writer,reader}_pid + ref_count
 *   - process-side: proc->brook_claims_head singly-linked list
 *
 * user_va_header identifies the claim for release ops (it is unique
 * per Brook open within a cabin and easy to derive from the boxlib
 * opaque handle).
 * ───────────────────────────────────────────────────────────────────── */
struct BrookClaim {
    BrookObject  *brook;
    BrookClaim   *proc_next;
    uint64_t      user_va_header;      /* mapped BrookHeader page VA */
    uint64_t      user_va_slots;       /* mapped slot region VA */
    uint32_t      role;                /* BROOK_WRITER or BROOK_READER */
    uint32_t      flags;
    struct process_t *proc;
};

void BrookInit(void);

/* ─────────────────────────────────────────────────────────────────────
 * BrookOpenInternal — open or create + map a Brook into proc->cabin.
 *
 * On success:
 *   *out_user_va_header  = VA of the BrookHeader page in the cabin
 *   *out_user_va_slots   = VA of the slot region in the cabin
 *   *out_frame_size      = effective frame_size (immutable; existing wins)
 *   *out_frame_count     = effective frame_count (immutable; existing wins)
 *
 * flags MUST contain exactly one of {BROOK_WRITER, BROOK_READER}. Pass
 * BROOK_CREATE to create the Brook if its tag isn't already bound. On
 * CREATE, frame_size and frame_count MUST be non-zero, frame_size in
 * [BROOK_FRAME_SIZE_MIN..MAX], frame_count in [MIN..MAX] AND power-of-2.
 *
 * Errors:
 *   ERR_INVALID_ARGUMENT — bad flag combo, bad bounds, NULL inputs
 *   ERR_TAG_NOT_FOUND    — open without CREATE on unbound tag
 *   ERR_BUSY             — second open with the same role
 *   ERR_ALREADY_EXISTS   — CREATE with different frame_size/count than
 *                          the existing Brook (semantics: stream shape
 *                          is part of the tag identity in BoxOS)
 *   ERR_NO_MEMORY        — PMM/kmalloc/VA failure
 *   ERR_INVALID_STATE    — proc->destroying mid-open
 * ───────────────────────────────────────────────────────────────────── */
error_t BrookOpenInternal(struct process_t *proc,
                          const char *tag,
                          uint32_t frame_size,
                          uint32_t frame_count,
                          uint32_t flags,
                          uint64_t *out_user_va_header,
                          uint64_t *out_user_va_slots,
                          uint32_t *out_frame_size,
                          uint32_t *out_frame_count);

/* Release this cabin's claim. Drops the matching role's pid + the
 * BrookObject ref. If ref hits zero the Brook is destroyed (chunks →
 * PMM). The surviving peer (if any) sees its peer_alive flag go to 0
 * on the next wait-loop iteration and returns the appropriate error. */
error_t BrookReleaseInternal(struct process_t *proc,
                             uint64_t user_va_header);

/* Process teardown hook. MUST run from process_destroy BEFORE
 * vmm_destroy_context. Drains proc->brook_claims_head, drops each
 * role, flips header.*_alive=0 so peer notices, returns chunks if
 * both peers are gone. */
void BrookCleanupProcess(struct process_t *proc);

/* Diagnostic snapshot.
 *   out[0] = live BrookObject count
 *   out[1] = active claims across all cabins
 *   out[2] = total backing pages (4 KiB units)
 *   out[3] = aggregate release events since boot */
void BrookStatsSnapshot(uint64_t out[4]);

#endif /* BROOK_H */
