#include "hpet.h"
#include "acpi.h"
#include "klib.h"
#include "vmm.h"

/*
 * HPET driver implementation.
 *
 * Layout in g_hpet:
 *   base_virt     — UC MMIO mapping of the HPET register block
 *   period_fs     — femtoseconds per main-counter tick, exactly as the
 *                   controller reports it. Every conversion in this file
 *                   goes through this number; nothing is pre-divided into
 *                   a ratio, because the canonical IA-PC period does not
 *                   divide evenly and a ratio throws the remainder away
 *   counter64     — true when the controller's main counter is 64-bit
 *   num_timers    — comparator count (n+1 in spec)
 *
 * Atomicity: the main counter is monotonic and free-running. On real HW
 * a 64-bit MMIO read can be split (e.g. UEFI vmware/qemu mappings sometimes
 * tear), but Intel's spec mandates the upper half stays stable as long as
 * the lower half wraps less than once per read pair. We implement the
 * canonical "lo, hi, lo' — retry if hi changed" guard so the public API
 * never returns a non-monotonic sample.
 */

typedef struct {
    volatile uint8_t *base_virt;
    uint64_t period_fs;        /* femtoseconds per main-counter tick */
    uint64_t origin_ticks;     /* main-counter value captured at init */
    uint8_t  num_timers;
    bool     counter64;
    bool     present;
    bool     leg_rt_cap;       /* LegacyReplacement-route capable per GCAP */
    bool     tick_active;      /* periodic legacy tick currently running */
    uint32_t tick_hz;
} hpet_state_t;

static hpet_state_t g_hpet = { 0 };

static inline uint32_t hpet_read32(uint32_t off) {
    return *(volatile uint32_t *)(g_hpet.base_virt + off);
}
static inline void hpet_write32(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g_hpet.base_virt + off) = v;
}
static inline uint64_t hpet_read64(uint32_t off) {
    return *(volatile uint64_t *)(g_hpet.base_virt + off);
}
static inline void hpet_write64(uint32_t off, uint64_t v) {
    *(volatile uint64_t *)(g_hpet.base_virt + off) = v;
}

/* One microsecond, in the femtoseconds the controller reports its period in. */
#define HPET_FS_PER_US   1000000000ULL

/* Main-counter ticks → microseconds, exactly, in 64-bit arithmetic.
 *
 * us = ticks * period_fs / 1e9. This used to be pre-divided into an integer
 * ticks-per-microsecond, and on every IA-PC board in existence — period
 * 69,841,279 fs, 14.31818 MHz, four times the NTSC colour subcarrier —
 * 1e9 / 69,841,279 truncates to 14 against a true 14.31818. That threw away
 * 22,727 ppm of a field the HPET specification (1.0a §2.3.4) expresses in
 * femtoseconds expressly "in order to achieve a resolution of 50 ppm", on
 * silicon §2.2 holds to 500 ppm. QEMU hid it: its period is exactly 10 ns
 * and divides 1e9 without remainder.
 *
 * The split keeps the product inside 64 bits with no 128-bit divide:
 *
 *   ticks * P / 1e9  =  (ticks / 1e9) * P  +  ((ticks % 1e9) * P) / 1e9
 *
 * The left term cannot overflow — P <= 1e8 by the spec bound checked at
 * init, so it would take more than 2^64 ticks — and the right term is
 * bounded by 1e9 * 1e8 = 1e17. Exact across the whole 64-bit tick range,
 * which at 14.318 MHz is some forty thousand years. */
static uint64_t hpet_ticks_to_us(uint64_t ticks) {
    uint64_t whole = ticks / HPET_FS_PER_US;
    uint64_t rem   = ticks % HPET_FS_PER_US;
    return whole * g_hpet.period_fs
         + (rem * g_hpet.period_fs) / HPET_FS_PER_US;
}

/* Read the 64-bit main counter, safe against split reads on 32-bit-only
 * controllers and against torn MMIO mappings. The "lo, hi, lo' compare"
 * pattern is the canonical x86 way (Intel SDM Vol 3 §17.17.2 uses the same
 * trick for TSC, the HPET applies identically). */
static uint64_t hpet_read_mcnt(void) {
    if (g_hpet.counter64) {
        return hpet_read64(HPET_REG_MCNT);
    }
    for (;;) {
        uint32_t hi  = hpet_read32(HPET_REG_MCNT + 4);
        uint32_t lo  = hpet_read32(HPET_REG_MCNT);
        uint32_t hi2 = hpet_read32(HPET_REG_MCNT + 4);
        if (hi == hi2) {
            return ((uint64_t)hi << 32) | (uint64_t)lo;
        }
    }
}

