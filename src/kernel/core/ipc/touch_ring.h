#ifndef TOUCH_RING_H
#define TOUCH_RING_H

#include "ktypes.h"
#include "cabin_layout.h"

/*
 * TouchRing — per-cabin kernel→userspace channel for Touch events.
 *
 * Why a dedicated ring (and not ResultRing multiplexing):
 *
 *   - Touch is a tag-multicast event stream, semantically distinct from
 *     syscall replies and IPC payload routing (which live on ResultRing).
 *   - Multiplexing forced consumer-side fan-out logic and a per-cabin
 *     `buf_heap_next` allocation for Touch payloads — that allocator
 *     monotonically grew and silently leaked physical RAM at the publish
 *     rate for the lifetime of every long-running daemon.
 *   - Owning the channel lets us co-locate the payload with the slot's
 *     Vyukov generation counter: producer writes both atomically with
 *     respect to consumer reads, lifetime is the slot's round, and no
 *     separate allocator is needed.
 *   - A Touch-only consumer (e.g. keyboard listener in display.elf)
 *     can UMWAIT on `TouchRing.hdr.tail` directly without polling the
 *     syscall-reply ring.
 *
 * Layout
 * ------
 * One 4 KiB header page at CABIN_TOUCH_RING_ADDR + a 1 MiB lazy-mapped
 * slot region at CABIN_TOUCH_SLOTS_BASE. Header carries head/tail
 * cursors on SEPARATE cachelines — Intel SDM Vol 3 §11.5 (Cache
 * Control Protocol — MESI line-state transitions) + Intel® 64
 * Optimization Reference Manual, Cache & Memory Subsystem chapter
 * (false sharing & RFO storms). Cache line is 64 B on every shipping
 * Intel/AMD x86_64 part; verified at runtime via CPUID.05H monitor-
 * line probe (cpuid.c). The same fix landed for the deferred-work ring of
 * the day in commit `3cdc79c`.
 *
 * Producer model — MPSC
 * ---------------------
 * Multiple K-Cores can land in KTouchPush concurrently for the same
 * target (the IRQ ring's reader on the drain core — keyboard, xHCI, ACPI,
 * ATA errors — plus synchronous storage_ops paths — all racing for one
 * cabin's ring).
 *
 * Producer side uses a Vyukov-style per-slot generation counter (slot.seq)
 * that gates BOTH the slot's metadata header AND its inline payload:
 *
 *   seq == 2*round           — slot available for `round` producer
 *   seq == 2*round + 1       — producer published, consumer may read
 *   seq == 2*(round + 1)     — consumer released, available for next round
 *
 * Initial state: all seq fields are zero (pages are pmm_alloc_zero'd on
 * first touch). Round-0 producers see seq == 0 == 2*0 and proceed
 * immediately, preserving the lazy-mapping invariant — no eager init.
 *
 * Consumer model — SP
 * -------------------
 * The cabin owner (single userspace thread) reads slot[head%cap],
 * gates on seq == 2*round+1, copies the entire 128-byte slot to its
 * stack, releases seq = 2*(round+1), advances head. Payload lifetime
 * is the slot's round: copying before release is mandatory.
 */

