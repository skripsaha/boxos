#include "hourglass.h"
#include "cpu_calibrate.h"
#include "pmtimer.h"
#include "pit.h"
#include "io.h"
#include "atomics.h"

#define PMT_TSC_PER_TICK_CEIL       2794ULL
#define PIT2_TSC_PER_TICK_CEIL      8381ULL

#define RULER_DEATH_TICKS           8ULL

#define PIT2_SPEAKER_PORT   0x61
#define PIT2_GATE_BIT       0x01
#define PIT2_SPEAKER_BIT    0x02
#define PIT2_CMD_SELECT     0x80
#define PIT2_CMD_LATCH      0x00
#define PIT2_CMD_MODE0      0x00
#define PIT2_MASK           0xFFFFu

static void pit2_start(void)
{
    outb(PIT2_SPEAKER_PORT,
         (uint8_t)((inb(PIT2_SPEAKER_PORT) & (uint8_t)~PIT2_SPEAKER_BIT) | PIT2_GATE_BIT));
    outb(PIT_COMMAND, PIT2_CMD_SELECT | PIT_CMD_RW_BOTH | PIT2_CMD_MODE0 | PIT_CMD_BINARY);
    outb(PIT_CHANNEL2, 0x00);
    outb(PIT_CHANNEL2, 0x00);
}

static uint32_t pit2_sample(void)
{
    outb(PIT_COMMAND, PIT2_CMD_SELECT | PIT2_CMD_LATCH);
    uint32_t lo = inb(PIT_CHANNEL2);
    uint32_t hi = inb(PIT_CHANNEL2);
    return (0u - ((hi << 8) | lo)) & PIT2_MASK;
}


static uint32_t ruler_sample(HourGlassRuler r)
{
    if (r == HOURGLASS_RULER_PMTIMER) {
        uint32_t v = 0;
        pmtimer_read(&v);
        return v;
    }
    if (r == HOURGLASS_RULER_PIT8254)
        return pit2_sample();
    return 0;
}

static bool ruler_is_alive(HourGlassRuler r, uint32_t mask, uint64_t stall)
{
    uint32_t first = ruler_sample(r);
    uint64_t t0    = rdtsc();

    for (;;) {
        if (((ruler_sample(r) - first) & mask) != 0)
            return true;
        if (rdtsc() - t0 <= stall) {
            __asm__ volatile("pause");
            continue;
        }
        return ((ruler_sample(r) - first) & mask) != 0;
    }
}

typedef enum { VERDICT_UNASKED = 0, VERDICT_ALIVE, VERDICT_DEAD } RulerVerdict;

static RulerVerdict s_pmt_verdict  = VERDICT_UNASKED;
static RulerVerdict s_pit2_verdict = VERDICT_UNASKED;

static uint64_t s_tsc_hz = 0;

static uint64_t tsc_arch_hz(void)
{
    if (s_tsc_hz == 0)
        s_tsc_hz = cpu_tsc_architectural_khz() * 1000ULL;
    return s_tsc_hz;
}


static uint64_t us_to_ticks(uint64_t us, uint64_t hz)
{
    uint64_t ticks = (us / 1000000ULL) * hz
                   + ((us % 1000000ULL) * hz + 999999ULL) / 1000000ULL;
    return ticks ? ticks : 1ULL;
}

