#ifndef TAGFS_SETTLE_INTERNAL_H
#define TAGFS_SETTLE_INTERNAL_H

#include "settle.h"

/*
 * What the cycle (settle.c), its cadence (cadence.c) and its waiters
 * (settle_wait.c) say to each other, and to nobody else.
 */

/* Why a cycle runs: its reason, for the cycle of either format. */
typedef enum {
    SETTLE_FOR_WAITER = 1,   /* a kernel path asks and waits for what it settles */
    SETTLE_FOR_ROOM,         /* the volume is short while Fallow rests blocks */
    SETTLE_FOR_HALT,         /* the machine is stopping: the last cycle */
    SETTLE_FOR_CADENCE,      /* Lull permitted, or the window ran out, or a demand */
} SettleWhy;

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
 * the Ink it settled up to. How many times room was asked for (SettleDemand's
 * ROOM), counted: the event a cycle refused for want of ground waits for. */
uint32_t CadenceRunBegins(void);
void     CadenceRunEnded(error_t rc, uint64_t busy_us, uint64_t s0_us, uint64_t target);
void     CadenceMountReset(void);
uint64_t CadenceRoomAsked(void);

/* settle_wait.c: a cycle took its snapshot — the seat:ready it began after is
 * noted, so a refusal can tell whether the medium already said it answers
 * again; the cycle's end, by its holder before the Give: the park its refusal
 * leaves, or none once it settled (the rule over SettleWaitersPark); and a
 * mount's first word for the waiters. */
void     SettleWaitersRunBegins(void);
void     SettleWaitersPark(error_t rc);
void     SettleWaitersMountReset(void);

/* settle_wait.c: when the park standing now was decided — 0 when none stands;
 * and its one line, for Nightwatch (run from a baton). */
uint64_t SettleParkedSinceUs(void);
void     SettleParkSay(void);

/* The time the cadence counts in: microseconds of the TSC. */
uint64_t SettleNowUs(void);

/* settle.c: a cycle of either format settled — every Ink up to `target`, as
 * the `cycle`-th of this mount, the volume sealed clean or not — under the
 * state lock, at the cycle's S6. */
void     SettleSettledLocked(uint64_t target, uint64_t cycle, bool clean);

/* settle.c: a cycle asked for and waited through — this call's own, or one
 * another core was making — for `why`; its answer (settle_halt.c). The ground
 * the last settled cycle's S6 gave back, and a mount's first word for the halt. */
error_t  SettleAskedFor(SettleWhy why);
uint64_t SettleGivenLast(void);
void     SettleHaltMountReset(void);

#endif /* TAGFS_SETTLE_INTERNAL_H */