bool hpet_init(void) {
    g_hpet.present = false;

    const acpi_hpet_info_t *info = acpi_get_hpet();
    if (!info) {
        debug_printf("[HPET] ACPI did not expose an HPET table\n");
        return false;
    }
    if (info->base == 0) {
        debug_printf("[HPET] HPET base address is zero\n");
        return false;
    }

    /* Map the 4 KB register block as UC MMIO. Intel HPET spec §2.3.5
     * mandates the controller registers be uncacheable. */
    volatile void *map = vmm_map_mmio(info->base, 4096,
                                       VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE |
                                       VMM_FLAG_CACHE_DISABLE);
    if (!map) {
        debug_printf("[HPET] vmm_map_mmio(0x%lx) failed\n",
                     (unsigned long)info->base);
        return false;
    }
    g_hpet.base_virt = (volatile uint8_t *)map;

    /* Read general capabilities. Period is encoded in bits 63:32 of GCAP.
     * Per spec §2.3.4 it must be 1..0x05F5E100 femtoseconds — 0x05F5E100
     * is 10^8 fs, which is 100 ns, not the 10 ns this comment claimed; the
     * bound was always right and the parenthetical was off by ten. Any
     * other value means the firmware did not initialise the controller and
     * we should not trust it. */
    uint64_t gcap = hpet_read64(HPET_REG_GCAP_ID);
    uint64_t period = gcap >> HPET_GCAP_PERIOD_SHIFT;
    if (period == 0 || period > 0x05F5E100ULL) {
        debug_printf("[HPET] GCAP period 0x%lx out of spec range\n",
                     (unsigned long)period);
        return false;
    }

    g_hpet.period_fs    = period;
    g_hpet.counter64    = (gcap & HPET_GCAP_COUNT_SIZE_CAP) != 0;
    g_hpet.leg_rt_cap   = (gcap & HPET_GCAP_LEG_RT_CAP) != 0;
    g_hpet.num_timers   = (uint8_t)(((gcap >> HPET_GCAP_NUM_TIM_SHIFT)
                                     & HPET_GCAP_NUM_TIM_MASK) + 1);

    /* Disable, clear, then enable the main counter. We do NOT enable
     * LegacyReplacement Mode — that takes over IRQ0/IRQ8 routing and
     * would conflict with the existing PIT and RTC drivers. */
    uint32_t gconf = hpet_read32(HPET_REG_GCONF);
    hpet_write32(HPET_REG_GCONF, gconf & ~(HPET_GCONF_ENABLE | HPET_GCONF_LEG_RT));

    /* Reset the main counter atomically by writing 0 to MCNT. The whole
     * counter is writable only while ENABLE_CNF is clear. */
    if (g_hpet.counter64) {
        hpet_write64(HPET_REG_MCNT, 0);
    } else {
        hpet_write32(HPET_REG_MCNT, 0);
        hpet_write32(HPET_REG_MCNT + 4, 0);
    }

    /* Mask every comparator so a leftover firmware programmation cannot
     * fire a stale interrupt. INT_ENB bit 2 cleared. */
    for (uint8_t i = 0; i < g_hpet.num_timers; i++) {
        uint64_t cnf = hpet_read64(HPET_REG_TIMER_CNF(i));
        cnf &= ~(uint64_t)HPET_TIMER_INT_ENB;
        hpet_write64(HPET_REG_TIMER_CNF(i), cnf);
    }

    /* Enable the main counter. */
    hpet_write32(HPET_REG_GCONF, HPET_GCONF_ENABLE);

    g_hpet.origin_ticks = hpet_read_mcnt();
    g_hpet.present      = true;

    /* Frequency for the log line is 1e15 fs/s divided by the period — in
     * kHz, so the canonical part reads 14318 kHz and not the "0 MHz" the
     * old line printed by deriving it from the same truncated 1e9 ratio. */
    debug_printf("[HPET] online: period=%lu fs (%lu kHz), %u timers, %s counter\n",
                 (unsigned long)g_hpet.period_fs,
                 (unsigned long)(1000000000000000ULL / g_hpet.period_fs / 1000ULL),
                 g_hpet.num_timers,
                 g_hpet.counter64 ? "64-bit" : "32-bit");
    return true;
}

bool hpet_is_present(void) {
    return g_hpet.present;
}

uint64_t hpet_period_fs(void) {
    return g_hpet.period_fs;
}

uint8_t hpet_num_comparators(void) {
    return g_hpet.num_timers;
}

bool hpet_counter_is_64bit(void) {
    return g_hpet.counter64;
}

uint64_t hpet_now_us(void) {
    if (!g_hpet.present) return 0;
    uint64_t now = hpet_read_mcnt();
    return hpet_ticks_to_us(now - g_hpet.origin_ticks);
}

