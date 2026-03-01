#ifndef BOXOS_MEMORY_H
#define BOXOS_MEMORY_H

#include "ktypes.h"

#define LOW_MEMORY_END       0x100000ULL     // 1MB
#define CRITICAL_MEMORY_END  0x1000000ULL    // 16MB
#define ADDR_32BIT_MAX       0x100000000ULL  // 4GB

#define IS_LOW_MEMORY(addr)      ((addr) < LOW_MEMORY_END)
#define IS_CRITICAL_MEMORY(addr) ((addr) < CRITICAL_MEMORY_END)
#define IS_32BIT_SAFE(addr)      ((addr) < ADDR_32BIT_MAX)

#define PAGE_ALIGN_DOWN(addr) ((addr) & ~0xFFFULL)
#define PAGE_ALIGN_UP(addr)   (((addr) + 0xFFFULL) & ~0xFFFULL)
#define IS_PAGE_ALIGNED(addr) (((addr) & 0xFFFULL) == 0)

#endif // BOXOS_MEMORY_H