static bool glass_arm(HourGlass *g, uint64_t us, HourGlassRuler r)
{
    g->ruler   = HOURGLASS_RULER_NONE;
    g->span    = 0;
    g->elapsed = 0;
    g->hz      = 0;
    g->mark    = 0;
    g->witness = 0;
    g->stall   = 0;
    g->last    = 0;
    g->mask    = 0;
    g->died    = false;

    if (r == HOURGLASS_RULER_TSC) {
        uint64_t hz = tsc_arch_hz();
        if (hz == 0)
            return false;
        g->ruler = r;
        g->hz    = hz;
        g->span  = us_to_ticks(us, hz);
        g->mark  = rdtsc();
        return true;
    }

    if (r == HOURGLASS_RULER_PMTIMER) {
        if (!pmtimer_is_present())
            return false;
        uint32_t mask  = pmtimer_mask();
        uint64_t stall = PMT_TSC_PER_TICK_CEIL * RULER_DEATH_TICKS;
        if (s_pmt_verdict == VERDICT_UNASKED)
            s_pmt_verdict = ruler_is_alive(r, mask, stall) ? VERDICT_ALIVE : VERDICT_DEAD;
        if (s_pmt_verdict != VERDICT_ALIVE)
            return false;
        g->ruler = r;
        g->hz    = PMTIMER_FREQ_HZ;
        g->mask  = mask;
        g->stall = stall;
    } else if (r == HOURGLASS_RULER_PIT8254) {
        uint64_t stall = PIT2_TSC_PER_TICK_CEIL * RULER_DEATH_TICKS;
        if (s_pit2_verdict == VERDICT_UNASKED) {
            pit2_start();
            s_pit2_verdict = ruler_is_alive(r, PIT2_MASK, stall) ? VERDICT_ALIVE
                                                                 : VERDICT_DEAD;
        }
        if (s_pit2_verdict != VERDICT_ALIVE)
            return false;
        g->ruler = r;
        g->hz    = PIT_FREQUENCY;
        g->mask  = PIT2_MASK;
        g->stall = stall;
    } else {
        return false;
    }

    g->span    = us_to_ticks(us, g->hz);
    g->last    = ruler_sample(r);
    g->witness = rdtsc();
    return true;
}

bool HourGlassTurn(HourGlass *g, uint64_t us)
{
    if (!g)
        return false;
    if (glass_arm(g, us, HOURGLASS_RULER_TSC))      return true;
    if (glass_arm(g, us, HOURGLASS_RULER_PMTIMER))  return true;
    if (glass_arm(g, us, HOURGLASS_RULER_PIT8254))  return true;
    glass_arm(g, us, HOURGLASS_RULER_NONE);
    return false;
}

bool HourGlassTurnOn(HourGlass *g, uint64_t us, HourGlassRuler ruler)
{
    if (!g)
        return false;
    return glass_arm(g, us, ruler);
}

bool HourGlassRunOut(HourGlass *g)
{
    if (!g || g->ruler == HOURGLASS_RULER_NONE || g->died)
        return true;
    if (g->elapsed >= g->span)
        return true;

    if (g->ruler == HOURGLASS_RULER_TSC) {
        g->elapsed = rdtsc() - g->mark;
        return g->elapsed >= g->span;
    }

    uint32_t now  = ruler_sample(g->ruler);
    uint64_t tsc  = rdtsc();
    uint32_t step = (now - g->last) & g->mask;

    if (step != 0) {
        g->elapsed += step;
        g->last     = now;
        g->witness  = tsc;
        return g->elapsed >= g->span;
    }

    if (tsc - g->witness <= g->stall)
        return false;

    now  = ruler_sample(g->ruler);
    step = (now - g->last) & g->mask;
    if (step != 0) {
        g->elapsed += step;
        g->last     = now;
        g->witness  = rdtsc();
        return g->elapsed >= g->span;
    }

    g->died = true;
    return true;
}

uint64_t HourGlassElapsedUs(const HourGlass *g)
{
    if (!g || g->hz == 0)
        return 0;
    uint64_t whole = g->elapsed / g->hz;
    uint64_t rem   = g->elapsed % g->hz;
    return whole * 1000000ULL + (rem * 1000000ULL) / g->hz;
}

bool HourGlassSourceDied(const HourGlass *g)
{
    return g ? g->died : false;
}

const char *HourGlassRulerName(const HourGlass *g)
{
    if (!g)
        return "none";
    switch (g->ruler) {
        case HOURGLASS_RULER_TSC:     return "TSC (CPUID.15h)";
        case HOURGLASS_RULER_PMTIMER: return "ACPI PM Timer";
        case HOURGLASS_RULER_PIT8254: return "8254 channel 2";
        default:                      return "none";
    }
}