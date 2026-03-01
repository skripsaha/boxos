#ifndef BOXOS_ADDRESSES_H
#define BOXOS_ADDRESSES_H

#include "cabin_layout.h"

#define NOTIFY_PAGE_ADDR      CABIN_NOTIFY_PAGE_ADDR   // 0x1000
#define RESULT_PAGE_ADDR      CABIN_RESULT_PAGE_ADDR   // 0x2000

// 0x7FFFF000: before user stack (0x7FFFFFFFE000), after code (0x3000+), leaves ~2GB for heap
#define CPU_CAPS_PAGE_ADDR    0x0000000007FFFF000ULL

#define USER_STACK_TOP        0x00007FFFFFFFE000ULL

#endif // BOXOS_ADDRESSES_H
