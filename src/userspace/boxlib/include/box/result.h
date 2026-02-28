#ifndef BOX_RESULT_H
#define BOX_RESULT_H

#include "types.h"
#include "error.h"

// Overflow marker in result payload
#define BOX_RESULT_OVERFLOW_MARKER 0xDEADBEEF

// Result Entry (from kernel result_page.h)
// MUST match kernel result_entry_t layout exactly
typedef struct BOX_PACKED
{
    uint8_t source; // ROUTE_SOURCE_* (replaces legacy status)
    uint8_t _reserved1;
    uint16_t size;       // Bytes of valid data in payload
    error_t error_code;  // Error code (4 bytes)
    uint32_t sender_pid; // PID of sending process (IPC)
    uint8_t payload[BOX_RESULT_PAYLOAD_SIZE];
} result_entry_t;

// ABI verification
BOX_STATIC_ASSERT(sizeof(result_entry_t) == 256, "Result entry must be 256 bytes");
BOX_STATIC_ASSERT(BOX_OFFSETOF(result_entry_t, payload) == 12, "Payload offset must match kernel");

// Result Ring Buffer
typedef struct BOX_PACKED
{
    volatile uint32_t head __attribute__((aligned(64))); // User updates
    uint8_t _pad1[60];
    volatile uint32_t tail __attribute__((aligned(64))); // Kernel updates
    uint8_t _pad2[60];
    result_entry_t entries[BOX_RESULT_RING_SIZE];
} result_ring_t;

// Result Page
typedef struct BOX_PACKED
{
    uint32_t magic;    // Offset 0
    uint32_t _padding; // Offset 4

    // Notification flag (cache-line isolated in first 64 bytes)
    volatile uint8_t notification_flag; // Offset 8
    uint8_t _pad_notify[63];            // Offset 9-71 (matches kernel cache-line isolation)

    result_ring_t ring;    // Offset 72 (MUST match kernel!)
    uint8_t _reserved[56]; // Offset 4040-4095 (final padding to 4096 bytes)
} result_page_t;

// ABI verification - MUST match kernel result_page_t layout
BOX_STATIC_ASSERT(sizeof(result_page_t) == 4096, "Result page must be exactly 4096 bytes");
BOX_STATIC_ASSERT(BOX_OFFSETOF(result_page_t, notification_flag) == 8, "notification_flag must be at offset 8");
BOX_STATIC_ASSERT(BOX_OFFSETOF(result_page_t, ring) == 72, "ring must be at offset 72 to match kernel");

// Get Result Page pointer
BOX_INLINE result_page_t *result_page(void)
{
    return (result_page_t *)BOX_RESULT_PAGE_ADDR;
}

// API functions
bool result_available(void);
uint32_t result_count(void);
bool result_pop(result_entry_t *out_entry);

// Wait for a result entry (automatic tier selection):
// - UMONITOR/UMWAIT on Intel 2020+ CPUs (WAITPKG)
// - Cooperative yield on all other CPUs
// @out_entry: Output buffer for result entry
// @timeout_ms: Timeout in milliseconds (0 = infinite wait)
// @return: true if result received, false if timeout
bool result_wait(result_entry_t *out_entry, uint32_t timeout_ms);

// Filtered result pop (IPC stash-aware)
bool result_pop_non_ipc(result_entry_t* out_entry);
bool result_pop_ipc(result_entry_t* out_entry);
uint32_t result_ipc_stash_count(void);

// Backpressure API
bool result_page_full(void);
bool event_ring_full(void);

// Overflow status API
uint32_t get_overflow_count(bool reset);

#endif // BOX_RESULT_H
