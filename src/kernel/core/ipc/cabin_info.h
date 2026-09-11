#ifndef CABIN_INFO_H
#define CABIN_INFO_H

#include "ktypes.h"
#include "boxos_magic.h"


typedef struct __packed {
    uint32_t magic;
    uint32_t pid;
    uint32_t spawner_pid;
    uint32_t generation;
    uint64_t heap_base;
    uint64_t heap_max_size;
    uint64_t buf_heap_base;
    uint64_t stack_top;
    uint64_t luggage_addr;
    uint32_t luggage_length;
    uint32_t luggage_reserved;
} CabinInfo;

_Static_assert(sizeof(CabinInfo) == 64, "CabinInfo header must be 64 bytes");

#define CABIN_LUGGAGE_INLINE_OFFSET  ((uint32_t)sizeof(CabinInfo))

static inline bool cabin_info_valid(const CabinInfo* ci)
{
    return ci && ci->magic == CABIN_INFO_MAGIC;
}

#endif