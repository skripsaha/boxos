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
 * Atomicity: the main counter is free-running, and how it must be read
 * depends on its WIDTH — the opposite way round from what stood here.
 *
 *   64-bit counter. HPET 1.0a §2.4.7, closing Note: "On a 64-bit platform,
 *   if software attempts a 64 bit read of the 64-bit counter, software must
 *   be aware that some platforms may break the 64 bit read into two 32 bit
 *   reads, and therefore the read may be inaccurate if the low 32 bits roll
 *   over between the high and low part reads." That is where the spec's own
 *   "high, low, high again" retry belongs, and that is where hpet_read_mcnt()
 *   now puts it. No sentence in the specification promises that the upper
 *   half of a 64-bit MMIO read stays stable; the promise this comment used
 *   to make was invented, and the guard it justified sat on the one branch
 *   where the spec says it can never fire.
 *
 *   32-bit counter. §2.3.7 rule 4: "32-bit counters will always return 0 for
 *   the upper 32-bits of this register." There is nothing to retry against —
 *   the upper half never changes. What such a counter does instead is wrap,
 *   every 2^32 ticks, and the laps are carried in software by hpet_extend32().
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
    uint64_t tick_interval;    /* main-counter ticks between timer-0 interrupts */
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

/* Software 64-bit extension of a 32-bit main counter.
 *
 * A controller that reports GCAP.COUNT_SIZE_CAP = 0 — AMD SB/FCH
 * southbridges ship exactly that, and Linux prints them as "32-bit
 * 14.318180 MHz counter" — rolls over every 2^32 ticks, which is 299.96 s
 * of uptime. Subtracting two raw samples across that point is what turned
 * this kernel's only monotonic clock into a ~1.3e18 us spike followed by
 * five minutes of time running BACKWARDS, with any hpet_busy_wait_us() in
 * flight spinning out the whole lap on the core that called it.
 *
 * Every sample is folded into a 64-bit accumulator as a delta taken in
 * 32-bit arithmetic — the same contract Linux's clocksource layer keeps for
 * the HPET with .mask = CLOCKSOURCE_MASK(32). It carries one obligation,
 * and the obligation belongs to the counter rather than to this code:
 * SOMETHING must read the counter more than once per wrap, because a
 * counter that is 32 bits wide cannot say how many laps it ran unobserved.
 * Linux names the same bound clocksource.max_idle_ns and caps how long the
 * tick may sleep at it. Here a gap wider than half a wrap cannot be told
 * apart from a sample taken behind the accumulator, so it is reported as no
 * advance: the clock stands still for that read rather than jumping, in
 * either direction. A 64-bit counter has no such ceiling and does not go
 * through here at all.
 *
 * Lock-free because every core reads this clock: the accumulator only ever
 * moves forward, by a CAS whose candidate was computed from the very value
 * it replaces. A core that loses the race recomputes against the winner's
 * value instead of overwriting it. */
#define HPET_EXT_HALF_LAP   0x80000000ULL

static uint64_t g_hpet_ext = 0;

