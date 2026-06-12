#ifndef BOX_CORE_CABIN_H
#define BOX_CORE_CABIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"

/*
 * CabinInfo — read-only metadata at CABIN_INFO_VADDR (0x1000).
 * Kernel writes this once at process creation. Userspace reads pid,
 * spawner_pid, heap layout, stack top.
 */
typedef struct PACKED {
    uint32_t magic;           /* CABIN_INFO_MAGIC ("CABN") */
    uint32_t pid;
    uint32_t spawner_pid;
    uint32_t reserved;
    uint64_t heap_base;
    uint64_t heap_max_size;
    uint64_t buf_heap_base;
    uint64_t stack_top;
} CabinInfo;

STATIC_ASSERT(sizeof(CabinInfo) == 48, "CabinInfo must be 48 bytes");

INLINE CabinInfo* cabin_info(void) {
    return (CabinInfo*)CABIN_INFO_VADDR;
}

#ifdef __cplusplus
}
#endif

#endif /* BOX_CORE_CABIN_H */
