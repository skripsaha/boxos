#ifndef BOX_NOTIFY_H
#define BOX_NOTIFY_H

#include "types.h"

#define NOTIFY_FLAG_YIELD  0x80

typedef struct PACKED {
    uint32_t magic;
    uint8_t  prefix_count;
    uint8_t  flags;
    uint8_t  status;
    uint8_t  reserved1;
    uint16_t prefixes[MAX_PREFIXES];
    uint8_t  data[INLINE_DATA_SIZE];
    volatile uint8_t event_ring_full;    // backpressure: EventRing >90% full
    volatile uint8_t result_page_full;   // backpressure: Result Page full
    uint32_t route_target;
    char     route_tag[32];
    uint32_t spawner_pid;

    // ASLR: kernel provides actual randomized addresses.
    uint64_t cabin_heap_base;            // actual heap base
    uint64_t cabin_heap_max_size;        // max heap size from this base
    uint64_t cabin_buf_heap_base;        // actual buffer heap base
    uint64_t cabin_stack_top;            // actual stack top
    uint8_t  _reserved[7822];            // pad to CABIN_NOTIFY_PAGE_SIZE (8KB)
} notify_page_t;

INLINE notify_page_t* notify_page(void) {
    return (notify_page_t*)NOTIFY_PAGE_ADDR;
}

event_id_t notify(void);

void notify_data(const void* data, size_t size);

void notify_prepare(void);
bool notify_add_prefix(uint8_t deck_id, uint8_t opcode);
size_t notify_write_data(const void* data, size_t size);

#endif // BOX_NOTIFY_H