typedef struct __packed {
    /* Cacheline 0 (64 B): consumer cursor + read-only init metadata.
     *
     * Userspace consumer writes `head` here. Kernel producer only
     * reads `head` (to gate against overflow); the metadata fields
     * are written once during init and read-only afterward. Co-locating
     * read-only metadata with the consumer-owned cursor minimises
     * lines the producer needs to load. */
    volatile uint64_t head;
    uint64_t          slots_base;
    uint32_t          slot_size;
    uint32_t          slot_count_max;
    uint64_t          magic;
    uint8_t           _pad_line0[32];     /* fill cacheline 0 */

    /* Cacheline 1 (64 B): producer cursor — own cacheline.
     *
     * Multiple K-Cores hammer `tail` via __atomic_fetch_add. Without
     * separation, the userspace consumer's load of `head` on a
     * different core would invalidate the producer's cacheline (and
     * vice versa), turning every push into an RFO storm on real
     * 16-core hardware. */
    volatile uint64_t tail;
    /* Events the kernel accepted for this ring but could not fit in it —
     * the Owed queue's depth (touch.c). Written by the kernel under the
     * owner's owed_lock, read-only for userspace.
     *
     * It sits HERE, on the producer cacheline, because that is the line the
     * consumer already watches: touch_wait_umwait arms UMONITOR on &tail and
     * then UMWAITs with no deadline. A ring that is full has a tail that will
     * not move, so a consumer that drained its slots and went back to sleep
     * would never learn that the kernel is still holding its events. Writing
     * this counter writes that line, so the slip wakes the sleeper and tells
     * it what the ring cannot: come to the door (yield) and the kernel will
     * hand the rest over. */
    volatile uint64_t owed;
    uint8_t           _pad_line1[48];     /* fill cacheline 1 */
} TouchRingHeader;

_Static_assert(sizeof(TouchRingHeader) == 128,
               "TouchRingHeader must be exactly 128 bytes (two cachelines)");
_Static_assert(__builtin_offsetof(TouchRingHeader, head) == 0,
               "TouchRingHeader.head must start at offset 0");
_Static_assert(__builtin_offsetof(TouchRingHeader, tail) == 64,
               "TouchRingHeader.tail must start at cacheline 1 (offset 64)");
_Static_assert(__builtin_offsetof(TouchRingHeader, owed) == 72,
               "TouchRingHeader.owed must share cacheline 1 with tail — the "
               "consumer's UMONITOR watches that line and nothing else");

/* Per-slot envelope: metadata + inline payload + Vyukov gate.
 *
 * Total stride 128 B. Inline payload region eliminates the need for a
 * separate per-payload allocation (which was the source of the
 * buf_heap_next leak in the multiplexed design). Payload is bounded
 * at BOXOS_TOUCH_PAYLOAD_MAX (96 B) — see cabin_layout.h for sizing
 * rationale and the "use Pocket IPC for bulk data" guidance. */
typedef struct __packed {
    uint16_t tag_id;                                  /* TouchTag (matches uint16_t typedef in touch.h) */
    uint16_t flags;                                   /* TOUCH_FLAG_KERNEL/USER/TAGFS */
    uint32_t source_pid;                              /* 0 = kernel publisher, else originating pid */
    uint32_t payload_len;                             /* bytes valid in payload[] (≤ BOXOS_TOUCH_PAYLOAD_MAX) */
    uint32_t _reserved;                               /* alignment + future use */
    uint64_t timestamp_tsc;                           /* TSC at publish, monotonic per core */
    uint8_t  payload[BOXOS_TOUCH_PAYLOAD_MAX];        /* inline payload — Vyukov-gated alongside metadata */
    uint64_t seq;                                     /* Vyukov generation counter (see file header) */
} TouchSlot;

_Static_assert(sizeof(TouchSlot) == 128,
               "TouchSlot must be exactly 128 bytes (TOUCH_SLOT_SIZE)");
_Static_assert(sizeof(TouchSlot) == TOUCH_SLOT_SIZE,
               "TouchSlot stride mismatch with cabin_layout.h TOUCH_SLOT_SIZE");
_Static_assert((TOUCH_SLOT_SIZE & (TOUCH_SLOT_SIZE - 1)) == 0,
               "TOUCH_SLOT_SIZE must be power-of-2 for clean modulo");
/* Straddle-safety geometry (Crate-boundary straddle hardening 7/7):
 * touch_ring_slot_uvaddr translates exactly sizeof(TouchSlot) at
 * slots_base + (idx % cap) * TOUCH_SLOT_SIZE. The stride divides the page and
 * the slot base is page-aligned, so a slot never crosses a page boundary (the
 * runtime per-strand base is checked in KTouchRingInitAt). */
