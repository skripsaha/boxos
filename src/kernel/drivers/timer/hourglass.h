#ifndef HOURGLASS_H
#define HOURGLASS_H

#include "ktypes.h"

/*
 * HourGlass — the bound a driver turns once and then glances at.
 *
 * Every driver in this kernel that waits on a hardware bit wants the same
 * three things and has, until now, had none of them: a span of real time
 * that is real, a way to poll its OWN register while the span runs, and an
 * answer afterwards that distinguishes "the span ended" from "the thing I
 * was timing against stopped ticking". A void busy-wait gives none of those.
 * It cannot say it did nothing, it cannot be interleaved with the caller's
 * own read, and when its counter freezes it spins with interrupts off and
 * the machine dies without a last line.
 *
 * So: turn the glass, get on with your own work, glance at it.
 *
 *     HourGlass g;
 *     if (!HourGlassTurn(&g, 500000)) {  // half a second
 *         kprintf("[X] no time ruler on this machine — waiting on the fact\n");
 *         while (!(REG & READY)) pause();          // unbounded, deliberate
 *     } else {
 *         while (!(REG & READY) && !HourGlassRunOut(&g)) pause();
 *         if (!(REG & READY))
 *             kprintf("[X] not ready after %lu us (%s)\n",
 *                     HourGlassElapsedUs(&g),
 *                     HourGlassSourceDied(&g) ? "ruler died" : "span ran out");
 *     }
 *
 * The struct is caller-owned and lives on the caller's stack. No globals in
 * the glass, no allocation, no lock — which is what makes it legal from an
 * interrupt handler, from AP bring-up before the scheduler exists, and from
 * inside any lock without entering the lock order at all.
 *
 * THE SUCCESSION OF RULERS. HourGlassTurn walks it top to bottom and stops
 * at the first ruler that has PROVEN itself on this machine:
 *
 *   1. TSC at an ARCHITECTURAL frequency. CPUID leaf 0x15 gives EBX/EAX as
 *      the TSC-to-core-crystal ratio and ECX as the crystal in Hz; when all
 *      three are non-zero the TSC rate is ECX*EBX/EAX exactly, with nothing
 *      measured and nothing calibrated. Available before ACPI, before the
 *      PIT, before anything. When ECX is zero — which is every Skylake,
 *      Kaby Lake and Coffee Lake part — this ruler is simply NOT AVAILABLE.
 *      It is not patched up with a 24 MHz guess; a guess is what a bound is
 *      supposed to protect the caller from.
 *   2. ACPI PM Timer, proven live.
 *   3. 8254 channel 2 as a free-running down-counter, proven live.
 *   4. Nothing. HourGlassTurn returns false and the caller must say so out
 *      loud and wait on the fact itself. It must NOT invent a spin count.
 *
 * PROVING A RULER WITHOUT ANOTHER CLOCK. The witness is the TSC: rdtsc
 * counts on every x86-64 part whether or not anyone has calibrated it, so
 * it is a monotonic counter long before it is a clock. Sample the candidate
 * and the TSC together; if the TSC has run past several of the candidate's
 * own tick periods while the candidate has not moved one bit, the candidate
 * is dead. An I/O port nothing claims reads 0xFFFFFFFF forever, and that is
 * precisely the reading this catches. The test runs once at adoption and
 * again inside every HourGlassRunOut, so a ruler that dies MID-SPAN ends
 * the span instead of hanging the machine.
 *
 * The TSC cannot witness itself. When the TSC is the chosen ruler there is
 * nothing left to check it against, and HourGlassSourceDied is always false
 * on that ruler. That is accepted: an x86-64 whose TSC does not advance
 * cannot execute this kernel far enough to ask the question.
 */

typedef enum {
    HOURGLASS_RULER_NONE = 0,
    HOURGLASS_RULER_TSC,        /* CPUID.15h architectural frequency */
    HOURGLASS_RULER_PMTIMER,    /* ACPI PM Timer, 3.579545 MHz       */
    HOURGLASS_RULER_PIT8254,    /* 8254 channel 2, 1.193182 MHz      */
} HourGlassRuler;

typedef struct {
    HourGlassRuler ruler;
    uint64_t span;      /* ruler ticks that must pass before the glass runs out */
    uint64_t elapsed;   /* ruler ticks actually seen so far */
    uint64_t hz;        /* ruler ticks per second — turns elapsed into time */
    uint64_t mark;      /* TSC ruler: rdtsc at the turn */
    uint64_t witness;   /* TSC when the counter was last SEEN to move */
    uint64_t stall;     /* TSC cycles of stillness that condemn the counter */
    uint32_t last;      /* counter rulers: previous raw sample */
    uint32_t mask;      /* counter width, so a wrap subtracts correctly */
    bool     died;      /* the ruler stopped mid-span */
} HourGlass;

/* Turn the glass for `us` microseconds against the best ruler this machine
 * has proven. Returns false when there is NO ruler at this point in boot —
 * the caller must then decide, loudly, and must not invent a substitute.
 * A glass that failed to turn reports RunOut true and ElapsedUs zero, so a
 * caller that ignores the return value fails fast instead of spinning. */
bool HourGlassTurn(HourGlass *g, uint64_t us);

/* Turn the glass against ONE named ruler, refusing every other. For the
 * caller that is measuring a ruler and therefore must not be timed by it —
 * cpu_calibrate_tsc measuring the TSC against the PM Timer would otherwise
 * get a glass that quietly chose the TSC and measured it against itself. */
bool HourGlassTurnOn(HourGlass *g, uint64_t us, HourGlassRuler ruler);

/* True once the span has run out, or once the ruler is proven dead. This is
 * the call that sits in the polling loop beside the caller's own hardware
 * read, so it costs one counter read, one rdtsc and one compare. */
bool HourGlassRunOut(HourGlass *g);

/* How much real time the glass has seen — as of the last HourGlassRunOut,
 * since that is the call that samples. For the "gave up after N us" lines
 * drivers want to print. */
uint64_t HourGlassElapsedUs(const HourGlass *g);

/* Did the ruler die mid-span, as opposed to the span simply ending? The
 * caller that gave up wants to name which of the two happened. */
bool HourGlassSourceDied(const HourGlass *g);

/* Which ruler this glass was turned on, for the same diagnostic line. */
const char *HourGlassRulerName(const HourGlass *g);

#endif /* HOURGLASS_H */
