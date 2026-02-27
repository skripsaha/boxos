#ifndef NOTIFY_PAGE_H
#define NOTIFY_PAGE_H

#include "ktypes.h"
#include "events.h"
#include "boxos_magic.h"
#include "boxos_sizes.h"

#define NOTIFY_PAGE_MAGIC     BOXOS_NOTIFY_MAGIC  // "NOTI" - Correct magic for Notify Page
#define NOTIFY_PAGE_SIZE      BOXOS_PAGE_SIZE
#define NOTIFY_MAX_PREFIXES   BOXOS_EVENT_MAX_PREFIXES  // MUST match EVENT_MAX_PREFIXES
#define NOTIFY_DATA_SIZE      BOXOS_EVENT_DATA_SIZE     // MUST match EVENT_DATA_SIZE

// Status codes for error reporting
#define NOTIFY_STATUS_OK          0
#define NOTIFY_STATUS_RING_FULL   1
#define NOTIFY_STATUS_INVALID     2

// Flags
#define NOTIFY_FLAG_CHECK_STATUS  0x01    // Userspace must check status field

typedef struct __packed {
    uint32_t magic;                          // 0x4E4F5449 "NOTI"
    uint8_t  prefix_count;                   // Number of prefixes (0-16)
    uint8_t  flags;                          // NOTIFY_FLAG_CHECK_STATUS
    uint8_t  status;                         // NOTIFY_STATUS_* codes
    uint8_t  reserved1;                      // Padding
    uint16_t prefixes[NOTIFY_MAX_PREFIXES];  // Command chain (16 * 2 = 32 bytes)
    uint8_t  data[NOTIFY_DATA_SIZE];         // Payload data (256 bytes)
    volatile uint8_t event_ring_full;        // Backpressure: EventRing >90% full
    volatile uint8_t result_page_full;       // Backpressure: Result Page full
    uint32_t route_target;
    char     route_tag[32];
    uint8_t  _reserved[3762];                // Pad to 4096 bytes
} notify_page_t;

STATIC_ASSERT(NOTIFY_MAX_PREFIXES == EVENT_MAX_PREFIXES, "Prefix count must match Event structure - see docs/api/event_structures.md");
STATIC_ASSERT(NOTIFY_DATA_SIZE == EVENT_DATA_SIZE, "Data size must match Event structure - see docs/api/event_structures.md");
STATIC_ASSERT(sizeof(notify_page_t) == NOTIFY_PAGE_SIZE, "Notify page must be exactly 4096 bytes - see docs/api/event_structures.md");

// Compile-time size checks
_Static_assert(sizeof(notify_page_t) == 4096, "Notify page must be exactly one page (4096 bytes)");
_Static_assert(sizeof(notify_page_t) % 4096 == 0, "Notify page must be page-aligned");

#endif // NOTIFY_PAGE_H
