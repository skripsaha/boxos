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
    /* Answers the kernel took for this ring but could not fit into it — the
     * Owed queue's depth, written by the kernel (kring.c) and read-only here.
     * It shares this cacheline with `tail` deliberately: the UMWAIT waits
     * below arm UMONITOR on &tail, and a full ring's tail does not move, so
     * the store that records the debt is what stirs the sleeper. The debt is
     * settled by this cabin's own next entry into the kernel — the syscall
     * gate hands it over — so nothing here has to act on the number. */
    volatile uint64_t owed;
    /* The kernel asking the strand that holds this core to yield it: another
     * strand waits in this core's runqueue. Written by the kernel (kring.c
     * KResultStepAside), read-only here. Every spin that waits on work ANOTHER
     * strand must do ends on it — that strand may be queued behind us on this
     * very core. */
    volatile uint64_t step_aside;
    uint8_t           _pad_line1[32];   /* fill cacheline 1 */

    /* Cacheline 2 — the stopwatch (a kernel built MEASURE=on; zero
     * otherwise): when the call answered last came to the gate, when a K-Core
     * took it and when it was answered, TSC. Written before that answer is
     * published, so it goes with the answer just taken. */
    volatile uint64_t call_gate;
    volatile uint64_t call_taken;
    volatile uint64_t call_answered;
    /* The strand's yields that gave its core to another, and the PID of the
     * one it went to last (UINT32_MAX the idle strand). */
    volatile uint32_t yields_given_away;
    volatile uint32_t yield_went_to;
    /* What the K-Core spent serving that answer, TSC cycles: pocket read,
     * Manifest staged, crates staged in, ops, crates written back, on to the
     * answer (0 where unknown). */
    volatile uint32_t call_part[6];
    uint8_t           _pad_line2[8];    /* fill cacheline 2 */

    /* Cacheline 3 — the strand this strand's yield gave its core to last, as
     * it stood then: its asks to sleep, those that laid it down, and those
     * refused for an arrival on its Touch ring, on its reply ring, or for
     * events the kernel held for it. */
    volatile uint32_t went_turnin_asked;
    volatile uint32_t went_turnin_parked;
    volatile uint32_t went_turnin_refused_touch;
    volatile uint32_t went_turnin_refused_result;
    volatile uint32_t went_turnin_refused_owed;
    uint32_t          _pad_word3;
    /* When the K-Core that took the call last woke: between the gate and the
     * taking, the call found it asleep. */
    volatile uint64_t call_woke;
    uint8_t           _pad_line3[32];   /* fill cacheline 3 */
} ResultRingHeader;

STATIC_ASSERT(sizeof(ResultRingHeader) == 256,
              "ResultRingHeader must be 256 bytes (four cachelines)");
STATIC_ASSERT(OFFSETOF(ResultRingHeader, call_gate) == 128,
              "ResultRingHeader.call_gate must sit at offset 128 (kernel agrees)");
STATIC_ASSERT(OFFSETOF(ResultRingHeader, awaiting) == 72,
              "ResultRingHeader.awaiting must sit at offset 72 (kernel agrees)");
STATIC_ASSERT(OFFSETOF(ResultRingHeader, owed) == 80,
              "ResultRingHeader.owed must sit at offset 80 (kernel agrees)");
STATIC_ASSERT(OFFSETOF(ResultRingHeader, step_aside) == 88,
              "ResultRingHeader.step_aside must sit at offset 88 (kernel agrees)");

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

/* The kernel is asking this strand to give its core up (see step_aside). */
INLINE bool result_step_aside(void) {
    ResultRing* rr = result_ring();
    return rr && __atomic_load_n(&rr->hdr.step_aside, __ATOMIC_ACQUIRE) != 0;
}

bool result_available(void);
/* The slot at head is published and unconsumed — poppable now. Unlike
 * result_available (a claim past head), this is the fact a sleep may end on. */
bool result_published_at_head(void);
uint32_t result_count(void);
bool result_pop(Result* out);
/* The record at the head of the ring, left where it is — what result_pop
 * would hand over next. */
bool result_peek(Result* out);

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
/* Any message — IPC or a kernel reply — without blocking; a ferry completion
 * met on the way goes to its station. For IPC servers and box::result_any. */
bool result_pop_any(Result* out);
/* A record already taken out of the ring, back to the stash of its consumer
 * (an IPC message held while waiting for something else). With no room in the
 * heap it is lost — the one such place — and that is said aloud. */
void result_restash(const Result* r);
uint32_t result_ipc_stash_count(void);
uint32_t result_non_ipc_stash_count(void);

/* A spawned strand's stashes (box/core/stash.h) back to the heap, at its
 * exit. Idempotent; a no-op on the main strand. */
void result_stash_free_self(void);

/* Drop orphan manifest replies left behind by timed-out callers. Call
 * BEFORE each fresh synchronous ManifestSubmit so the stash+ring carry
 * no stale `error_code` that would otherwise be returned for the new op. */
void result_drain_orphan_replies(void);

/* Pop a KCTX_TOUCH entry from stash/ring. Used by touch_await to
 * fast-path consume already-queued touches without paying for a
 * kernel manifest round-trip. */
bool result_pop_touch(Result* out);

// Spin until ANY result arrives (no filtering). For IPC servers (the display
// daemon and box::message) that receive both IPC and kernel results.
//
// It spins on PAUSE and nothing else: no WAITPKG arm, no yield, no park. With
// timeout_ms == 0 that is an unbounded wait that never re-enters the kernel —
// deliberate, and the reason is written out over result_wait_ipc_turnin in
// result.c. The name says "wait" where the body says "spin"; see the same note.
bool result_wait_any(Result* out, uint32_t timeout_ms);

// Block until an IPC message arrives (or timeout). Event-driven: UMWAIT on the
// ResultRing tail where WAITPKG exists (woken when a sender's KResultPush
// advances tail), pause/yield fallback otherwise. Backs receive_wait().
bool result_wait_ipc(Result* out, uint32_t timeout_ms);

/* Ф26e — box::ferry (async file I/O) completion channel. Storage completions
 * carry KCTX_STORAGE and are FULLY ISOLATED: every other ResultRing consumer
 * routes them out into a per-strand ferry stash, and they are returned ONLY by
 * result_pop_ferry / result_wait_ferry. result_ferry_stash_count backs the
 * wait loop's non-allocating readiness probe. Backs box::ferry
 * (box/cxx/ferry.h). */
bool result_pop_ferry(Result* out);
bool result_wait_ferry(Result* out, uint32_t timeout_ms);
uint32_t result_ferry_stash_count(void);

// Diagnostic: snapshot result_pop counters
//   out[0]=calls, out[1]=empty(head==tail), out[2]=seq_mismatch, out[3]=success
//   out[4]=last_seq_seen, out[5]=last_expected, out[6]=last_pos, out[7]=last_tail
void result_pop_stats(uint64_t out[8]);


#ifdef __cplusplus
}
#endif

#endif // BOX_RESULT_H
