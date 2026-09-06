#ifndef BOX_CORE_RESULT_H
#define BOX_CORE_RESULT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "box/error.h"
#include "boxos_kctx.h"      /* KResultContext enum — shared with kernel */
#include "box/core/strand_self.h"   /* strand_rings() — per-strand ring routing (P5a) */

// Result: syscall response from kernel to userspace.
// Data is NOT inline — data_addr points to cabin heap.
//
// `context` is the kernel-stamped KResultContext (KCTX_TOUCH, KCTX_IPC, ...)
// — boxlib's result_pop_* helpers fan out by this tag rather than by parsing
// the payload. Layout matches the kernel-internal `Result` in src/kernel/
// core/ipc/result.h exactly.
typedef struct PACKED {
    uint32_t error_code;
    uint32_t data_length;
    uint64_t data_addr;      // virtual address of data in cabin heap
    uint32_t sender_pid;     // 0 = kernel, != 0 = IPC sender
    uint32_t context;        // KResultContext: which subsystem published this
} Result;

STATIC_ASSERT(sizeof(Result) == 24, "Result must be 24 bytes");

/* ResultRing — Phase 11 lazy-growable, monotonic-index, MPSC kernel
 * producers / SP userspace consumer.
 *
 * Header at CABIN_RESULT_RING_ADDR (0x3000); slots at CABIN_RESULT_SLOTS_BASE,
 * lazily mapped by the kernel on first KResultPush touch.
 *
 * Each slot carries a Vyukov generation counter (slot.seq) used to gate
 * producer/consumer access:
 *   seq == 2*round           — slot empty, producer for `round` may write
 *   seq == 2*round + 1       — producer wrote, consumer for `round` may read
 *   seq == 2*(round + 1)     — consumer read, slot ready for next round
 * where round = pos / slot_count_max. Zero-init pages naturally satisfy
 * round-0 producers; no eager init is required.
 */
typedef struct PACKED {
    Result   r;                          /* 24 bytes payload */
    uint64_t seq;                        /* 8  bytes generation counter */
} ResultSlot;

STATIC_ASSERT(sizeof(ResultSlot) == 32, "ResultSlot must be 32 bytes");

/* ResultRingHeader — cacheline-separated cursors (mirror of kernel layout).
 *
 *   Cacheline 0 — consumer cursor (userspace) + read-only init metadata
 *   Cacheline 1 — producer reservation cursor (kernel MPSC) — own cacheline
 *
 * Kernel K-Cores hammer `tail` via __atomic_fetch_add on every push.
 * Without separation, the userspace consumer's load of `head` on a
 * different core would force RFO traffic with every push. Intel SDM
 * Vol 3 §11.4.4. Must stay BYTE-IDENTICAL to the kernel-side struct
 * in src/kernel/core/ipc/result_ring.h. */
typedef struct PACKED {
    /* Cacheline 0 — consumer cursor + read-only metadata. */
    volatile uint64_t head;             /* userspace cursor */
    uint64_t          slots_base;       /* user vaddr of slot 0 */
    uint32_t          slot_size;        /* sizeof(ResultSlot) == 32 */
    uint32_t          slot_count_max;
    uint64_t          magic;
    uint8_t           _pad_line0[32];   /* fill cacheline 0 */

    /* Cacheline 1 — producer reservation cursor (kernel MPSC). */
    volatile uint64_t tail;             /* kernel reservation cursor */
    /* The cloakroom token this strand is holding out for, 0 when none.
     * Written here by result_wait so the kernel's watch can tell a strand
     * that is working from one that is waiting for an answer nobody is going
     * to give — see the kernel's result_ring.h for why it cannot infer it. */
    volatile uint64_t awaiting;
    uint8_t           _pad_line1[48];   /* fill cacheline 1 */
} ResultRingHeader;

STATIC_ASSERT(sizeof(ResultRingHeader) == 128,
              "ResultRingHeader must be 128 bytes (two cachelines)");
