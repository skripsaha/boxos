#ifndef NIGHTWATCH_H
#define NIGHTWATCH_H

#include "ktypes.h"

/*
 * Nightwatch — the watch that notices the ship has stopped.
 *
 * NOT a watchdog, and the difference is the whole point. A watchdog measures
 * TIME: "X has not happened in N ms, therefore something is wrong." N is a
 * guess, and when the guess is wrong the watchdog either cries wolf or stays
 * silent through a real failure. Worse, a watchdog usually ACTS — resets,
 * retries, kills — and every recovery it performs erases the evidence of the
 * defect it existed to expose.
 *
 * Nightwatch measures a CONTRADICTION instead, and it only ever reports.
 *
 * Two contradictions it proves — neither on a clock's say-so:
 *
 *   LOST WAKE     A process is parked on an address, no deadline was armed,
 *                 the value it waits on has ALREADY changed — and one full
 *                 look later the same park is still there and the value is
 *                 still changed. Whoever changed it owed a wake and never
 *                 delivered one. The first half alone is NOT proof, and it
 *                 was believed to be until a passing test showed otherwise:
 *                 a waker stores before it asks for the wake in a separate
 *                 call, so "changed and still asleep" is what every delivery
 *                 looks like for the length of one syscall — and a walk that
 *                 reads the word later than it copied the park sees the same
 *                 thing on a machine that is merely busy (measured: a whole
 *                 brigade of std::execution::par accused at once while its
 *                 phase passed). Persistence is the proof, exactly as for the
 *                 delivery oracles in nightwatch.c: a delivery in flight
 *                 cannot span two looks, a lost wake spans every look there
 *                 will ever be.
 *
 *   UNREACHABLE   The user VA the parker supplied resolves TODAY to a
 *                 different physical page than the one its entry is filed
 *                 under. The wait table is keyed by PHYSICAL address, so any
 *                 waker hashes into a different bucket and this waiter can
 *                 never be found again. addr_wait.h names this hazard in its
 *                 REAL-HW CAVEAT; until now nothing in the kernel could tell
 *                 that it had actually happened.
 *
 * The spacing of looks (NIGHTWATCH_LOOK_MS in nightwatch.c) is NOT the
 * diagnosis, and it is not a threshold on the machine either. It decides WHEN
 * to look, so the checks cost nothing while the system is doing work; and it
 * is the distance between the two looks a persistence proof needs — chosen so
 * that anything in flight has long since landed by the second one.
 *
 * Cost while healthy: one byte store per core entering or leaving idle, plus
 * one integer compare per idle wake-up. Nothing on the syscall, IPC or
 * scheduler-selection path.
 */

void nightwatch_init(void);

/* A core is about to sleep with nothing to run. Called from every idle site.
 * This is also the re-check point: an idle core wakes on every timer tick, so
 * the quiet period is evaluated without any timer of Nightwatch's own. */
void nightwatch_core_idle(uint8_t core);

/* A core has taken a real (non-idle) process. Clears this core's idle mark and
 * re-arms the report, so a system that stalls twice reports twice. */
void nightwatch_core_busy(uint8_t core);

#endif /* NIGHTWATCH_H */
