#ifndef BOX_CORE_CABIN_H
#define BOX_CORE_CABIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"

typedef struct PACKED {
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

STATIC_ASSERT(sizeof(CabinInfo) == 64, "CabinInfo must be 64 bytes");

INLINE CabinInfo* cabin_info(void) {
    return (CabinInfo*)CABIN_INFO_VADDR;
}

#ifdef __cplusplus
}
#endif

#endif