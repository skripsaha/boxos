#include "box/notify.h"
#include "box/system.h"

void yield(void) {
    // Submit an empty pocket with yield flag set
    Pocket p;
    pocket_prepare(&p);
    p.flags = 0x80;  // YIELD flag

    PocketRing* ring = pocket_ring();
    pocket_ring_push(ring, &p);

    __asm__ volatile("int $0x80" ::: "memory");
}
