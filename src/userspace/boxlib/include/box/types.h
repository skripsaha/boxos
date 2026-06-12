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
#define TOUCH_RING_VADDR   CABIN_TOUCH_RING_ADDR
#define CODE_START_ADDR    CABIN_CODE_START_ADDR

#define PACKED __attribute__((packed))
#define INLINE static inline __attribute__((always_inline))

#ifdef __cplusplus
#define STATIC_ASSERT(expr, msg) static_assert(expr, msg)
#else
#define STATIC_ASSERT(expr, msg) _Static_assert(expr, msg)
#endif

#define OFFSETOF(type, member) __builtin_offsetof(type, member)

typedef uint32_t file_id_t;

#endif // BOX_TYPES_H
