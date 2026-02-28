#ifndef RESULT_PAGE_H
#define RESULT_PAGE_H

#include "ktypes.h"
#include "error.h"
#include "boxos_magic.h"
#include "boxos_sizes.h"

// Result Page Magic
#define RESULT_PAGE_MAGIC BOXOS_RESULT_MAGIC  // "RESU"
#define RESULT_RING_SIZE BOXOS_RESULT_RING_SIZE

// Result Entry: Single result in the ring buffer
typedef struct __packed {
    uint8_t  source;             // ROUTE_SOURCE_* (replaces legacy status high byte)
    uint8_t  _reserved1;
    uint16_t size;               // Bytes of valid data in payload
    boxos_error_t error_code;    // Error code (4 bytes)
    uint32_t sender_pid;         // PID of sending process (IPC)
    uint8_t  payload[BOXOS_RESULT_PAYLOAD_SIZE];
} result_entry_t;

// Size verification - see docs/api/event_structures.md
STATIC_ASSERT(RESULT_RING_SIZE == 15, "Result ring size must be 15 - see docs/api/event_structures.md");
STATIC_ASSERT(sizeof(result_entry_t) == 256, "Result entry must be exactly 256 bytes - see docs/api/event_structures.md");

// Result Ring: Lock-free SPSC ring buffer
typedef struct __packed {
    volatile uint32_t head __attribute__((aligned(64)));  // Read index (user updates) - cache line aligned
    uint8_t _pad1[60];  // Pad to 64 bytes to prevent false sharing
    volatile uint32_t tail __attribute__((aligned(64)));  // Write index (kernel updates) - cache line aligned
    uint8_t _pad2[60];  // Pad to 64 bytes to prevent false sharing
    result_entry_t entries[RESULT_RING_SIZE];  // Ring buffer
} result_ring_t;

// Result Page: Exactly 4096 bytes (ONE page at 0x2000)
// Contains magic + notification flag (cache-line isolated) + ring buffer
typedef struct __packed {
    uint32_t magic;           // 0x52455355 "RESU" - offset 0
    uint32_t _padding;        // Alignment padding - offset 4

    // Notification flag (cache-line isolated in first 64 bytes)
    // Offset 8: in first cache line (bytes 0-63), isolated from ring
    volatile uint8_t notification_flag;  // Offset 8
    uint8_t _pad_notify[63];             // Offset 9-71 (total 64 bytes with flag)

    result_ring_t ring;       // Offset 72, size 3968 → ends at 4040
    uint8_t _reserved[56];    // Offset 4040-4095
} result_page_t;

// Size verification
STATIC_ASSERT(sizeof(result_page_t) == 4096, "Result page must be exactly 4096 bytes - see docs/api/event_structures.md");
STATIC_ASSERT(offsetof(result_page_t, notification_flag) == 8, "notification_flag must be at offset 8");
STATIC_ASSERT((offsetof(result_page_t, notification_flag) % 64) == 8, "notification_flag in first cache line (offset % 64 == 8)");


// Ring buffer helpers
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
