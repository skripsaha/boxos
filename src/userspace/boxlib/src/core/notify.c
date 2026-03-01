#include "box/notify.h"
#include "box/string.h"

void notify_prepare(void) {
    notify_page_t* np = notify_page();
    uint32_t saved_parent = np->parent_pid;
    memset(np, 0, sizeof(notify_page_t));
    np->magic = BOX_NOTIFY_MAGIC;
    np->parent_pid = saved_parent;
}

bool notify_add_prefix(uint8_t deck_id, uint8_t opcode) {
    notify_page_t* np = notify_page();
    if (np->prefix_count >= BOX_MAX_PREFIXES) {
        return false;
    }
    np->prefixes[np->prefix_count++] = BOX_PREFIX(deck_id, opcode);
    return true;
}

size_t notify_write_data(const void* data, size_t size) {
    notify_page_t* np = notify_page();

    // Kernel copies only BOX_INLINE_DATA_SIZE (256) bytes
    size_t to_write = (size > BOX_INLINE_DATA_SIZE)
                      ? BOX_INLINE_DATA_SIZE
                      : size;

    memcpy(np->data, data, to_write);
    return to_write;
}

event_id_t notify(void) {
    uint64_t result;
    __asm__ volatile(
        "int $0x80"
        : "=a"(result)
        :
        : "memory"
    );
    // Invalidate page so next chain builder triggers auto-prepare
    notify_page()->magic = 0;
    return (event_id_t)result;
}

void notify_data(const void* data, size_t size) {
    notify_page_t* np = notify_page();
    if (np->magic != BOX_NOTIFY_MAGIC) {
        notify_prepare();
    }
    notify_write_data(data, size);
}
