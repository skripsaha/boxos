#ifndef RESULT_PAGE_H
#define RESULT_PAGE_H

#include "ktypes.h"
#include "error.h"
#include "boxos_magic.h"
#include "boxos_sizes.h"
#include "cabin_layout.h"

#define RESULT_PAGE_MAGIC RESULT_MAGIC

typedef struct __packed {
    uint8_t  source;             // ROUTE_SOURCE_*
    uint8_t  _reserved1;
    uint16_t size;               // bytes of valid data in payload
    error_t error_code;
    uint32_t sender_pid;
    uint8_t  payload[RESULT_PAYLOAD_SIZE];
} result_entry_t;

STATIC_ASSERT(RESULT_RING_SIZE == 111, "Result ring size must be 111 - see docs/api/event_structures.md");
STATIC_ASSERT(sizeof(result_entry_t) == 256, "Result entry must be exactly 256 bytes - see docs/api/event_structures.md");

// SPSC ring: head updated by user, tail updated by kernel; cache-line padding prevents false sharing
typedef struct __packed {
    volatile uint32_t head __attribute__((aligned(64)));
    uint8_t _pad1[60];
    volatile uint32_t tail __attribute__((aligned(64)));
    uint8_t _pad2[60];
    result_entry_t entries[RESULT_RING_SIZE];
} result_ring_t;

typedef struct __packed {
    uint32_t magic;           // 0x52455355 "RESU" - offset 0
    uint32_t _padding;        // offset 4

    // notification_flag is in the first cache line (bytes 0-63), isolated from the ring
    volatile uint8_t notification_flag;  // offset 8
    uint8_t _pad_notify[63];             // offset 9-71

    result_ring_t ring;       // offset 72, size 28544 -> ends at 28616
    uint8_t _reserved[56];    // offset 28616-28671
} result_page_t;

STATIC_ASSERT(sizeof(result_page_t) == CABIN_RESULT_PAGE_SIZE, "Result page must be exactly CABIN_RESULT_PAGE_SIZE (28KB)");
STATIC_ASSERT(offsetof(result_page_t, notification_flag) == 8, "notification_flag must be at offset 8");
STATIC_ASSERT((offsetof(result_page_t, notification_flag) % 64) == 8, "notification_flag in first cache line (offset % 64 == 8)");

static inline int result_ring_is_full(const result_ring_t* ring) {
    return ((ring->tail + 1) % RESULT_RING_SIZE) == ring->head;
}

static inline int result_ring_is_empty(const result_ring_t* ring) {
    return ring->tail == ring->head;
}

static inline uint32_t result_ring_count(const result_ring_t* ring) {
    if (ring->tail >= ring->head) {
        return ring->tail - ring->head;
    }
    return RESULT_RING_SIZE - (ring->head - ring->tail);
}

#endif // RESULT_PAGE_H