_Static_assert(4096 % TOUCH_SLOT_SIZE == 0,
               "TOUCH_SLOT_SIZE must divide a 4 KiB page");
_Static_assert((CABIN_TOUCH_SLOTS_BASE & 0xFFFULL) == 0,
               "CABIN_TOUCH_SLOTS_BASE must be page-aligned");
_Static_assert(__builtin_offsetof(TouchSlot, seq) ==
               TOUCH_SLOT_SIZE - sizeof(uint64_t),
               "TouchSlot.seq must be the LAST field (consumer reads metadata + payload first)");

/* Full ring header page — header + zero padding. We allocate one physical
 * page; only the first sizeof(TouchRingHeader) bytes carry meaning. */
typedef struct __packed {
    TouchRingHeader hdr;
    uint8_t         _page_pad[4096 - sizeof(TouchRingHeader)];
} TouchRing;

_Static_assert(sizeof(TouchRing) == 4096,
               "TouchRing header page must be exactly one page");

/* Inline accessors — usable from both kernel and userspace (boxlib mirror
 * uses the same layout). All cursor reads should go through __atomic_load_n
 * with appropriate ordering for the role. */

static inline bool touch_ring_is_empty(const TouchRing *r)
{
    return r->hdr.head == r->hdr.tail;
}

static inline bool touch_ring_is_full(const TouchRing *r)
{
    return (r->hdr.tail - r->hdr.head) >= r->hdr.slot_count_max;
}

static inline uint32_t touch_ring_count(const TouchRing *r)
{
    uint64_t n = r->hdr.tail - r->hdr.head;
    return n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)n;
}

static inline uintptr_t touch_ring_slot_uvaddr(const TouchRing *r, uint64_t idx)
{
    return (uintptr_t)(r->hdr.slots_base
                       + (idx % r->hdr.slot_count_max) * r->hdr.slot_size);
}

/* Forward decls for kernel-side helpers (implemented in touch_ring.c). */
typedef struct process_t process_t;

/* Initialise a freshly-allocated ring header page. The caller owns the
 * physical page; this writes head=tail=0, slots_base, slot_size, magic.
 * The plain form uses the fixed cabin slot region + full capacity (the main
 * strand's ring); KTouchRingInitAt takes an explicit per-strand slot-region
 * VA and capacity (a Hammock-carved spawned-strand ring — strand_rings.c). */
void KTouchRingInit(TouchRing *hdr);
void KTouchRingInitAt(TouchRing *hdr, uint64_t slots_base, uint32_t slot_count_max);

/* MPSC producer entry point. Publishes a Touch event into `target`'s
 * TouchRing. The slot page is lazily mapped on demand (vmm_ensure_user_page
 * is idempotent). Returns true on successful publish, false on full ring,
 * destroying target, or transient mapping failure. Payload is truncated
 * silently at BOXOS_TOUCH_PAYLOAD_MAX bytes (caller has already been
 * advised by the API contract to keep events small). */
bool KTouchPush(process_t *target,
                uint16_t tag_id, uint16_t flags, uint32_t source_pid,
                const void *payload, uint32_t payload_len);

/* True when the slot at the ring's head is published and unconsumed. Turn In
 * refuses a sleep across it — see KResultRingHasUnreadAtHead (kring.h). */
bool KTouchRingHasUnreadAtHead(process_t *proc);

/* Diagnostic: snapshot per-return-path counters.
 *   out[0] = null/no-hdr/zero-cap rejections
 *   out[1] = refused because the ring was full (the event becomes Owed)
 *   out[2] = page-map failures
 *   out[3] = translate failures
 *   out[4] = publishes that had to retry the claim (producer contention)
 *   out[5] = claims whose slot was not released — a consumer reporting a head
 *            it has not reached; its own stream is what suffers
 *   out[6] = successful publishes */
void KTouchPushStats(uint64_t out[7]);

#endif /* TOUCH_RING_H */
