#ifndef CHIT_H
#define CHIT_H

#include "ktypes.h"
#include "klib.h"

/*
 * Chit — the kernel's half of the cloakroom token.
 *
 * A strand that submits a Pocket and waits for its answer publishes the token
 * it is holding out for (ResultRingHeader.awaiting, written by boxlib's
 * result_wait). That is the waiter's half. Until now the kernel kept no half
 * of its own: when a handler put an answer off — parked the strand on an
 * address, on another process's death, on a disk — nothing recorded that a
 * promise had been made, by whom, or whether the promised event had come.
 * Nightwatch could only guess at "owed and never produced" from the state of
 * the machine as a whole (every K-Core asleep, no answer published anywhere
 * in ten seconds), which is a witness, not a proof, and it was measured
 * accusing a healthy brigade of workers whose leader was merely busy.
 *
 * A chit is that record. It is a short note of debt, the kind a mess hall
 * runs on: the handler that defers an answer leaves one, the event that makes
 * the answer marks it due, and the push that publishes the answer redeems it.
 *
 *   PENDING  somebody OUTSIDE the kernel owes the event: a peer will wake the
 *            word, a child will die, a device will answer, a deadline will
 *            pass. Lawful for as long as it takes — a shell waits out a whole
 *            child's life here, and no clock may say otherwise. The kernel's
 *            guards against a silent device live in the drivers (their
 *            patience limits), and they end such a wait with an error, which
 *            is itself an answer.
 *   DUE      the answer is DETERMINED and the kernel itself owes the delivery:
 *            the park was claimed, the deadline fired, the death happened, the
 *            read reached its finish. What remains is one KResultPush, and it
 *            takes microseconds. Due across two of Nightwatch's looks is a
 *            kernel that dropped an answer, with the holder named.
 *   KEPT     the answer was published into the strand's reply ring. The slot
 *            is not cleared on keeping: a completion can outrun the guide, and
 *            the guide's own check (below) must then find the chit, not a hole.
 *
 * ONE SLOT PER STRAND. A strand is single-threaded and holds out for one token
 * at a time — `awaiting` is itself one word. Two promises outstanding at once
 * would take a wait abandoned early on a deadline the caller named; guaranteed
 * answers carry none, and a caller who did name one and moved on is answered
 * about that too: giving a chit over one still DUE says so out loud, because
 * that is the one shape — the kernel owed, the waiter gave up — that would
 * otherwise vanish with the overwrite.
 *
 * THE LOCK IS A LEAF, the innermost one there is. It is taken under
 * process_list_lock (Nightwatch's snapshot), under an addr-wait bucket lock
 * (the claim), under gone_lock, and from the PIT tick; it takes nothing
 * itself, ever. spin_lock disables interrupts, so every one of those callers
 * is in order. Nothing that holds this lock may call out.
 *
 * WHO WRITES THE ASYNC FLAG. ChitGive is the only writer of
 * OpContext.async_owns_crates. A handler cannot own a completion without
 * passing through here, so it cannot defer an answer that the watch is blind
 * to — that is the construction, and the guide checks it besides.
 */

struct process_t;
struct OpContext;

typedef enum {
    CHIT_NONE    = 0,   /* nothing was ever promised to this strand */
    CHIT_PENDING = 1,   /* the event that makes the answer has not come */
    CHIT_DUE     = 2,   /* the event came; the kernel owes the delivery */
    CHIT_KEPT    = 3,   /* the answer is in the strand's reply ring */
} ChitState;

/* What the watch reads: everything but the lock. */
typedef struct ChitView {
    uint32_t    cookie;     /* the cloakroom token; 0 = no chit */
    uint8_t     state;      /* ChitState */
    const char *holder;     /* who owes it — the op's registered name */
    uint64_t    detail;     /* the holder's own word: the watched phys, the
                             * awaited pid and generation, the file */
    uint64_t    since_ms;   /* when given; when it fell due */
} ChitView;

typedef struct Chit {
    spinlock_t  lock;
    ChitView    view;
} Chit;

void ChitInit(Chit *c);

/* The handler defers its answer. Sets *ctx->async_owns_crates (the only place
 * that does) and, if the submit carries a token, leaves a PENDING chit for it
 * naming `holder`. A submit with no token (fire-and-forget: touch_await, Turn
 * In, a bell, a ferry) is waited on by nobody, so no chit is left — the flag
 * is still set. */
void ChitGive(const struct OpContext *ctx, const char *holder, uint64_t detail);

/* The event has happened: the answer is determined and the kernel owes its
 * delivery. A token that is not this strand's current chit is ignored — a
 * stale claimer cannot mark a later promise due. */
void ChitDue(struct process_t *p, uint32_t cookie);

/* The answer was published (KResultPush, on success only). */
void ChitKeep(struct process_t *p, uint32_t cookie);

/* Nightwatch's snapshot. */
void ChitPeek(struct process_t *p, ChitView *out);

#endif /* CHIT_H */
