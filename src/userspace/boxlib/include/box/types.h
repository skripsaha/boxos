#ifndef BOX_TYPES_H
#define BOX_TYPES_H

#include "box/defs.h"
#include "cabin_layout.h"
#include "boxos_magic.h"
#include "boxos_sizes.h"
#include "boxos_decks.h"

#define NOTIFY_PAGE_ADDR   CABIN_NOTIFY_PAGE_ADDR
#define RESULT_PAGE_ADDR   CABIN_RESULT_PAGE_ADDR
#define CODE_START_ADDR    CABIN_CODE_START_ADDR

#define MAX_PREFIXES       EVENT_MAX_PREFIXES
#define INLINE_DATA_SIZE   EVENT_DATA_SIZE

#define NOTIFY_STATUS_OK          0
#define NOTIFY_STATUS_RING_FULL   1
#define NOTIFY_STATUS_INVALID     2

#define NOTIFY_FLAG_CHECK_STATUS  0x01

typedef enum {
    EVENT_NEW = 0,
    EVENT_PROCESSING = 1,
    EVENT_COMPLETED = 2,
    EVENT_ERROR = 3,
    EVENT_ACCESS_DENIED = 4,
    EVENT_CRITICAL_ERROR = 5,
    EVENT_RETRY = 6
} event_state_t;

#define PREFIX(deck_id, opcode) \
    ((uint16_t)(((uint8_t)(deck_id) << 8) | ((uint8_t)(opcode) & 0xFF)))

#define DECK_ID(prefix)  (((prefix) >> 8) & 0xFF)
#define OPCODE(prefix)   ((prefix) & 0xFF)

#define PACKED __attribute__((packed))
#define INLINE static inline __attribute__((always_inline))

#define STATIC_ASSERT(expr, msg) _Static_assert(expr, msg)

#define OFFSETOF(type, member) __builtin_offsetof(type, member)

typedef uint32_t event_id_t;
typedef uint32_t file_id_t;

#endif // BOX_TYPES_H
