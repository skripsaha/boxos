#include "box/notify.h"
#include "box/system.h"

void yield(void) {
    notify_page_t* np = notify_page();

    np->magic = BOX_NOTIFY_MAGIC;
    np->prefix_count = 0;
    np->flags = BOX_NOTIFY_FLAG_YIELD;
    np->status = 0;

    __asm__ volatile("int $0x80");

    // Invalidate so next chain builder triggers fresh notify_prepare()
    // (clears flags, prefix_count, etc.)
    np->magic = 0;
}
