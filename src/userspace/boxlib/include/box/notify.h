#ifndef BOX_NOTIFY_H
#define BOX_NOTIFY_H

#include "types.h"

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
    uint8_t  _reserved[3762];
} box_notify_page_t;

// Get Notify Page pointer
BOX_INLINE box_notify_page_t* box_notify_page(void) {
    return (box_notify_page_t*)BOX_NOTIFY_PAGE_ADDR;
}

// API functions
void box_notify_prepare(void);
bool box_notify_add_prefix(uint8_t deck_id, uint8_t opcode);
size_t box_notify_write_data(const void* data, size_t size);
box_event_id_t box_notify_execute(void);

// High-level wrapper
box_event_id_t box_notify(uint8_t deck_id, uint8_t opcode,
                          const void* data, size_t data_size);

// Checked version: returns status code
int box_notify_checked(uint8_t deck_id, uint8_t opcode,
                       const void* data, size_t data_size,
                       box_event_id_t* out_event_id);

// Retry version: automatically retries with exponential backoff
box_event_id_t box_notify_with_retry(uint8_t deck_id, uint8_t opcode,
                                     const void* data, size_t data_size,
                                     uint32_t max_retries);

#endif // BOX_NOTIFY_H
