#ifndef BOXOS_ADDRESSES_H
#define BOXOS_ADDRESSES_H

#include "cabin_layout.h"

// Cabin region addresses (per-process, virtual)
#define CABIN_INFO_VADDR          CABIN_INFO_ADDR          // 0x1000
#define POCKET_RING_VADDR         CABIN_POCKET_RING_ADDR   // 0x2000
#define RESULT_RING_VADDR         CABIN_RESULT_RING_ADDR   // 0x3000

// CPU capabilities page (shared read-only, before user stack)
#define CPU_CAPS_PAGE_ADDR        0x0000000007FFFF000ULL

// User stack top (~128TB user space)
#define USER_STACK_TOP            0x00007FFFFFFFE000ULL

#endif // BOXOS_ADDRESSES_H
