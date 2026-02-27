#ifndef CABIN_LAYOUT_H
#define CABIN_LAYOUT_H

// ============================================================================
// BOXOS CABIN MEMORY LAYOUT
// ============================================================================
// Fixed virtual address layout for every process (Snowball Architecture)
// See: docs/core/memory_cabin.md

// Cabin zones
#define CABIN_NULL_TRAP_START    0x0000000000000000ULL  // 0x0000-0x0FFF
#define CABIN_NULL_TRAP_END      0x0000000000000FFFULL
#define CABIN_NULL_TRAP_SIZE     4096

#define CABIN_NOTIFY_PAGE_ADDR   0x0000000000001000ULL  // 0x1000
#define CABIN_RESULT_PAGE_ADDR   0x0000000000002000ULL  // 0x2000
#define CABIN_CODE_START_ADDR    0x0000000000003000ULL  // 0x3000+

// Page-aligned sizes
#define CABIN_NOTIFY_PAGE_SIZE   4096
#define CABIN_RESULT_PAGE_SIZE   4096

// Human-readable aliases
#define NOTIFY_PAGE_VADDR        CABIN_NOTIFY_PAGE_ADDR
#define RESULT_PAGE_VADDR        CABIN_RESULT_PAGE_ADDR
#define USER_CODE_ENTRY_POINT    CABIN_CODE_START_ADDR

#endif // CABIN_LAYOUT_H