void hpet_busy_wait_us(uint64_t us) {
    if (!g_hpet.present) return;
    uint64_t deadline = hpet_now_us() + us;
    while (hpet_now_us() < deadline) {
        __asm__ volatile("pause");
    }
}

bool hpet_tick_active(void) { return g_hpet.tick_active; }

/* Program timer 0 in periodic mode at `hz` Hz and enable
 * LegacyReplacement routing. The reference for the sequence below is
 * Intel HPET Specification 1.0a §2.3.9.2 and §3.2.5.
 *
 * Sequence (with the main counter momentarily stopped):
 *   1.  GCONF.ENABLE = 0      — halt main counter
 *   2.  Tn_CONF      = TYPE_PERIODIC | INT_ENB | VAL_SET (level=0)
 *   3.  Tn_CMP       = current_counter + ticks_per_period
 *   4.  Tn_CMP       = ticks_per_period   (second write programs the
 *                                          auto-increment in periodic+VAL_SET)
 *   5.  GCONF.ENABLE | GCONF.LEG_RT — start counter, route legacy IRQs
 *
 * After this, every `ticks_per_period` main-counter ticks the timer
 * raises an interrupt that LegacyReplacement routes to ISA IRQ0
 * (timer 1 would route to IRQ8 — left disabled here).
 */
bool hpet_start_legacy_tick(uint32_t hz) {
    if (!g_hpet.present || hz == 0) return false;
    if (!g_hpet.leg_rt_cap) {
        debug_printf("[HPET] LegacyReplacement not supported by controller\n");
        return false;
    }
    if (g_hpet.num_timers < 1) return false;

    /* Check timer 0 periodic capability. */
    uint64_t t0cnf = hpet_read64(HPET_REG_TIMER_CNF(0));
    if (!(t0cnf & HPET_TIMER_PER_INT_CAP)) {
        debug_printf("[HPET] timer 0 is not periodic-capable\n");
        return false;
    }

    /* Compute the interval in main-counter ticks: ticks = us * 1e9 / period_fs,
     * rounded to NEAREST so the tick carries no systematic lead or lag. The
     * truncated ticks-per-microsecond this used to multiply by made a 250 Hz
     * request arrive at 255.68 Hz on canonical silicon while pit_tick went on
     * crediting 4000 us per interrupt. hz >= 1 is ensured above, so period_us
     * is at most 1e6 and the product at most 1e15 — no overflow. */
    uint64_t period_us = 1000000ULL / (uint64_t)hz;
    if (period_us == 0) period_us = 1;
    uint64_t interval = (period_us * HPET_FS_PER_US + g_hpet.period_fs / 2)
                        / g_hpet.period_fs;
    if (interval == 0) return false;

    /* Step 1 — halt counter, clearing LEG_RT as a side effect so we
     * don't end up with a half-configured legacy route. */
    uint32_t gconf = hpet_read32(HPET_REG_GCONF);
    hpet_write32(HPET_REG_GCONF, gconf & ~(HPET_GCONF_ENABLE | HPET_GCONF_LEG_RT));

    /* Step 2 — timer 0 config:
     *   TYPE_PERIODIC | INT_ENB | VAL_SET
     *   trigger = edge (TIMER_INT_TYPE = 0)
     *   delivery to LegacyReplacement; INT_ROUTE bits ignored when
     *   LEG_RT is enabled in GCONF, so we don't need to touch them. */
    uint64_t new_t0cnf = HPET_TIMER_INT_ENB
                         | HPET_TIMER_TYPE_PERIODIC
                         | HPET_TIMER_VAL_SET;
    /* Preserve the 32-bit-mode bit if the controller is 32-bit-only. */
    if (!g_hpet.counter64) new_t0cnf |= HPET_TIMER_32BIT_MODE;
    hpet_write64(HPET_REG_TIMER_CNF(0), new_t0cnf);

    /* Steps 3-4 — write comparator twice. First write seeds the deadline
     * (current + interval); second write seeds the periodic increment. */
    uint64_t now = hpet_read_mcnt();
    hpet_write64(HPET_REG_TIMER_CMP(0), now + interval);
    hpet_write64(HPET_REG_TIMER_CMP(0), interval);

    /* Step 5 — enable counter + legacy route. */
    hpet_write32(HPET_REG_GCONF, HPET_GCONF_ENABLE | HPET_GCONF_LEG_RT);

    g_hpet.tick_active = true;
    g_hpet.tick_hz     = hz;
    debug_printf("[HPET] legacy tick active: %u Hz, interval=%lu ticks (period_us=%lu)\n",
                 hz, (unsigned long)interval, (unsigned long)period_us);
    return true;
}
