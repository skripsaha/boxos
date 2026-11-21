#include "box/core/notify.h"
#include "box/core/result.h"
#include "box/string.h"

void pocket_prepare(Pocket* p) {
    memset(p, 0, sizeof(Pocket));
}

int pocket_submit(Pocket* p) {
    if (!p) return -1;

    PocketRing* ring = pocket_ring();
    /* Room in the ring is guaranteed to come — its one consumer is the
     * kernel, which serves every pocket in it, one a turn — so a full ring is
     * waited out, never refused. A refusal here was every caller's silent
     * failure at once: a print that never printed, a Brook release that never
     * released and a lane number reused on top of its living claim (BIOS 16c,
     * 2026-09-06). The wait gives the core away, and every pass through the
     * gate with pockets waiting makes sure a K-Core has been told. A push
     * that fails while the ring is NOT full is a broken header, and that is
     * the one thing still reported. */
    while (!pocket_ring_push(ring, p)) {
        if (!pocket_ring_is_full(ring)) return -1;
        yield();
    }

    /* A ring in the kernel's hand is looked at once more before it is let
     * go, so the gate is rung only for one that is not. The push, a full
     * barrier and this look pair with the K-Core's letting go, barrier and
     * look at the ring (kcore.c): one of the two sees the other — the pocket
     * is served without a word, or the gate is rung. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!ring->hdr.in_hand)
        __notify();
    return 0;
}

void yield(void) {
    __asm__ volatile("syscall" : : "D"(GATE_YIELD) : "memory", "rax", "rcx", "r11");
}
