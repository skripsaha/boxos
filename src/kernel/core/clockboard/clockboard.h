#ifndef CLOCKBOARD_H
#define CLOCKBOARD_H

#include "ktypes.h"
#include "boxos_clockboard.h"   /* ClockBoard, CLOCKBOARD_MAGIC — the page's layout, shared with boxlib */

/*
 * ClockBoard — kernel-published, userspace-readable clock page.
 *
 * One physical page allocated at boot, mapped read-only into every Cabin
 * at CABIN_CLOCKBOARD_ADDR. The IRQ0 handler on the BSP refreshes the dynamic
 * counters per tick, and kernel_main refreshes them once before userspace
 * starts so the first Ring 3 read is not a zero from before the first tick;
 * static fields are written once during early boot. A read from userspace is
 * a plain pointer load — no syscall, no manifest, no ring round-trip.
 *
 * The layout is boxos_clockboard.h's, the one header the kernel and BoxLib
 * both read it from. Reader pattern: check `magic` first; without it (no page
 * mapped) fall back to the manifest path.
 *
 * Counter consistency: uptime_us is a single naturally-aligned 64-bit
 * word, written by exactly one core (BSP). On x86-64 such a store is
 * atomic — a cross-core read sees either the old or the new value,
 * never a torn mix. uptime_ms is derived per-tick from uptime_us so it
 * stays in lock-step. tsc_freq_khz / boot_unix_secs are constant after
 * boot calibration.
 *
 * BoxOS-native answer to the "vDSO time page" idea: routed through the
 * fixed-VA Cabin layout instead of a separate VDSO mapping. The kernel
 * publishes; the userspace consumes via plain pointer read; per-process
 * filtering / derivation lives in the boxlib layer.
 */

/* One-time setup: allocate the physical backing page, fill the
 * magic. Call from kernel_main BEFORE pit_init so the IRQ handler
 * always finds a valid page. Idempotent. */
void clockboard_init(void);

/* Phys address of the shared page. Used by vmm_setup_cabin to map the
 * page R/O into each process's address space at CABIN_CLOCKBOARD_ADDR. */
uint64_t clockboard_phys(void);

/* Copy the kernel's uptime onto the page. Takes no arguments on purpose: the
 * page must carry the same clock the system deck answers with, and a function
 * that accepted a number could be — and was — handed a different one. Called
 * from pit_tick() every tick, and from kernel_main once before the first
 * userspace process can read the page. Single-writer, so plain stores are
 * safe. */
void clockboard_publish_uptime(void);

/* Late-boot setters for the static fields. Called from cpu_calibrate
 * and rtc_init once those values are known. Safe to call repeatedly. */
/* Monotonic uptime in milliseconds for KERNEL callers: the clock itself, not
 * the page. No lock, no syscall, no page walk — so it may be read from any
 * context, including one where taking another lock would itself be a hazard
 * (Touch reads it for its deadlines, Nightwatch from the idle path). It does
 * NOT read the published page, because the page only advances once IRQ0 is
 * arriving and these callers run before that. */
uint64_t clockboard_uptime_ms(void);

void clockboard_set_tsc_freq_khz(uint64_t khz);
void clockboard_set_boot_unix_secs(uint64_t secs);

#endif /* CLOCKBOARD_H */
