#include "box/notify.h"
#include "box/string.h"

void notify_prepare(void) {
    notify_page_t* np = notify_page();

    // Save fields that kernel wrote at process creation (ASLR, spawner)
    uint32_t saved_parent        = np->spawner_pid;
    uint64_t saved_heap_base     = np->cabin_heap_base;
    uint64_t saved_heap_max_size = np->cabin_heap_max_size;
    uint64_t saved_buf_heap_base = np->cabin_buf_heap_base;
    uint64_t saved_stack_top     = np->cabin_stack_top;

    memset(np, 0, sizeof(notify_page_t));

    np->magic              = NOTIFY_MAGIC;
    np->spawner_pid        = saved_parent;
    np->cabin_heap_base    = saved_heap_base;
    np->cabin_heap_max_size = saved_heap_max_size;
    np->cabin_buf_heap_base = saved_buf_heap_base;
    np->cabin_stack_top    = saved_stack_top;
}

bool notify_add_prefix(uint8_t deck_id, uint8_t opcode) {
    notify_page_t* np = notify_page();
    if (np->prefix_count >= MAX_PREFIXES) {
        return false;
    }
    np->prefixes[np->prefix_count++] = PREFIX(deck_id, opcode);
    return true;
}

size_t notify_write_data(const void* data, size_t size) {
    notify_page_t* np = notify_page();

    size_t to_write = (size > INLINE_DATA_SIZE)
                      ? INLINE_DATA_SIZE
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
    notify_page()->magic = 0;
    return (event_id_t)result;
}

void notify_data(const void* data, size_t size) {
    notify_page_t* np = notify_page();
    if (np->magic != NOTIFY_MAGIC) {
        notify_prepare();
    }
    notify_write_data(data, size);
}
