#ifndef BOX_NOTIFY_H
#define BOX_NOTIFY_H

#include "types.h"

// Notify flags
#define BOX_NOTIFY_FLAG_YIELD  0x80

// Notify Page structure (MUST match kernel!)
typedef struct BOX_PACKED {
    uint32_t magic;
    uint8_t  prefix_count;
    uint8_t  flags;
    uint8_t  status;
    uint8_t  reserved1;
    uint16_t prefixes[BOX_MAX_PREFIXES];
    uint8_t  data[BOX_INLINE_DATA_SIZE];
    volatile uint8_t event_ring_full;    // Backpressure: EventRing >90% full
    volatile uint8_t result_page_full;   // Backpressure: Result Page full
    uint32_t route_target;
    char     route_tag[32];
    uint32_t parent_pid;
    uint8_t  _reserved[3758];
} notify_page_t;

// Get Notify Page pointer
BOX_INLINE notify_page_t* notify_page(void) {
    return (notify_page_t*)BOX_NOTIFY_PAGE_ADDR;
}

// === Core: always last in chain, sends + resets ===
event_id_t notify(void);

// === Raw data writer for advanced chains ===
void notify_data(const void* data, size_t size);

// === Internal helpers (used by chain builders) ===
void notify_prepare(void);
bool notify_add_prefix(uint8_t deck_id, uint8_t opcode);
size_t notify_write_data(const void* data, size_t size);

#endif // BOX_NOTIFY_H
