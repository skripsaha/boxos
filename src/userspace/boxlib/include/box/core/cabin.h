#ifndef BOX_CORE_CABIN_H
#define BOX_CORE_CABIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"

/*
 * CabinInfo — read-only metadata at CABIN_INFO_VADDR (0x1000).
 * Kernel writes this once at process creation. Userspace reads pid,
 * spawner_pid, heap layout, stack top — and the Luggage: what the spawner
 * said to this cabin at boarding, the command line as the person typed it.
 * luggage_addr points at the bytes (in this very page when they fit, in the
 * buffer heap when they do not), luggage_length counts them; both zero for
 * a cabin given nothing. Read it through box/luggage.h.
 */
typedef struct PACKED {
    uint32_t magic;           /* CABIN_INFO_MAGIC ("CABN") */
    uint32_t pid;
    uint32_t spawner_pid;
    uint32_t generation;      /* pid_generation at creation: (pid, generation) is who this is */
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

#endif /* BOX_CORE_CABIN_H */
