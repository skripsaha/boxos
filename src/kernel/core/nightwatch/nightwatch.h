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
 * Two contradictions it proves with no threshold whatsoever:
 *
 *   LOST WAKE     A process is parked on an address, no deadline was armed,
 *                 and the value it waits on has ALREADY changed. Whoever
 *                 changed it owed a wake and never delivered one. This is
 *                 true at the instant it is read — nothing is waited for in
 *                 order to conclude it.
 *
 *   UNREACHABLE   The user VA the parker supplied resolves TODAY to a
 *                 different physical page than the one its entry is filed
 *                 under. The wait table is keyed by PHYSICAL address, so any
 *                 waker hashes into a different bucket and this waiter can
 *                 never be found again. addr_wait.h names this hazard in its
 *                 REAL-HW CAVEAT; until now nothing in the kernel could tell
 *                 that it had actually happened.
 *
 * The all-cores-idle quiet period below is NOT the diagnosis. It only decides
 * WHEN to look, so the checks cost nothing while the system is doing work. A
 * wake IPI in flight can leave every core momentarily idle, which is the only
 * reason a duration appears anywhere in this subsystem.
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