static uint64_t hpet_extend32(uint32_t now_lo) {
    uint64_t last = __atomic_load_n(&g_hpet_ext, __ATOMIC_ACQUIRE);
    for (;;) {
        uint32_t delta = now_lo - (uint32_t)last;
        if (delta == 0 || (uint64_t)delta >= HPET_EXT_HALF_LAP) {
            return last;
        }
        uint64_t next = last + (uint64_t)delta;
        if (__atomic_compare_exchange_n(&g_hpet_ext, &last, next, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return next;
        }
    }
}

/* The main counter exactly as the hardware holds it. This is the value a
 * comparator must be programmed against, because Tn_CMP is compared with
 * the hardware counter and knows nothing of the software laps. */
static uint64_t hpet_read_mcnt_raw(void) {
    if (g_hpet.counter64) {
        /* HPET 1.0a §2.4.7: "Software can read the high 32 bits, then the
         * low 32 bits, then the high 32 bits again. If the high 32 bits have
         * not changed between the two reads, then a rollover has not
         * happened and the low 32 bits are valid." Three 32-bit reads rather
         * than one 64-bit load because of that section's closing Note: it is
         * the platform, not the instruction, that decides whether a 64-bit
         * read of this register stays whole. */
        for (;;) {
            uint32_t hi  = hpet_read32(HPET_REG_MCNT + 4);
            uint32_t lo  = hpet_read32(HPET_REG_MCNT);
            uint32_t hi2 = hpet_read32(HPET_REG_MCNT + 4);
            if (hi == hi2) {
                return ((uint64_t)hi << 32) | (uint64_t)lo;
            }
        }
    }
    /* §2.3.7 rule 4: the upper half of a 32-bit counter is architecturally
     * zero. Reading it proves nothing. */
    return (uint64_t)hpet_read32(HPET_REG_MCNT);
}

/* The main counter as a 64-bit tick count that never goes backwards: the
 * hardware value at 64 bits, the software lap count at 32. */
static uint64_t hpet_read_mcnt(void) {
    uint64_t raw = hpet_read_mcnt_raw();
    return g_hpet.counter64 ? raw : hpet_extend32((uint32_t)raw);
}

bool hpet_init(void) {
    g_hpet.present = false;

    const acpi_hpet_info_t *info = acpi_get_hpet();
    if (!info) {
        kprintf("[HPET] ACPI describes no HPET on this machine — there is no "
                "high-resolution counter here, so IRQ0 stays with the 8254 and "
                "every time this kernel reports is counted out of those "
                "interrupts\n");
        return false;
    }
    if (info->base == 0) {
        kprintf("[HPET] the ACPI HPET table places the controller at address "
                "zero, which is nowhere — firmware left the table unfilled, and "
                "this kernel will not go looking for it\n");
        return false;
    }

    /* Map the 4 KB register block as UC MMIO. Intel HPET spec §2.3.5
     * mandates the controller registers be uncacheable. */
    volatile void *map = vmm_map_mmio(info->base, 4096,
                                       VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE |
                                       VMM_FLAG_CACHE_DISABLE);
    if (!map) {
        kprintf("[HPET] the register block firmware placed at 0x%lx could not be "
                "mapped — this boot has no high-resolution counter, and IRQ0 "
                "stays with the 8254\n", (unsigned long)info->base);
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
        kprintf("[HPET] the controller states %lu fs per tick, outside the "
                "1..100000000 fs the specification allows (HPET 1.0a 2.3.4) — "
                "firmware never initialised it, and every time derived from a "
                "period like that would be wrong, so it is left alone\n",
                (unsigned long)period);
        return false;
    }

    g_hpet.period_fs    = period;
    g_hpet.counter64    = (gcap & HPET_GCAP_COUNT_SIZE_CAP) != 0;
    g_hpet.leg_rt_cap   = (gcap & HPET_GCAP_LEG_RT_CAP) != 0;
    g_hpet.num_timers   = (uint8_t)(((gcap >> HPET_GCAP_NUM_TIM_SHIFT)
                                     & HPET_GCAP_NUM_TIM_MASK) + 1);

    /* Disable, clear, then enable the main counter. hpet_init() itself
     * leaves LEG_RT_CNF clear — at this point the 8254 still owns IRQ0 and
     * the RTC still owns IRQ8, and the decision to take those pins belongs
     * to hpet_start_legacy_tick() further down this file, which is what
     * sets the bit. HPET 1.0a §2.4.2.1 is why that is a decision and not a
     * collision: with LEG_RT_CNF set, timer 0 is forced onto IRQ0/INTI2 and
     * "the 8254 timer will not cause any interrupts", timer 1 onto IRQ8 and
     * "the RTC will not cause any interrupts". */
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

    /* The software lap counter starts where the hardware counter was just
     * put, so the first extended read is the first tick after zero. */
    __atomic_store_n(&g_hpet_ext, 0, __ATOMIC_RELEASE);

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

    /* The line that lets a board answer for its own clock, and a kprintf
     * because the machine that most needs answering is the one whose only
     * diagnostic is a photograph of the screen.
     *
     * Frequency is 1e15 fs/s divided by the period the controller states, so
     * the canonical part reads 14318180 Hz and not the "0 MHz" an older line
     * printed off a truncated ratio. The WIDTH is spelled out with its
     * consequence rather than tagged, because the width is what decides how a
     * sample is taken — three MMIO reads or one — and whether the counter
     * wraps inside a session. */
    uint64_t counter_hz = 1000000000000000ULL / g_hpet.period_fs;
    if (g_hpet.counter64) {
        kprintf("[HPET] the counter runs at %lu Hz (the controller states %lu fs "
                "per tick) and carries %u comparators; it is 64 bits wide, so "
                "every sample costs three MMIO reads to prove the two halves "
                "belong together, and it cannot wrap inside this machine's "
                "lifetime\n",
                (unsigned long)counter_hz, (unsigned long)g_hpet.period_fs,
                g_hpet.num_timers);
    } else {
        /* 2^32 ticks at the stated period. The product cannot overflow: the
         * period is bounded above by 1e8 fs two checks up, so 2^32 * period
         * is at most 4.3e17. */
        uint64_t wrap_s = (0x100000000ULL * g_hpet.period_fs)
                          / 1000000000000000ULL;
        kprintf("[HPET] the counter runs at %lu Hz (the controller states %lu fs "
                "per tick) and carries %u comparators; it is only 32 bits wide, "
                "so a sample is a single MMIO read, but it wraps every %lu s and "
                "this kernel counts the laps itself — something must read it "
                "more often than that or time stands still\n",
                (unsigned long)counter_hz, (unsigned long)g_hpet.period_fs,
                g_hpet.num_timers, (unsigned long)wrap_s);
    }
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
    /* The subtraction is safe at both widths because hpet_read_mcnt() never
     * goes backwards: on a 64-bit counter origin_ticks is a value the
     * hardware already passed, and on a 32-bit one it is a value the
     * accumulator already held. */
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

uint32_t hpet_tick_hz(void) {
    if (!g_hpet.tick_active || g_hpet.tick_interval == 0) return 0;
    /* One tick period in femtoseconds: the interval this kernel wrote into
     * the comparator times the period the controller states. That product is
     * the requested period in microseconds times 1e9 by construction, so it
     * stays under 1e15 and cannot overflow. Rounded to nearest, because the
     * rate reported must be the closest whole hertz to what the silicon will
     * deliver rather than a truncation of it. */
    uint64_t tick_fs = g_hpet.tick_interval * g_hpet.period_fs;
    if (tick_fs == 0) return 0;
    return (uint32_t)((1000000000000000ULL + tick_fs / 2) / tick_fs);
}

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
        kprintf("[HPET] this controller cannot route LegacyReplacement "
                "(GCAP.LEG_RT_CAP is 0), so IRQ0 stays with the 8254 and the "
                "system tick comes from the chip this kernel programs "
                "itself\n");
        return false;
    }
    if (g_hpet.num_timers < 1) return false;

    /* Check timer 0 periodic capability. */
    uint64_t t0cnf = hpet_read64(HPET_REG_TIMER_CNF(0));
    if (!(t0cnf & HPET_TIMER_PER_INT_CAP)) {
        kprintf("[HPET] timer 0 on this controller is not periodic-capable "
                "(Tn_CNF.PER_INT_CAP is 0), so it cannot carry a system tick "
                "and IRQ0 stays with the 8254\n");
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
     * (current + interval); second write seeds the periodic increment. The
     * counter is taken RAW here: Tn_CMP is matched against the hardware
     * counter, so the software lap count hpet_read_mcnt() carries on a
     * 32-bit controller would seed a deadline the comparator never meets. */
    uint64_t now = hpet_read_mcnt_raw();
    hpet_write64(HPET_REG_TIMER_CMP(0), now + interval);
    hpet_write64(HPET_REG_TIMER_CMP(0), interval);

    /* Step 5 — enable counter + legacy route. */
    hpet_write32(HPET_REG_GCONF, HPET_GCONF_ENABLE | HPET_GCONF_LEG_RT);

    g_hpet.tick_active   = true;
    g_hpet.tick_interval = interval;

    /* The rate that will actually arrive, derived back out of the interval
     * just written, rather than the rate that was asked for: the two differ
     * whenever the requested period is not a whole number of counter ticks,
     * and a log that repeats the request back has reported nothing. */
    kprintf("[HPET] timer 0 now drives IRQ0 at %u Hz through LegacyReplacement, "
            "one interrupt every %lu counter ticks — the 8254 is off that line "
            "entirely (HPET 1.0a 2.4.2.1), and from here this counter is both "
            "the system tick and the only monotonic clock this kernel has\n",
            hpet_tick_hz(), (unsigned long)interval);
    return true;
}
