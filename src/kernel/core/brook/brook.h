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
 * When one side releases, or the STRAND holding it dies, the kernel
 * flips the matching `*_alive` flag to 0 in the shared header. The
 * strand is the unit here and not the cabin: a stream ends when its
 * named occupant leaves, even if the flat it printed from goes on
 * living. The surviving peer's wait-loop re-reads it on every
 * iteration:
 *   - writer: full ring + reader_alive==0 + reader_ever_attached==1
 *             → -ERR_PROCESS_TERMINATED
 *   - reader: empty ring + writer_alive==0 + writer_ever_attached==1
 *             → -ERR_STREAM_CLOSED
 *   - reader: frames still in ring after writer left → drain normally;
 *             EOF only on the empty re-check that follows.
 *
 * The `*_ever_attached` history bits prevent a reader that opens before
 * any writer from returning EOF on its first pop. Set on first attach,
 * never cleared.
 *
 * Two lifetimes, two owners
 * -------------------------
 * A Brook claim carries two different things and they do NOT end at the
 * same moment.
 *
 *   The ROLE — the writer's or the reader's seat on a globally
 *   tag-named, single-occupancy stream — belongs to the STRAND that
 *   took it. A strand that finishes printing and exits MUST vacate its
 *   seat: the writer-leave terminal is the only signal that ENDS a
 *   session, and a seat left warm by a dead strand poisons the tag for
 *   whoever comes next.
 *
 *   The MAPPING — the header page and the slot region, in the cabin's
 *   address space — belongs to the CABIN. Every sibling strand runs in
 *   that address space and can already hold a pointer into it, so
 *   tearing it down because one strand finished pulls the floor out
 *   from under siblings that did nothing wrong.
 *
 * The room is rented to the flat, but one named occupant is sitting at
 * the table and the lease names them. When the occupant leaves the
 * lease ends — the flat does not lose the room.
 *
 * Bay's cure (make the whole claim the cabin's, drop the opener
 * entirely) was rejected here for exactly that reason: a Bay window
 * carries only memory, so a stale one costs pages; a Brook claim
 * carries memory AND a seat.
 *
 * Lifecycle
 * ---------
 * BrookObject.ref_count counts MAPPINGS — one per live BrookClaim,
 * across every cabin — and never roles. That is the whole difficulty of
 * splitting the two lifetimes: a refcount that counted seats would
 * reach zero the moment both strands exited and free the ring out from
 * under cabins that still have its pages in their page tables. When the
 * last claim anywhere drops, the BrookObject unlinks from its bucket
 * and the backing chunks return to PMM.
 *
 *   INVARIANT: a role is only ever held by a claim that is still on a
 *   cabin's list, and every such claim holds exactly one mapping
 *   reference. So ref_count == 0 proves both that no cabin maps these
 *   pages and that no strand holds a seat on them — which is why the
 *   role paths below always vacate the seat BEFORE dropping the
 *   mapping reference that keeps the BrookObject addressable.
 *
 * Two hooks, one per lifetime. BrookCleanupStrand runs from
 * process_destroy and ends only the ROLES the dying strand held — its
 * peer is notified within one wait-cycle and the mapping is left
 * standing. BrookCleanupCabin runs from cabin_destroy, when the last
 * strand is already reaped, and takes down every mapping the cabin
 * still holds, so a crashing app never leaks backing pages.
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
 * single-session: reader returns ERR_STREAM_CLOSED on writer-leave, writer
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
struct cabin_t;

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
 *     - writer_bell [a sleeping writer's pid; the reader takes it and rings]
 *     - frame_size, frame_count, magic [immutable]
 *
 *   Cacheline 1 (reader-state) — watched by writer:
 *     - head [reader writes; writer reads]
 *     - reader_alive [kernel writes]
 *     - reader_ever_attached [kernel writes]
 *     - reader_bell [a sleeping reader's pid; the writer takes it and rings]
 *
 * head/tail are monotonic uint64 counters; modulo against frame_count
 * (power-of-2) gives the slot index.
 *
 * Userspace EOF/PROCESS_TERMINATED semantics:
 *   alive==0 && ever_attached==1 → peer left; emit terminal error.
 *   alive==0 && ever_attached==0 → no peer YET; keep waiting (lets a
 *                                  reader open before any writer attaches).
 * ───────────────────────────────────────────────────────────────────── */
/* Taking a bell MARKS it; it does not erase it. The peer that rings sets this
 * bit, and only the sleeper itself ever writes the word back to zero.
 *
 * The first design had the ringer CAS the bell to 0, which is the obvious way
 * to make exactly one peer pay — and it destroyed the only evidence that the
 * strand was asleep in a stream at all. A ring that is taken and then LOST
 * left a header indistinguishable from a healthy one, so Nightwatch went blind
 * on precisely the failure the bell exists to make visible. Found by staging
 * that failure: the watch said nothing for thirty seconds over a wedged
 * reader.
 *
 * With the mark, the word answers two questions instead of one — is this
 * strand asleep in a brook (non-zero), and did anybody already ring for it
 * (bit set) — and neither answer can be erased by the peer. */
#define BROOK_BELL_RUNG   0x80000000u
#define BROOK_BELL_PID(v) ((v) & ~BROOK_BELL_RUNG)

typedef struct {
    /* Cacheline 0 (64 B) — writer state, watched by reader */
    volatile uint64_t tail;                  /* writer writes; reader reads */
    volatile uint32_t writer_alive;
    volatile uint32_t writer_ever_attached;
    uint32_t          frame_size;            /* immutable after create */
    uint32_t          frame_count;           /* immutable, power-of-2 */
    uint64_t          magic;                 /* BROOK_HEADER_MAGIC */
    volatile uint32_t writer_bell;           /* see the bells, below */
    uint8_t           _pad_line0[28];

    /* Cacheline 1 (64 B) — reader state, watched by writer */
    volatile uint64_t head;                  /* reader writes; writer reads */
    volatile uint32_t reader_alive;
    volatile uint32_t reader_ever_attached;
    /* THE BELLS — how a side that has gone to sleep can still be reached.
     *
     * A Brook is the one delivery surface in this system the kernel never
     * sees: a push and a pop are stores into a shared page, so nothing exists
     * to wake a peer that has stopped looking. A bell is the sleeper's own
     * pid, left where the other side already looks, and it is symmetric
     * because the waiting is:
     *
     *   reader_bell — a READER with an empty stream hangs it here, on the line
     *   a writer already reads for `head` on every push. The writer takes it
     *   (CAS to 0, so exactly one writer pays) and rings.
     *
     *   writer_bell — a WRITER with a full stream hangs it on CL0, on the line
     *   a reader already reads for `tail` on every pop. The reader rings it
     *   after freeing a slot.
     *
     * Each bell lives in ITS OWN side's cacheline, which is what makes reading
     * it free: the peer has that line in cache already for the cursor. Ringing
     * costs one message and happens once per SLEEP, never per frame.
     *
     * Hanging and taking are Dekker's two halves with the peer's cursor
     * publish — see brook_bell_hang() in boxlib brook.c for the fence
     * argument. A stale pid (peer gone, seat re-let) rings a stranger's
     * doorbell once and costs it a discarded empty message.
     *
     * The kernel rings the survivor's bell when a peer DEPARTS (BrookRingPeer):
     * a sleeper waiting for a stream whose other end has gone must be told, and
     * the departure is a store to `*_alive` that no sleeping strand will see. */
    volatile uint32_t reader_bell;
    uint8_t           _pad_line1[44];
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
_Static_assert(__builtin_offsetof(BrookHeader, reader_bell) >= 64,
               "reader_bell must share CL1 with head — the line the writer already reads");
_Static_assert(__builtin_offsetof(BrookHeader, writer_bell) < 64,
               "writer_bell must share CL0 with tail — the line the reader already reads");

/* ─────────────────────────────────────────────────────────────────────
 * BrookObject — kernel-side per-tag record. Identified by tag_id.
 * Hash-bucketed (see brook.c).
 *
 * frame_size, frame_count, slot allocation are immutable after create.
 * The bucket lock serialises writer_pid/reader_pid/ref_count mutations
 * and the create-or-attach race.
 *
 * writer_pid / reader_pid name the STRAND sitting in each seat, and they
 * move with the seat: BrookHandOver re-stamps one of them, the header
 * bell that names the same sleeper, and the claim's `holder`, so the
 * three can never drift apart.
 * ───────────────────────────────────────────────────────────────────── */
struct BrookObject {
    BrookObject  *bucket_next;         /* hash-bucket collision chain */

    uint32_t      tag_id;              /* TagFS-interned id */
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
    /* One per live BrookClaim — a MAPPING count, not a seat count. See the
     * lifecycle note at the top of this file for why the distinction is the
     * difference between a freed page and a use-after-free. */
    uint32_t      ref_count;
    uint32_t      _pad1;
};

/* ─────────────────────────────────────────────────────────────────────
 * BrookClaim — one per cabin-open of a Brook. Lives in two places:
 *   - bucket-side: BrookObject->{writer,reader}_pid + ref_count
 *   - cabin-side: cabin->brook_claims_head singly-linked list
 *
 * user_va_header identifies the claim for release ops (it is unique
 * per Brook open within a cabin and easy to derive from the boxlib
 * opaque handle).
 *
 * `holder` is the pid of the strand whose hand is on the seat right now
 * — not the strand that opened it, which is a different question the
 * moment BrookHandOver moves the seat to a sibling. It is guarded by
 * cabin->brook_lock, the same lock that guards the list this row sits
 * on, and it does two jobs:
 *
 *   it PINS the row. While it is non-zero no sibling may release this
 *   claim, which is what lets the seat-vacating paths drop brook_lock,
 *   take a bucket lock, and still find the row (and its BrookObject)
 *   where they left it — with no lock nested inside another.
 *
 *   it ANSWERS "is this claim's seat still live". A bare pid cannot:
 *   after a seat is vacated the same seat may be taken by a different
 *   claim in a different cabin, so comparing against the BrookObject's
 *   role pid would call a long-dead claim the holder. holder == 0 says
 *   the seat is over and the mapping is the cabin's alone — any strand
 *   of the cabin may then hand it back.
 *
 * The rejected alternative was a back-pointer to the opening strand,
 * which is what used to stand here. Nothing else in this file consulted
 * it (release already matched by VA alone), and strand death used it to
 * splice claims out and clear PTEs in an address space the dying
 * strand's SIBLINGS were still executing in.
 * ───────────────────────────────────────────────────────────────────── */
struct BrookClaim {
    BrookObject  *brook;
    BrookClaim   *cabin_next;          /* cabin claim list link */
    uint64_t      user_va_header;      /* mapped BrookHeader page VA */
    uint64_t      user_va_slots;       /* mapped slot region VA */
    uint32_t      role;                /* BROOK_WRITER or BROOK_READER */
    uint32_t      flags;
    uint32_t      holder;              /* strand pid in the seat; 0 = vacated */
    uint32_t      _pad;
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

/* Hand this cabin's claim over to a sibling strand: the seat moves, the
 * mapping does not (it was never the strand's to move). The BrookObject
 * role pid, the header bell that names the same sleeper, and the
 * claim's holder are re-stamped as one act, so the three cannot drift
 * apart — the defect where a sibling hung its own pid on a bell while
 * the seat still named the opener.
 *
 * Only the current holder may hand a seat over, and only to a strand of
 * the SAME cabin: the mapping the seat sits on exists in this cabin's
 * address space and nowhere else. Making one in another cabin is a
 * different operation with a different failure set (VA reservation,
 * page-table work, MemTag capability) — it is brook_open, and this op
 * refuses rather than pretending to be it.
 *
 * Errors:
 *   ERR_INVALID_ARGUMENT    — NULL proc/cabin, zero VA, to_pid == 0, or
 *                             to_pid == the caller (a seat cannot be
 *                             handed to the strand already in it)
 *   ERR_TAG_NOT_FOUND       — no claim at that VA in this cabin
 *   ERR_PERMISSION_DENIED   — the caller is not the holder
 *   ERR_PROCESS_NOT_FOUND   — to_pid names no live strand
 *   ERR_PROCESS_DESTROYING  — the target is being torn down
 *   ERR_NOT_MAPPED          — the target is in another cabin, where this
 *                             Brook has no mapping at all
 *   ERR_INVALID_STATE       — the seat was vacated underneath us */
error_t BrookHandOver(struct process_t *from,
                      uint64_t user_va_header,
                      uint32_t to_pid);

/* Release this cabin's claim: the mapping goes AND, if it is still
 * seated, the role. Drops the BrookObject's mapping ref; if that ref
 * hits zero the Brook is destroyed (chunks → PMM). The surviving peer
 * (if any) sees its peer_alive flag go to 0 on the next wait-loop
 * iteration and returns the appropriate error.
 *
 * A strand that is not the holder is REFUSED. Bay deliberately went the
 * other way — any strand of the cabin may hand back any of the cabin's
 * windows — and that was right for Bay, whose claim carries only
 * memory. Here the same rule would unmap the pages and leave the seat
 * stamped on a BrookObject with no claim left to vacate it: a role slot
 * lost for the lifetime of the tag. Once the seat IS vacated
 * (holder == 0) the claim is nothing but the cabin's mapping and any
 * strand of the cabin may release it, exactly as in Bay. */
error_t BrookReleaseInternal(struct process_t *proc,
                             uint64_t user_va_header);

/* Strand teardown hook — the ROLE half. Runs from process_destroy and
 * vacates every seat this strand still holds: header.*_alive → 0 so the
 * peer notices within one wait-cycle, the role pid cleared, the
 * survivor's bell rung. It unmaps NOTHING and drops no reference: the
 * mapping lives in an address space this strand's siblings are still
 * executing in.
 *
 * "Strand", not "Process", because process_t IS a strand here and
 * "Process" is now the word that misleads. */
void BrookCleanupStrand(struct process_t *proc);

/* Cabin teardown hook — the MAPPING half, and the only automatic one.
 * MUST be called from cabin_destroy BEFORE vmm_destroy_context:
 * otherwise the destroy walk frees Brook-owned ring pages as if they
 * were the cabin's own exclusive pages, while the peer cabin still has
 * them mapped. Takes down every claim the cabin still holds; a seat
 * found still warm (a strand reaped without the hook above running) is
 * vacated first, because the BrookObject must not be freed with a live
 * role stamped on it. */
void BrookCleanupCabin(struct cabin_t *cabin);

/* Diagnostic snapshot.
 *   out[0] = live BrookObject count
 *   out[1] = active claims across all cabins
 *   out[2] = total backing pages (4 KiB units)
 *   out[3] = aggregate release events since boot */
void BrookStatsSnapshot(uint64_t out[4]);

/* Nightwatch's eye on the one delivery surface the kernel never witnesses.
 *
 * A Brook push is a store into a shared page: no syscall, no record, nothing
 * for the watch to find. So a reader asleep on a frame it was never told about
 * is invisible to every proof the verdict otherwise has — both its rings are
 * quiet, its pocket ring is empty, and by every kernel measure it is simply a
 * strand with nothing to do. The bell is what makes it visible: a reader hangs
 * its pid there before going down, and a writer that publishes a frame is
 * supposed to take it and ring. A brook found with a bell still hung out AND
 * frames unread is exactly the ring that never happened.
 *
 * Reports the first such brook read by `reader_pid`, or false if there is none.
 * Takes bucket locks, so it must not be called from interrupt context. */
bool BrookBellUnrung(uint32_t reader_pid, uint32_t *out_tag_id,
                     uint64_t *out_head, uint64_t *out_tail);

#endif /* BROOK_H */
