#include "box/notify.h"
#include "box/system.h"

void yield(void) {
    box_notify_page_t* np = box_notify_page();

    np->magic = BOX_NOTIFY_MAGIC;
    np->prefix_count = 2;
    np->prefixes[0] = (0xFF << 8) | 0x00;
    np->prefixes[1] = (0xFE << 8) | 0x00;
    np->prefixes[2] = PREFIX_TERMINATOR;
    np->flags = 0;
    np->status = 0;

    __asm__ volatile("int $0x80");
}
