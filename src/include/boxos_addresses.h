#ifndef BOXOS_ADDRESSES_H
#define BOXOS_ADDRESSES_H

// Centralized address definitions for special pages mapped in userspace.
// See: docs/core/memory_cabin.md

#include "cabin_layout.h"

// Core system pages (from cabin_layout.h)
#define BOX_NOTIFY_PAGE_ADDR      CABIN_NOTIFY_PAGE_ADDR   // 0x1000
#define BOX_RESULT_PAGE_ADDR      CABIN_RESULT_PAGE_ADDR   // 0x2000

// CPU Capabilities Page
// Address: 0x7FFFF000 (before stack at 0x7FFFFFFFE000)
// Rationale:
// - CANNOT be at 0x3000 (conflicts with ELF code segment)
// - Placed high in address space, before user stack
// - Leaves ~2GB space for heap growth (0x3000 to 0x7FFFF000)
#define BOX_CPU_CAPS_PAGE_ADDR    0x0000000007FFFF000ULL

// User stack (from vmm.h)
#define BOX_USER_STACK_TOP        0x00007FFFFFFFE000ULL

#endif // BOXOS_ADDRESSES_H