STATIC_ASSERT(OFFSETOF(ResultRingHeader, awaiting) == 72,
              "ResultRingHeader.awaiting must sit at offset 72 (kernel agrees)");

typedef struct PACKED {
    ResultRingHeader hdr;
    uint8_t          _page_pad[4096 - sizeof(ResultRingHeader)];
} ResultRing;

STATIC_ASSERT(sizeof(ResultRing) == 4096, "ResultRing header must be one page");

INLINE ResultRing* result_ring(void) {
    return (ResultRing*)(uintptr_t)strand_rings().result_va;
}

INLINE bool result_ring_is_empty(const ResultRing* ring) {
    return ring->hdr.head == ring->hdr.tail;
}

bool result_available(void);
/* The slot at head is published and unconsumed — poppable now. Unlike
 * result_available (a claim past head), this is the fact a sleep may end on. */
bool result_published_at_head(void);
uint32_t result_count(void);
bool result_pop(Result* out);

// Paired wait for a synchronous submit's reply. `expect_cookie` is the
// submit's cloakroom token (Pocket.cookie24, echoed by the kernel in the
// reply's context high bits — see boxos_kctx.h): only the Result carrying
// this token is returned; reply-kind entries with any other token are
// provable orphans of earlier calls and are dropped where they stand.
// Uses UMONITOR/UMWAIT on CPUs with WAITPKG; falls back to cooperative yield.
bool result_wait(Result* out, uint32_t expect_cookie, uint32_t timeout_ms);

// How many reply-kind Results were dropped as proven orphans of earlier,
// abandoned submits. Steady zero on a healthy machine; a rising count means
// calls are being abandoned somewhere.
uint64_t result_orphans_dropped(void);

bool result_pop_non_ipc(Result* out);
bool result_pop_ipc(Result* out);
uint32_t result_ipc_stash_count(void);

/* Drop orphan manifest replies left behind by timed-out callers. Call
 * BEFORE each fresh synchronous ManifestSubmit so the stash+ring carry
 * no stale `error_code` that would otherwise be returned for the new op. */
void result_drain_orphan_replies(void);

/* Pop a KCTX_TOUCH entry from stash/ring. Used by touch_await to
 * fast-path consume already-queued touches without paying for a
 * kernel manifest round-trip. */
bool result_pop_touch(Result* out);

// Block until ANY result arrives (no filtering).
// For IPC servers (display daemon, etc.) that receive both IPC and kernel results.
bool result_wait_any(Result* out, uint32_t timeout_ms);

// Block until an IPC message arrives (or timeout). Event-driven: UMWAIT on the
// ResultRing tail where WAITPKG exists (woken when a sender's KResultPush
// advances tail), pause/yield fallback otherwise. Backs receive_wait().
bool result_wait_ipc(Result* out, uint32_t timeout_ms);

/* Ф26e — box::ferry (async file I/O) completion channel. Storage completions
 * carry KCTX_STORAGE and are FULLY ISOLATED: every other ResultRing consumer
 * routes them out into a per-strand ferry stash, and they are returned ONLY by
 * result_pop_ferry / result_wait_ferry. result_restash routes one non-ferry
 * record back to its own consumer; result_ferry_stash_count backs the wait
 * loop's non-allocating readiness probe. Backs box::ferry (box/cxx/ferry.h). */
bool result_pop_ferry(Result* out);
bool result_wait_ferry(Result* out, uint32_t timeout_ms);
void result_restash(const Result* r);
uint32_t result_ferry_stash_count(void);

// Diagnostic: snapshot result_pop counters
//   out[0]=calls, out[1]=empty(head==tail), out[2]=seq_mismatch, out[3]=success
//   out[4]=last_seq_seen, out[5]=last_expected, out[6]=last_pos, out[7]=last_tail
void result_pop_stats(uint64_t out[8]);


#ifdef __cplusplus
}
#endif

#endif // BOX_RESULT_H
