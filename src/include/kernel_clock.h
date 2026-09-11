#ifndef KERNEL_CLOCK_H
#define KERNEL_CLOCK_H

#include "ktypes.h"

extern volatile uint64_t g_global_tick;

static inline uint64_t kernel_tick_get(void)
{
    return __atomic_load_n((uint64_t *)&g_global_tick, __ATOMIC_RELAXED);
}

#endif