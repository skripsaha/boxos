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
     * kernel, which drains it whole — so a full ring is waited out, never
     * refused. A refusal here was every caller's silent failure at once:
     * a print that never printed, a Brook release that never released and
     * a lane number reused on top of its living claim (BIOS 16c,
     * 2026-09-06). The wait gives the core away, and every pass through the
     * gate with pockets waiting makes sure a K-Core has been told. A push
     * that fails while the ring is NOT full is a broken header, and that is
     * the one thing still reported. */
    while (!pocket_ring_push(ring, p)) {
        if (!pocket_ring_is_full(ring)) return -1;
        yield();
    }

    __notify();
    return 0;
}

void yield(void) {
    __asm__ volatile("syscall" : : "D"(GATE_YIELD) : "memory", "rax", "rcx", "r11");
}
