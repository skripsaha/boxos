#include "hourglass.h"
#include "cpu_calibrate.h"
#include "pmtimer.h"
#include "pit.h"
#include "io.h"
#include "atomics.h"

/* ----------------------------------------------------------------
 * Death thresholds — how much TSC may pass with a counter standing
 * perfectly still before the counter is declared dead.
 *
 * Each is derived, never guessed:
 *
 *   A live counter of frequency F cannot stand still for longer than
 *   one of its own tick periods, 1/F seconds. The TSC is the only
 *   witness available (see hourglass.h), so the period has to be
 *   expressed in TSC cycles, and that needs a ceiling on the core
 *   clock. Take 10 GHz — no shipping x86-64 part is close, and it is
 *   the same ceiling cpu_calibrate.c already states as the upper
 *   sanity bound on a measured TSC (TSC_SANE_MAX_KHZ = 10000000 kHz).
 *
 *   PM Timer, ACPI 6.5 §4.8.3.3, F = 3579545 Hz:
 *       1/F = 279.365 ns ; 10e9 * 279.365e-9 = 2793.65 → 2794 cycles
 *   8254 channel 2, F = 1193182 Hz:
 *       1/F = 838.095 ns ; 10e9 * 838.095e-9 = 8380.95 → 8381 cycles
 * ---------------------------------------------------------------- */
#define PMT_TSC_PER_TICK_CEIL       2794ULL
#define PIT2_TSC_PER_TICK_CEIL      8381ULL

/* How many of those tick periods must pass, still, before the verdict.
 *
 * One would already be sound arithmetic — a live counter simply cannot
 * hold a value for two tick periods. Eight is taken because the two
 * samples that bracket a verdict are not simultaneous: the counter is
 * read, and only then is the TSC read, and between those two an
 * emulator can trap the I/O access or an SMI can steal the core, which
 * inflates the observed TSC span without the counter being at fault.
 * Eight periods is 22352 cycles on the PM Timer — 22 us on a 1 GHz
 * part, 2.2 us on a 10 GHz one — so the verdict is still reached in
 * microseconds, not milliseconds.
 *
 * The multiplier is a margin, not the proof. The proof is the
 * confirming re-read every verdict passes through below. */
#define RULER_DEATH_TICKS           8ULL

/* ----------------------------------------------------------------
 * 8254 channel 2 as a free-running ruler.
 *
 * Channel 2 is the one channel an OS may take without stealing the
 * system tick: its gate is software-controlled through port 0x61 bit 0
 * and its output drives only the PC speaker, through bit 1. Bit 1 is
 * left CLEAR, so the speaker stays silent and only the gate is raised.
 *
 * Mode 0 loaded with a count of 0 means a full 65536 (Intel 8254
 * datasheet, "Mode 0: Interrupt on Terminal Count" — the counter is
 * 16-bit and 0 is loaded as 2^16), and the counter keeps decrementing
 * and wrapping after it reaches zero. That is a 16-bit down-counter at
 * 1.193182 MHz wrapping every 65536/1193182 = 54.925 ms: a ruler, not
 * a one-shot.
 *
 * amp.c uses this same channel as a one-shot GATE — load a count, then
 * watch port 0x61 bit 5 rise — which is a different mechanism on the
 * same hardware, not the same code wearing two hats. It is deliberately
 * not shared: factoring it would mean editing amp.c's AP bring-up path
 * for no gain to either caller.
 * ---------------------------------------------------------------- */
#define PIT2_SPEAKER_PORT   0x61
#define PIT2_GATE_BIT       0x01    /* 8254 channel 2 GATE input      */
#define PIT2_SPEAKER_BIT    0x02    /* speaker data — must stay clear */
#define PIT2_CMD_SELECT     0x80    /* command byte bits [7:6] = 10   */
#define PIT2_CMD_LATCH      0x00    /* command byte bits [5:4] = 00   */
#define PIT2_CMD_MODE0      0x00    /* command byte bits [3:1] = 000  */
#define PIT2_MASK           0xFFFFu

static void pit2_start(void)
{
    outb(PIT2_SPEAKER_PORT,
         (uint8_t)((inb(PIT2_SPEAKER_PORT) & (uint8_t)~PIT2_SPEAKER_BIT) | PIT2_GATE_BIT));
    outb(PIT_COMMAND, PIT2_CMD_SELECT | PIT_CMD_RW_BOTH | PIT2_CMD_MODE0 | PIT_CMD_BINARY);
    outb(PIT_CHANNEL2, 0x00);
    outb(PIT_CHANNEL2, 0x00);
}

/* Latch and read channel 2, then NEGATE it. The 8254 counts DOWN; every
 * other ruler here counts up, and the glass wants one delta rule for all
 * of them. Two's-complement negation of a 16-bit down-counter is an
 * up-counter of the same period, so (now - last) & mask is the elapsed
 * tick count on this ruler exactly as it is on the others. */
