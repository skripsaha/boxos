#include "hpet.h"
#include "acpi.h"
#include "klib.h"
#include "vmm.h"


typedef struct {
    volatile uint8_t *base_virt;
    uint64_t period_fs;
    uint64_t ticks_per_us;
    uint64_t origin_ticks;
    uint8_t  num_timers;
    bool     counter64;
    bool     present;
    bool     leg_rt_cap;
    bool     tick_active;
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

    volatile void *map = vmm_map_mmio(info->base, 4096,
                                       VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE |
                                       VMM_FLAG_CACHE_DISABLE);
    if (!map) {
        debug_printf("[HPET] vmm_map_mmio(0x%lx) failed\n",
                     (unsigned long)info->base);
        return false;
    }
    g_hpet.base_virt = (volatile uint8_t *)map;

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

    g_hpet.ticks_per_us = 1000000000ULL / period;
    if (g_hpet.ticks_per_us == 0) g_hpet.ticks_per_us = 1;

    uint32_t gconf = hpet_read32(HPET_REG_GCONF);
    hpet_write32(HPET_REG_GCONF, gconf & ~(HPET_GCONF_ENABLE | HPET_GCONF_LEG_RT));

    if (g_hpet.counter64) {
        hpet_write64(HPET_REG_MCNT, 0);
    } else {
        hpet_write32(HPET_REG_MCNT, 0);
        hpet_write32(HPET_REG_MCNT + 4, 0);
    }

    for (uint8_t i = 0; i < g_hpet.num_timers; i++) {
        uint64_t cnf = hpet_read64(HPET_REG_TIMER_CNF(i));
        cnf &= ~(uint64_t)HPET_TIMER_INT_ENB;
        hpet_write64(HPET_REG_TIMER_CNF(i), cnf);
    }

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

bool hpet_start_legacy_tick(uint32_t hz) {
    if (!g_hpet.present || hz == 0) return false;
    if (!g_hpet.leg_rt_cap) {
        debug_printf("[HPET] LegacyReplacement not supported by controller\n");
        return false;
    }
    if (g_hpet.num_timers < 1) return false;

    uint64_t t0cnf = hpet_read64(HPET_REG_TIMER_CNF(0));
    if (!(t0cnf & HPET_TIMER_PER_INT_CAP)) {
        debug_printf("[HPET] timer 0 is not periodic-capable\n");
        return false;
    }

    uint64_t period_us = 1000000ULL / (uint64_t)hz;
    if (period_us == 0) period_us = 1;
    uint64_t interval = period_us * g_hpet.ticks_per_us;
    if (interval == 0) return false;

    uint32_t gconf = hpet_read32(HPET_REG_GCONF);
    hpet_write32(HPET_REG_GCONF, gconf & ~(HPET_GCONF_ENABLE | HPET_GCONF_LEG_RT));

    uint64_t new_t0cnf = HPET_TIMER_INT_ENB
                         | HPET_TIMER_TYPE_PERIODIC
                         | HPET_TIMER_VAL_SET;
    if (!g_hpet.counter64) new_t0cnf |= HPET_TIMER_32BIT_MODE;
    hpet_write64(HPET_REG_TIMER_CNF(0), new_t0cnf);

    uint64_t now = hpet_read_mcnt();
    hpet_write64(HPET_REG_TIMER_CMP(0), now + interval);
    hpet_write64(HPET_REG_TIMER_CMP(0), interval);

    hpet_write32(HPET_REG_GCONF, HPET_GCONF_ENABLE | HPET_GCONF_LEG_RT);

    g_hpet.tick_active = true;
    g_hpet.tick_hz     = hz;
    debug_printf("[HPET] legacy tick active: %u Hz, interval=%lu ticks (period_us=%lu)\n",
                 hz, (unsigned long)interval, (unsigned long)period_us);
    return true;
}