#ifndef NOTIFY_PAGE_H
#define NOTIFY_PAGE_H

#include "ktypes.h"
#include "events.h"
#include "boxos_magic.h"
#include "boxos_sizes.h"

#define NOTIFY_PAGE_MAGIC     NOTIFY_MAGIC
#define NOTIFY_PAGE_SIZE      PAGE_SIZE
#define NOTIFY_MAX_PREFIXES   EVENT_MAX_PREFIXES
#define NOTIFY_DATA_SIZE      EVENT_DATA_SIZE

#define NOTIFY_STATUS_OK          0
#define NOTIFY_STATUS_RING_FULL   1
#define NOTIFY_STATUS_INVALID     2

#define NOTIFY_FLAG_CHECK_STATUS  0x01    // userspace must check status field before reuse
#define NOTIFY_FLAG_YIELD         0x80    // cooperative yield: no event, just reschedule

typedef struct __packed {
    uint32_t magic;                          // 0x4E4F5449 "NOTI"
    uint8_t  prefix_count;                   // 0-16
    uint8_t  flags;
    uint8_t  status;
    uint8_t  reserved1;
    uint16_t prefixes[NOTIFY_MAX_PREFIXES];  // 16 * 2 = 32 bytes
    uint8_t  data[NOTIFY_DATA_SIZE];         // 256 bytes
    volatile uint8_t event_ring_full;        // backpressure: EventRing >90% full
    volatile uint8_t result_page_full;       // backpressure: Result Page full
    uint32_t route_target;
    char     route_tag[32];
    uint32_t spawner_pid;
    uint8_t  _reserved[3758];                // pad to 4096 bytes
} notify_page_t;

STATIC_ASSERT(NOTIFY_MAX_PREFIXES == EVENT_MAX_PREFIXES, "Prefix count must match Event structure - see docs/api/event_structures.md");
STATIC_ASSERT(NOTIFY_DATA_SIZE == EVENT_DATA_SIZE, "Data size must match Event structure - see docs/api/event_structures.md");
STATIC_ASSERT(sizeof(notify_page_t) == NOTIFY_PAGE_SIZE, "Notify page must be exactly 4096 bytes - see docs/api/event_structures.md");

_Static_assert(sizeof(notify_page_t) == 4096, "Notify page must be exactly one page (4096 bytes)");
_Static_assert(sizeof(notify_page_t) % 4096 == 0, "Notify page must be page-aligned");

#endif // NOTIFY_PAGE_H
