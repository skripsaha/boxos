#ifndef KERNEL_CLOCK_H
#define KERNEL_CLOCK_H

#include "ktypes.h"

/*
 * Global monotonic tick counter — incremented atomically by the BSP PIT
 * interrupt handler at SCHEDULER_DEFAULT_TICK_HZ (250 Hz → 4 ms per tick).
 *
 * Defined in scheduler.c. Use kernel_tick_get() for safe cross-core reads.
 * Never use rdtsc() for inter-core timeout comparisons — TSC is per-core
 * and may diverge across C-state transitions.
 */
extern volatile uint64_t g_global_tick;

static inline uint64_t kernel_tick_get(void)
{
    return __atomic_load_n((uint64_t *)&g_global_tick, __ATOMIC_RELAXED);
}

#endif // KERNEL_CLOCK_H
