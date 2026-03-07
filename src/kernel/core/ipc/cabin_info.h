#ifndef CABIN_INFO_H
#define CABIN_INFO_H

#include "ktypes.h"
#include "boxos_magic.h"

// CabinInfo: read-only process metadata at fixed address 0x1000.
// Kernel fills this at process creation. Userspace reads pid, heap_base, etc.

typedef struct __packed {
    uint32_t magic;           // CABIN_INFO_MAGIC ("CABN")
    uint32_t pid;
    uint32_t spawner_pid;
    uint32_t flags;           // reserved for future use
    uint64_t heap_base;       // ASLR randomized heap start
    uint64_t heap_max_size;   // max heap size
    uint64_t buf_heap_base;   // ASLR randomized buffer heap start
    uint64_t stack_top;       // ASLR randomized stack top
    uint8_t  _reserved[4048]; // pad to exactly 4096 bytes (1 page)
} CabinInfo;

_Static_assert(sizeof(CabinInfo) == 4096, "CabinInfo must be exactly one page (4096 bytes)");

static inline bool cabin_info_valid(const CabinInfo* ci)
{
    return ci && ci->magic == CABIN_INFO_MAGIC;
}

#endif // CABIN_INFO_H
