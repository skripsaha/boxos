#ifndef BOX_RESULT_H
#define BOX_RESULT_H

#include "types.h"
#include "error.h"

#define RESULT_OVERFLOW_MARKER 0xDEADBEEF

typedef struct PACKED
{
    uint8_t source;
    uint8_t _reserved1;
    uint16_t size;
    error_t error_code;
    uint32_t sender_pid;
    uint8_t payload[RESULT_PAYLOAD_SIZE];
} result_entry_t;

STATIC_ASSERT(sizeof(result_entry_t) == 256, "Result entry must be 256 bytes");
STATIC_ASSERT(OFFSETOF(result_entry_t, payload) == 12, "Payload offset must match kernel");

typedef struct PACKED
{
    volatile uint32_t head __attribute__((aligned(64))); // User updates
    uint8_t _pad1[60];
    volatile uint32_t tail __attribute__((aligned(64))); // Kernel updates
    uint8_t _pad2[60];
    result_entry_t entries[RESULT_RING_SIZE];
} result_ring_t;

typedef struct PACKED
{
    uint32_t magic;
    uint32_t _padding;

    // cache-line isolated (matches kernel layout)
    volatile uint8_t notification_flag; // Offset 8
    uint8_t _pad_notify[63];            // Offset 9-71

    result_ring_t ring;    // Offset 72
    uint8_t _reserved[56]; // Offset 28616-28671
} result_page_t;

STATIC_ASSERT(sizeof(result_page_t) == CABIN_RESULT_PAGE_SIZE, "Result page must be exactly CABIN_RESULT_PAGE_SIZE (28KB)");
STATIC_ASSERT(OFFSETOF(result_page_t, notification_flag) == 8, "notification_flag must be at offset 8");
STATIC_ASSERT(OFFSETOF(result_page_t, ring) == 72, "ring must be at offset 72 to match kernel");

INLINE result_page_t *result_page(void)
{
    return (result_page_t *)RESULT_PAGE_ADDR;
}

bool result_available(void);
uint32_t result_count(void);
bool result_pop(result_entry_t *out_entry);

// Uses UMONITOR/UMWAIT on CPUs with WAITPKG; falls back to cooperative yield.
bool result_wait(result_entry_t *out_entry, uint32_t timeout_ms);

bool result_pop_non_ipc(result_entry_t* out_entry);
bool result_pop_ipc(result_entry_t* out_entry);
uint32_t result_ipc_stash_count(void);

bool result_page_full(void);
bool event_ring_full(void);

uint32_t get_overflow_count(bool reset);

#endif // BOX_RESULT_H
