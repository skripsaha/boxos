#ifndef BOX_TYPES_H
#define BOX_TYPES_H

#include "box/defs.h"
#include "cabin_layout.h"
#include "boxos_magic.h"
#include "boxos_sizes.h"
#include "boxos_decks.h"

// Memory Cabin addresses
#define BOX_NOTIFY_PAGE_ADDR   CABIN_NOTIFY_PAGE_ADDR
#define BOX_RESULT_PAGE_ADDR   CABIN_RESULT_PAGE_ADDR
#define BOX_CODE_START_ADDR    CABIN_CODE_START_ADDR

// Magic numbers
#define BOX_NOTIFY_MAGIC       BOXOS_NOTIFY_MAGIC  // "NOTI"
#define BOX_RESULT_MAGIC       BOXOS_RESULT_MAGIC  // "RESU"

#define BOX_MAX_PREFIXES       BOXOS_EVENT_MAX_PREFIXES
#define BOX_INLINE_DATA_SIZE   BOXOS_EVENT_DATA_SIZE
#define BOX_RESULT_PAYLOAD_SIZE BOXOS_RESULT_PAYLOAD_SIZE
#define BOX_RESULT_RING_SIZE   BOXOS_RESULT_RING_SIZE

// Notify status codes
#define BOX_NOTIFY_STATUS_OK          0
#define BOX_NOTIFY_STATUS_RING_FULL   1
#define BOX_NOTIFY_STATUS_INVALID     2

// Notify flags
#define BOX_NOTIFY_FLAG_CHECK_STATUS  0x01

#define BOX_DECK_OPERATIONS    DECK_OPERATIONS
#define BOX_DECK_STORAGE       DECK_STORAGE
#define BOX_DECK_HARDWARE      DECK_HARDWARE
#define BOX_DECK_SYSTEM        DECK_SYSTEM
#define BOX_DECK_EXECUTION     DECK_EXECUTION

// Event states
typedef enum {
    BOX_EVENT_NEW = 0,
    BOX_EVENT_PROCESSING = 1,
    BOX_EVENT_COMPLETED = 2,
    BOX_EVENT_ERROR = 3,
    BOX_EVENT_ACCESS_DENIED = 4,
    BOX_EVENT_CRITICAL_ERROR = 5,
    BOX_EVENT_RETRY = 6
} event_state_t;

// Prefix helpers
#define BOX_PREFIX(deck_id, opcode) \
    ((uint16_t)(((uint8_t)(deck_id) << 8) | ((uint8_t)(opcode) & 0xFF)))

#define BOX_DECK_ID(prefix)  (((prefix) >> 8) & 0xFF)
#define BOX_OPCODE(prefix)   ((prefix) & 0xFF)

// Compiler attributes
#define BOX_PACKED __attribute__((packed))
#define BOX_INLINE static inline __attribute__((always_inline))

// Compile-time assertions
// BOX_STATIC_ASSERT is a legacy alias for _Static_assert
#define BOX_STATIC_ASSERT(expr, msg) _Static_assert(expr, msg)

// Offset calculation for struct members
#define BOX_OFFSETOF(type, member) __builtin_offsetof(type, member)

// Types
typedef uint32_t event_id_t;
typedef uint32_t file_id_t;

#endif // BOX_TYPES_H
