#include "box/notify.h"
#include "box/system.h"

void yield(void) {
    // Submit an empty pocket with yield flag set
    Pocket p;
    pocket_prepare(&p);
    p.flags = 0x80;  // YIELD flag

    PocketRing* ring = pocket_ring();
    pocket_ring_push(ring, &p);

    // notify — yield hint to scheduler
    __asm__ volatile("syscall" ::: "memory", "rcx", "r11");
}
