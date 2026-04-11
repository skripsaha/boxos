#ifndef BOX_TYPES_H
#define BOX_TYPES_H

#include "box/defs.h"
#include "cabin_layout.h"
#include "boxos_magic.h"
#include "boxos_sizes.h"
#include "boxos_decks.h"

// Cabin addresses
#define CABIN_INFO_VADDR   CABIN_INFO_ADDR
#define POCKET_RING_VADDR  CABIN_POCKET_RING_ADDR
#define RESULT_RING_VADDR  CABIN_RESULT_RING_ADDR
#define CODE_START_ADDR    CABIN_CODE_START_ADDR

// Prefix encoding
#define MAX_PREFIXES       POCKET_MAX_PREFIXES

#define PREFIX(deck_id, opcode) \
    ((uint16_t)(((uint8_t)(deck_id) << 8) | ((uint8_t)(opcode) & 0xFF)))

#define DECK_ID(prefix)  (((prefix) >> 8) & 0xFF)
#define OPCODE(prefix)   ((prefix) & 0xFF)

#define PACKED __attribute__((packed))
#define INLINE static inline __attribute__((always_inline))

#define STATIC_ASSERT(expr, msg) _Static_assert(expr, msg)

#define OFFSETOF(type, member) __builtin_offsetof(type, member)

typedef uint32_t file_id_t;

#endif // BOX_TYPES_H
