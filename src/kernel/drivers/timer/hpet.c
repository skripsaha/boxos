#include "hpet.h"
#include "acpi.h"
#include "klib.h"
#include "vmm.h"

/*
 * HPET driver implementation.
 *
 * Layout in g_hpet:
 *   base_virt     — UC MMIO mapping of the HPET register block
 *   period_fs     — femtoseconds per main-counter tick
 *   ticks_per_us  — pre-computed conversion factor (always >= 1)
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
    uint64_t period_fs;
    uint64_t ticks_per_us;     /* main-counter ticks in one microsecond */
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
     * Per spec it must be 1..0x05F5E100 femtoseconds (i.e. <=10 ns); any
     * other value means the firmware did not initialise the controller
     * and we should not trust it. */
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

    /* Pre-compute ticks-per-microsecond. One microsecond = 1e9 fs, so
     *   ticks/us = 1_000_000_000 / period_fs
     * Period is bounded by 0x05F5E100 = 100,000,000 fs => ratio >= 10.
     * Period >= 1 fs => ratio <= 1e9. Both fit in uint64_t. */
    g_hpet.ticks_per_us = 1000000000ULL / period;
    if (g_hpet.ticks_per_us == 0) g_hpet.ticks_per_us = 1;   /* sanity */

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

    debug_printf("[HPET] online: period=%lu fs (%lu MHz), %u timers, %s counter\n",
                 (unsigned long)g_hpet.period_fs,
                 (unsigned long)(1000000000ULL / g_hpet.period_fs / 1000ULL),
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
    uint64_t delta = now - g_hpet.origin_ticks;
    /* delta / ticks_per_us — division by a >=10 divisor, never overflows
     * because delta is bounded by uint64_t and ticks_per_us >= 10. */
    return delta / g_hpet.ticks_per_us;
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

    /* Compute interval. ticks_per_us is bounded; 1_000_000/hz fits a
     * uint32_t for any reasonable hz; product fits uint64. Avoid
     * divide-by-zero: hz >= 1 ensured above. */
    uint64_t period_us = 1000000ULL / (uint64_t)hz;
    if (period_us == 0) period_us = 1;
    uint64_t interval = period_us * g_hpet.ticks_per_us;
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
