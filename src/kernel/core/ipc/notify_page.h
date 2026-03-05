#ifndef NOTIFY_PAGE_H
#define NOTIFY_PAGE_H

#include "ktypes.h"
#include "events.h"
#include "boxos_magic.h"
#include "boxos_sizes.h"
#include "cabin_layout.h"

#define NOTIFY_PAGE_MAGIC     NOTIFY_MAGIC
#define NOTIFY_PAGE_SIZE      CABIN_NOTIFY_PAGE_SIZE
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

    // ASLR: kernel writes actual randomized addresses here at process creation.
    // Userspace reads these to initialize heap/stack with correct bases.
    uint64_t cabin_heap_base;                // actual heap base (CABIN_HEAP_BASE + random offset)
    uint64_t cabin_heap_max_size;            // max heap size from this base
    uint64_t cabin_buf_heap_base;            // actual buffer heap base
    uint64_t cabin_stack_top;                // actual stack top
    uint8_t  _reserved[7822];                // pad to 8192 bytes (CABIN_NOTIFY_PAGE_SIZE)
} notify_page_t;

STATIC_ASSERT(NOTIFY_MAX_PREFIXES == EVENT_MAX_PREFIXES, "Prefix count must match Event structure - see docs/api/event_structures.md");
STATIC_ASSERT(NOTIFY_DATA_SIZE == EVENT_DATA_SIZE, "Data size must match Event structure - see docs/api/event_structures.md");
STATIC_ASSERT(sizeof(notify_page_t) == NOTIFY_PAGE_SIZE, "Notify page must be exactly CABIN_NOTIFY_PAGE_SIZE bytes");

_Static_assert(sizeof(notify_page_t) == CABIN_NOTIFY_PAGE_SIZE, "Notify page must match CABIN_NOTIFY_PAGE_SIZE (8KB)");
_Static_assert(sizeof(notify_page_t) % 4096 == 0, "Notify page must be page-aligned");

#endif // NOTIFY_PAGE_H
