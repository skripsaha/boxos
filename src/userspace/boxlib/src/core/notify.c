#include "box/core/notify.h"
#include "box/core/result.h"
#include "box/string.h"

void pocket_prepare(Pocket* p) {
    memset(p, 0, sizeof(Pocket));
}

int pocket_submit(Pocket* p) {
    if (!p) return -1;

    PocketRing* ring = pocket_ring();
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