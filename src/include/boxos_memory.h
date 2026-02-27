#ifndef BOXOS_MEMORY_H
#define BOXOS_MEMORY_H

#include "ktypes.h"

// ============================================================================
// BOXOS MEMORY BOUNDARIES AND CONSTANTS
// ============================================================================

// Important memory boundaries
#define BOXOS_LOW_MEMORY_END       0x100000ULL     // 1MB - End of real mode region
#define BOXOS_CRITICAL_MEMORY_END  0x1000000ULL    // 16MB - Critical kernel region
#define BOXOS_32BIT_MAX_ADDR       0x100000000ULL  // 4GB - 32-bit addressable limit

// Memory region checks
#define BOXOS_IS_LOW_MEMORY(addr)      ((addr) < BOXOS_LOW_MEMORY_END)
#define BOXOS_IS_CRITICAL_MEMORY(addr) ((addr) < BOXOS_CRITICAL_MEMORY_END)
#define BOXOS_IS_32BIT_SAFE(addr)      ((addr) < BOXOS_32BIT_MAX_ADDR)

// Page alignment
#define BOXOS_PAGE_ALIGN_DOWN(addr) ((addr) & ~0xFFFULL)
#define BOXOS_PAGE_ALIGN_UP(addr)   (((addr) + 0xFFFULL) & ~0xFFFULL)
#define BOXOS_IS_PAGE_ALIGNED(addr) (((addr) & 0xFFFULL) == 0)

#endif // BOXOS_MEMORY_H
