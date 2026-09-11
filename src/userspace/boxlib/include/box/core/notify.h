#ifndef BOX_CORE_NOTIFY_H
#define BOX_CORE_NOTIFY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/core/pocket.h"

#define __notify() \
    __asm__ volatile("syscall" : : "D"(0) : "memory", "rax", "rcx", "r11")

void pocket_prepare(Pocket* p);

int pocket_submit(Pocket* p);

void yield(void);

#ifdef __cplusplus
}
#endif

#endif