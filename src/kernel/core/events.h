#ifndef EVENTS_H
#define EVENTS_H

#include "ktypes.h"
#include "kernel_config.h"
#include "klib.h"
#include "error.h"
#include "boxos_magic.h"
#include "boxos_sizes.h"

#define EVENT_MAGIC            BOXOS_EVENT_MAGIC  // "EVT!"
#define RESPONSE_MAGIC         0x52455350  // "RESP"
#define EVENT_MAX_PREFIXES     BOXOS_EVENT_MAX_PREFIXES
#define EVENT_METADATA_SIZE    32
#define EVENT_DATA_SIZE        BOXOS_EVENT_DATA_SIZE
#define EVENT_TOTAL_SIZE       384

// Routing constants (IPC)
#define ROUTE_SOURCE_KERNEL   0x00
#define ROUTE_SOURCE_PROCESS  0x01
#define ROUTE_SOURCE_HARDWARE 0x02

#define ROUTE_FLAG_TEXT       0x00
#define ROUTE_FLAG_ERROR      0x01
#define ROUTE_FLAG_BINARY     0x02
#define ROUTE_FLAG_COMMAND    0x03

#define MAX_ROUTE_TAG_TARGETS 16
#define RESPONSE_METADATA_SIZE 32
#define RESPONSE_DATA_SIZE     4064
#define RESPONSE_TOTAL_SIZE    4096

// Legacy EventStatus - DEPRECATED: Use boxos_error_t instead
// Kept for backward compatibility during migration
typedef enum {
    EVENT_STATE_NEW = 0,
    EVENT_STATE_PROCESSING,
    EVENT_STATE_COMPLETED,
    EVENT_STATE_ERROR,
    EVENT_STATE_ACCESS_DENIED,
    EVENT_STATE_CRITICAL_ERROR,
    EVENT_STATE_RETRY
} EventStatus __attribute__((deprecated("Use boxos_error_t instead")));

typedef struct __packed {
    uint32_t magic;
    uint32_t pid;
    uint32_t event_id;
    uint8_t  current_prefix_idx;
    uint8_t  prefix_count;
    uint8_t  state;
    uint8_t  error_deck_idx;
    uint64_t timestamp;
    boxos_error_t error_code;
    boxos_error_t first_error;
    // Routing header
    uint32_t route_target;
    uint32_t sender_pid;
    uint8_t  route_flags;
    char     route_tag[32];
    uint8_t  _reserved_routing[23];
    // Prefix chain
    uint16_t prefixes[EVENT_MAX_PREFIXES];
    // Data payload
    uint8_t  data[EVENT_DATA_SIZE];
} Event;

STATIC_ASSERT(EVENT_MAX_PREFIXES == 16, "Prefix count must be 16 - see docs/api/event_structures.md");
STATIC_ASSERT(EVENT_DATA_SIZE == 256, "Data size must be 256 bytes - see docs/api/event_structures.md");
STATIC_ASSERT(sizeof(Event) == EVENT_TOTAL_SIZE, "Event must be exactly 384 bytes - see docs/api/event_structures.md");

// Compile-time alignment checks
_Static_assert(sizeof(Event) % 64 == 0, "Event should be cache-line aligned (64 bytes)");
_Static_assert(sizeof(Event) == 384, "Event size should be 384 bytes");

// NOTE: Response structure removed - BoxOS uses result_page.h instead

static inline void event_init(Event* event, uint32_t pid, uint32_t event_id) {
    if (!event) return;

    memset(event, 0, sizeof(Event));
    event->magic = EVENT_MAGIC;
    event->pid = pid;
    event->event_id = event_id;
    event->state = EVENT_STATE_NEW;  // Legacy field
    event->current_prefix_idx = 0;
    event->prefix_count = 0;

    // Initialize error tracking
    event->error_code = BOXOS_OK;
    event->first_error = BOXOS_OK;
    event->error_deck_idx = 0xFF;

    // Initialize routing fields
    event->route_target = 0;
    event->sender_pid = 0;
    event->route_flags = ROUTE_SOURCE_KERNEL;
    event->route_tag[0] = '\0';
}

static inline bool event_validate(const Event* event) {
    return event &&
           event->magic == EVENT_MAGIC &&
           event->current_prefix_idx < EVENT_MAX_PREFIXES;
}

static inline uint8_t event_get_deck_id(const Event* event, uint8_t idx) {
    if (!event || idx >= EVENT_MAX_PREFIXES) return 0xFF;
    return (event->prefixes[idx] >> 8) & 0xFF;
}

static inline uint8_t event_get_opcode(const Event* event, uint8_t idx) {
    if (!event || idx >= EVENT_MAX_PREFIXES) return 0xFF;
    return event->prefixes[idx] & 0xFF;
}

static inline uint16_t event_current_prefix(const Event* event) {
    if (!event || event->current_prefix_idx >= EVENT_MAX_PREFIXES) return 0x0000;
    return event->prefixes[event->current_prefix_idx];
}

static inline void event_advance(Event* event) {
    if (event && event->current_prefix_idx < EVENT_MAX_PREFIXES) {
        event->current_prefix_idx++;
    }
}

// Error tracking helpers
static inline bool event_is_pending(const Event* evt) {
    return evt && evt->error_code == BOXOS_OK && evt->first_error == BOXOS_OK;
}

static inline bool event_is_success(const Event* evt) {
    return evt && evt->first_error == BOXOS_OK;
}

static inline bool event_is_failed(const Event* evt) {
    return evt && evt->first_error != BOXOS_OK;
}

static inline void event_set_error(Event* evt, boxos_error_t err, uint8_t deck_idx) {
    if (!evt) return;

    evt->error_code = err;

    // Capture first error only (root cause tracking)
    if (evt->first_error == BOXOS_OK && err != BOXOS_OK) {
        evt->first_error = err;
        evt->error_deck_idx = deck_idx;
    }
}

#endif // EVENTS_H
