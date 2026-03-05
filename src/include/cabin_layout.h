#ifndef CABIN_LAYOUT_H
#define CABIN_LAYOUT_H

#include "boxos_limits.h"

// Fixed virtual address layout for every process (Snowball Architecture).

#define CABIN_NULL_TRAP_START    0x0000000000000000ULL
#define CABIN_NULL_TRAP_END      0x0000000000000FFFULL
#define CABIN_NULL_TRAP_SIZE     4096

#define CABIN_NOTIFY_PAGE_ADDR   0x0000000000001000ULL  // 0x1000
#define CABIN_NOTIFY_PAGE_END    0x0000000000002FFFULL  // 0x2FFF
#define CABIN_RESULT_PAGE_ADDR   0x0000000000003000ULL  // 0x3000
#define CABIN_RESULT_PAGE_END    0x000000000000BFFFULL  // 0xBFFF
#define CABIN_CODE_START_ADDR    0x000000000000C000ULL  // 0xC000+

#define CABIN_NOTIFY_PAGE_SIZE   0x2000   // 8KB  (2 pages)
#define CABIN_RESULT_PAGE_SIZE   0x9000   // 36KB (9 pages)

#define CABIN_NOTIFY_PAGE_PAGES  (CABIN_NOTIFY_PAGE_SIZE / 4096)  // 2
#define CABIN_RESULT_PAGE_PAGES  (CABIN_RESULT_PAGE_SIZE / 4096)  // 7

// Nominal base addresses — actual per-process addresses are randomized by ASLR.
// Kernel adds a random page-aligned offset at process creation time.
// Userspace reads the actual heap base from cabin_info in the notify page.
#define CABIN_HEAP_BASE          0x0000000010000000ULL  // 256MB: nominal heap start
#define CABIN_BUF_HEAP_START     0x0000000040000000ULL  // 1GB: nominal buffer alloc region

#define NOTIFY_PAGE_VADDR        CABIN_NOTIFY_PAGE_ADDR
#define RESULT_PAGE_VADDR        CABIN_RESULT_PAGE_ADDR
#define USER_CODE_ENTRY_POINT    CABIN_CODE_START_ADDR

#define CABIN_HEAP_MAX_SIZE      BOXOS_USER_HEAP_MAX_SIZE

#endif // CABIN_LAYOUT_H
