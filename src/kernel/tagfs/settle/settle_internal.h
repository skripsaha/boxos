#ifndef TAGFS_SETTLE_INTERNAL_H
#define TAGFS_SETTLE_INTERNAL_H

#include "settle.h"

/*
 * What the cycle (settle.c), its cadence (cadence.c) and its waiters
 * (settle_wait.c) say to each other, and to nobody else.
 */

/* The core the cadence runs cycles on: the only core of a one-core machine;
 * else a K-Core that is not the drain core when there is one — the drain core
 * runs every continuation of the machine — and the drain core when it is the
 * only K-Core. */
uint8_t  SettleCommitterCore(void);

/* settle.c: whether a cycle is in flight, and whether it is this core's and
 * stands between two of its steps; one step of it; a cycle begun by the cadence
 * when the Scribe is free — false when another asker holds it. */
bool     SettleRunInFlight(void);
bool     SettleRunBetweenStepsHere(void);
void     SettleRunStepHere(void);
bool     SettleRunBeginForCadence(void);

/* cadence.c: a cycle took its snapshot — the demands it answers, taken; and
 * the cycle ended: its answer, its own time on the medium, when its S0 was, and
 * the Ink it settled up to. */
uint32_t CadenceRunBegins(void);
void     CadenceRunEnded(error_t rc, uint64_t busy_us, uint64_t s0_us, uint64_t target);
void     CadenceMountReset(void);

/* settle_wait.c: a cycle took its snapshot — the seat:ready it began after is
 * noted, so a refusal can tell whether the medium already said it answers
 * again; and a mount's first word for the waiters. */
void     SettleWaitersRunBegins(void);
void     SettleWaitersMountReset(void);

/* The time the cadence counts in: microseconds of the TSC. */
uint64_t SettleNowUs(void);

#endif /* TAGFS_SETTLE_INTERNAL_H */