static uint32_t pit2_sample(void)
{
    outb(PIT_COMMAND, PIT2_CMD_SELECT | PIT2_CMD_LATCH);
    uint32_t lo = inb(PIT_CHANNEL2);
    uint32_t hi = inb(PIT_CHANNEL2);
    return (0u - ((hi << 8) | lo)) & PIT2_MASK;
}

/* ----------------------------------------------------------------
 * Sampling and proving
 * ---------------------------------------------------------------- */

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

/* Watch the candidate for `stall` TSC cycles. Bounded both ways: it
 * returns the moment the counter moves, and it returns no later than
 * `stall` cycles from entry even when the counter never moves at all.
 * There is no path through here that spins forever, which is the whole
 * point — the alternative this replaces spun with IF=0 on a counter
 * that would never advance again. */
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
        /* Confirm before condemning. A host that descheduled this vCPU
         * in the gap between the counter read and the rdtsc makes a
         * perfectly live counter look frozen across an enormous TSC
         * span — the counter read happened early, the TSC read happened
         * late, and nothing is wrong. One more read settles it: a live
         * counter cannot have failed to move across the span the TSC
         * has just reported. Only a counter that is still standing on
         * the confirming read is dead. */
        return ((ruler_sample(r) - first) & mask) != 0;
    }
}

/* One verdict per ruler per boot. The probe costs real time, and a
 * counter that was dead at boot does not come back — while a counter
 * that was ALIVE is re-examined on every span anyway by the mid-span
 * test inside HourGlassRunOut, so caching "alive" costs no safety.
 *
 * Deliberately NOT a single cached "the adopted ruler", because the set
 * of rulers grows during boot: the PM Timer does not exist until
 * acpi_parse_tables hands it over, and a glass turned before that must
 * be able to fall to the 8254 without freezing that choice for the rest
 * of the machine's life. HourGlassTurn re-walks the succession every
 * time; the walk is three loads once the verdicts are in.
 *
 * Two cores probing at once both do the work and both store the same
 * verdict — a benign race on a byte, not worth a lock this subsystem
 * has promised not to take. */
typedef enum { VERDICT_UNASKED = 0, VERDICT_ALIVE, VERDICT_DEAD } RulerVerdict;

static RulerVerdict s_pmt_verdict  = VERDICT_UNASKED;
static RulerVerdict s_pit2_verdict = VERDICT_UNASKED;

/* CPUID.15h answer, cached only when it is an ANSWER. A zero is not
 * cached: g_cpu_caps.max_basic_leaf is zero until cpu_detect_features
 * has run, and freezing the pre-detection "no" would cost the machine
 * its best ruler for the rest of the boot. Re-asking is one CPUID, and
 * only on the turn, never in the poll. */
static uint64_t s_tsc_hz = 0;

static uint64_t tsc_arch_hz(void)
{
    if (s_tsc_hz == 0)
        s_tsc_hz = cpu_tsc_architectural_khz() * 1000ULL;
    return s_tsc_hz;
}

/* ----------------------------------------------------------------
 * Turning the glass
 * ---------------------------------------------------------------- */

/* us → ruler ticks, rounded UP, never zero.
 *
 * Split at the whole second so the multiply cannot overflow: the
 * fractional part is below 1e6 and the highest ruler frequency this
 * kernel will ever hold is the TSC ceiling of 1e10, so the widest
 * product here is 1e16 against a 1.8e19 range. Rounding up applies to
 * the fraction only — the whole-second part is exact. A span that
 * rounds to zero is raised to one tick, because a caller asking for
 * time is never asking for none. */
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
        /* The TSC is 64 bits and monotonic; at the 10 GHz ceiling it
         * wraps in 58 years, so there is no mask and no wrap arithmetic
         * — elapsed is a plain subtraction from the turn. */
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
        /* Nothing witnesses the TSC — see hourglass.h. One read, one
         * subtraction, one compare, and SourceDied stays false forever
         * on this ruler. */
        g->elapsed = rdtsc() - g->mark;
        return g->elapsed >= g->span;
    }

    /* One counter read and one rdtsc serve BOTH jobs: the same timestamp
     * re-arms the witness when the counter moved and judges the stall
     * when it did not. That keeps the common path — the counter moved —
     * at one read, one subtract, one compare. */
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

    /* Same confirming re-read as the adoption probe, and for the same
     * reason: without it a host deschedule landing between the counter
     * read and the rdtsc would condemn a live counter and cut the
     * caller's span short. */
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
    /* Split at the whole second again, this time so that a long span at
     * a fast ruler cannot overflow the ×1e6: the remainder is below hz
     * and hz is at most 1e10, giving 1e16 at worst. */
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
