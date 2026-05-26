#ifndef HVCLOCK_H
#define HVCLOCK_H

#include "ktypes.h"

/*
 * Hyper-V reference TSC page (TLFS §10.7.2).
 *
 * Modern Microsoft Hyper-V (and Azure VMs) advertises an enlightened
 * monotonic-time facility via a single 4 KB page reachable through
 * MSR HV_X64_MSR_REFERENCE_TSC (0x40000021). The page contains:
 *
 *   struct HV_REFERENCE_TSC_PAGE {
 *       volatile uint32_t TscSequence;     // 0 = invalid; non-zero = seqlock
 *       uint32_t          Reserved1;
 *       volatile uint64_t TscScale;
 *       volatile int64_t  TscOffset;
 *       uint64_t          Reserved2[509];
 *   };
 *
 * Reference time in 100-ns units is computed as:
 *
 *   ReferenceTime = ((rdtsc() * TscScale) >> 64) + TscOffset
 *
 * with 128-bit multiplication (mulq on x86-64). The TscSequence field
 * is a seqlock — guests retry the read whenever the sequence changes
 * between sampling, and treat sequence==0 as "fallback to
 * HV_X64_MSR_TIME_REF_COUNT (0x40000020) MSR rdmsr".
 *
 * This is the recommended clocksource on Hyper-V because it survives
 * live migration (TscScale/TscOffset get rewritten transparently) and
 * costs only a memory read + a mulq — orders of magnitude faster than
 * an MSR read.
 */

bool hvclock_init(void);
bool hvclock_is_available(void);

/* Reference time in nanoseconds (the spec is 100-ns units; we multiply
 * by 100). Monotonic across migration when the underlying iTSC is
 * available; falls back to MSR rdmsr when the seqlock indicates the
 * page is invalid. */
uint64_t hvclock_now_ns(void);

#endif /* HVCLOCK_H */
