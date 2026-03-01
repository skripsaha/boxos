#include "box/notify.h"
#include "box/system.h"

void yield(void) {
    notify_page_t* np = notify_page();

    np->magic = NOTIFY_MAGIC;
    np->prefix_count = 0;
    np->flags = NOTIFY_FLAG_YIELD;
    np->status = 0;

    __asm__ volatile("int $0x80");

    np->magic = 0;
}
