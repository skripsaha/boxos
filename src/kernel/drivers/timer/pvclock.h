#ifndef PVCLOCK_H
#define PVCLOCK_H

#include "ktypes.h"

/*
 * KVM paravirtual clock (kvmclock).
 *
 * Reference: linux kernel `Documentation/virt/kvm/x86/msr.rst`,
 * §MSR_KVM_SYSTEM_TIME_NEW (0x4b564d01) and §MSR_KVM_WALL_CLOCK_NEW
 * (0x4b564d00); structure pvclock_vcpu_time_info defined in
 * `arch/x86/include/asm/pvclock-abi.h`.
 *
 * The hypervisor publishes a per-VCPU 32-byte memory area carrying a
 * monotonic system_time in nanoseconds and a TSC→ns conversion couple
 * (mul, shift) that linearises rdtsc() against the host clock. A guest
 * read is:
 *
 *   do {
 *       v0     = pvti->version;          // odd = update in flight
 *       if (v0 & 1) continue;
 *       barrier
 *       tsc    = rdtsc();
 *       cycles = tsc - pvti->tsc_timestamp;
 *       if (pvti->tsc_shift >= 0) cycles <<= pvti->tsc_shift;
 *       else                       cycles >>= -pvti->tsc_shift;
 *       ns = ((cycles * pvti->tsc_to_system_mul) >> 32) + pvti->system_time;
 *       barrier
 *       v1     = pvti->version;
 *   } while (v0 != v1);
 *
 * Crucially: this works even when the host TSC is not invariant or
 * when the guest migrates between hosts — the hypervisor updates the
 * structure with the seqlock-style `version` mechanism (odd = mid
 * update, even = stable, must match across read).
 *
 * This driver makes pvclock available as a high-quality clocksource
 * for the entire kernel, used as the preferred input to TSC frequency
 * calibration (cpu_calibrate.c) and as a direct ns time source for
 * any subsystem that wants nanosecond precision.
 */

bool pvclock_init(void);
bool pvclock_is_available(void);

/* Per-AP activation. Each VCPU needs its own pvclock slot (KVM only
 * updates the slot whose physical address THIS VCPU wrote to its
 * MSR_KVM_SYSTEM_TIME_NEW). Called from per_core_init_ap() right
 * after lapic_enable; no-op on bare metal / non-KVM hosts. */
void pvclock_init_ap(uint8_t core_index);

/* Read the current monotonic time in nanoseconds. Returns 0 if
 * pvclock is not initialised. Safe to call from any context (uses the
 * seqlock-style version-pair protocol, no locks). */
uint64_t pvclock_now_ns(void);

/* Read wall-clock seconds + nanoseconds offset (the value the host
 * had when the guest booted, set once by the hypervisor via the
 * separate MSR_KVM_WALL_CLOCK_NEW area). Add to pvclock_now_ns() for
 * the current host wall time. Returns false if not available. */
bool pvclock_walltime(uint64_t *out_sec, uint32_t *out_nsec);

/* TSC kHz derived from the pvclock conversion factors. Exact —
 * computed from the (mul, shift) couple, not a measurement. Returns
 * 0 if pvclock is unavailable or the conversion factor cannot be
 * inverted to a kHz value. */
uint64_t pvclock_tsc_khz(void);

#endif /* PVCLOCK_H */
