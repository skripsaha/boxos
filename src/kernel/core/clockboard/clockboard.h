#ifndef CLOCKBOARD_H
#define CLOCKBOARD_H

#include "ktypes.h"
#include "cabin_layout.h"

/*
 * ClockBoard — kernel-published, userspace-readable clock page.
 *
 * One physical page allocated at boot, mapped read-only into every Cabin
 * at CABIN_CLOCKBOARD_ADDR. The PIT IRQ (BSP only) updates the dynamic
 * counters per tick; static fields are written once during early boot.
 * A read from userspace is a plain pointer load — no syscall, no
 * manifest, no ring round-trip.
 *
 * Layout is versioned so future field additions cannot silently break
 * mismatched userspace boxlibs. Reader pattern: check `magic` then
 * `version` first; on mismatch fall back to the manifest path.
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

#define CLOCKBOARD_MAGIC    0x4b4c4342u    /* 'BCLK' = Box Clock */
#define CLOCKBOARD_VERSION  1u

typedef struct __attribute__((packed)) {
    /* Static (set once during boot, never changes after). */
    uint32_t magic;            /* CLOCKBOARD_MAGIC */
    uint32_t version;          /* CLOCKBOARD_VERSION */

    /* Dynamic — written per PIT tick by BSP. */
    volatile uint64_t uptime_us;     /* monotonic; advanced by 1_000_000/freq µs per tick */
    volatile uint64_t uptime_ms;     /* uptime_us / 1000, refreshed per tick */
    volatile uint64_t tick_count;    /* raw PIT tick counter */

    /* Constant after boot. */
    uint64_t tsc_freq_khz;     /* TSC frequency in kHz, calibrated once */
    uint64_t boot_unix_secs;   /* RTC unix-epoch seconds at boot */

    /* Pad to one full page so the struct sizeof matches the mapping. */
    uint8_t  _reserved[4096 - 4 - 4 - 8 - 8 - 8 - 8 - 8];
} ClockBoard;

_Static_assert(sizeof(ClockBoard) == 4096, "ClockBoard must be exactly one page");

/* One-time setup: allocate the physical backing page, fill magic /
 * version. Call from kernel_main BEFORE pit_init so the IRQ handler
 * always finds a valid page. Idempotent. */
void clockboard_init(void);

/* Phys address of the shared page. Used by vmm_setup_cabin to map the
 * page R/O into each process's address space at CABIN_CLOCKBOARD_ADDR. */
uint64_t clockboard_phys(void);

/* Called from pit_tick(). Updates dynamic counters. Single-writer
 * (BSP IRQ), so plain stores are safe. */
void clockboard_tick_update(uint64_t uptime_us, uint64_t tick_count);

/* Late-boot setters for the static fields. Called from cpu_calibrate
 * and rtc_init once those values are known. Safe to call repeatedly. */
void clockboard_set_tsc_freq_khz(uint64_t khz);
void clockboard_set_boot_unix_secs(uint64_t secs);

#endif /* CLOCKBOARD_H */
