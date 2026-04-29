#include "box/core/notify.h"
#include "box/core/result.h"
#include "box/string.h"

void pocket_prepare(Pocket* p) {
    memset(p, 0, sizeof(Pocket));
}

int pocket_submit(Pocket* p) {
    if (!p) return -1;

    PocketRing* ring = pocket_ring();
    if (!pocket_ring_push(ring, p)) {
        return -1;  // ring full
    }

    __notify();
    return 0;
}

void yield(void) {
    Pocket p;
    pocket_prepare(&p);
    p.flags = POCKET_FLAG_YIELD;
    PocketRing* ring = pocket_ring();
    pocket_ring_push(ring, &p);
    __notify();
}
