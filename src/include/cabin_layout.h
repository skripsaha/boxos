#ifndef CABIN_LAYOUT_H
#define CABIN_LAYOUT_H

#include "boxos_limits.h"

// Fixed virtual address layout for every process (Cabin Architecture).
// Each process lives in an isolated Cabin with deterministic layout.

#define CABIN_NULL_TRAP_START    0x0000000000000000ULL
#define CABIN_NULL_TRAP_END      0x0000000000000FFFULL
#define CABIN_NULL_TRAP_SIZE     4096

// CabinInfo: read-only metadata (pid, heap_base, stack_top, etc.)
#define CABIN_INFO_ADDR          0x0000000000001000ULL  // 0x1000
#define CABIN_INFO_SIZE          0x1000                 // 4KB (1 page)
#define CABIN_INFO_PAGES         1

// PocketRing: userspace writes Pockets, kernel reads them (SPSC)
#define CABIN_POCKET_RING_ADDR   0x0000000000002000ULL  // 0x2000
#define CABIN_POCKET_RING_SIZE   0x1000                 // 4KB (1 page)
#define CABIN_POCKET_RING_PAGES  1

// ResultRing: kernel writes Results, userspace reads them (SPSC)
#define CABIN_RESULT_RING_ADDR   0x0000000000003000ULL  // 0x3000
#define CABIN_RESULT_RING_SIZE   0x9000                 // 36KB (9 pages)
#define CABIN_RESULT_RING_PAGES  9

// Code starts after ResultRing
#define CABIN_CODE_START_ADDR    0x000000000000C000ULL  // 0xC000+

// Nominal base addresses (actual per-process addresses are ASLR randomized)
#define CABIN_HEAP_BASE          0x0000000010000000ULL  // 256MB: nominal heap start
#define CABIN_BUF_HEAP_START     0x0000000040000000ULL  // 1GB: nominal buffer alloc region

#define CABIN_HEAP_MAX_SIZE      BOXOS_USER_HEAP_MAX_SIZE

// Convenience aliases
#define USER_CODE_ENTRY_POINT    CABIN_CODE_START_ADDR

#endif // CABIN_LAYOUT_H
