#ifndef KRING_H
#define KRING_H

#include "ktypes.h"
#include "pocket.h"
#include "pocket_ring.h"
#include "result_ring.h"

typedef struct process_t process_t;

/* Initialise a freshly-allocated ring header page. The caller owns the
 * physical page; this writes head=tail=0, slots_base, slot_size, etc.
 * The plain forms use the fixed cabin slot region + full capacity (the main
 * strand's rings); the *InitAt forms take an explicit per-strand slot-region
 * VA and capacity (a Hammock-carved spawned-strand ring — strand_rings.c). */
void KRingPocketInit(PocketRing *hdr);
void KRingResultInit(ResultRing *hdr);
void KRingPocketInitAt(PocketRing *hdr, uint64_t slots_base, uint32_t slot_count_max);
void KRingResultInitAt(ResultRing *hdr, uint64_t slots_base, uint32_t slot_count_max);

/* Kernel-side PocketRing consumer.
 *   KPocketPeek   — copies the pocket at the head of the ring into `out`,
 *                   through a porthole onto the slot; false when the ring is
 *                   empty or the slot is not the cabin's to read. The caller
 *                   must NOT advance head; use KPocketPopAt after processing.
 *   KPocketPopAt  — bumps head by one. */
/* Take a copy of the pocket at the head of the ring; `*pos_out` names the
 * position looked at, and it is the ONLY thing KPocketPopAt will take. */
bool     KPocketPeek(process_t *proc, uint64_t *pos_out, Pocket *out);
/* Take the ring past exactly `pos`, and only if it is still the head. Returns
 * false when it no longer is — see the note in kring.c. */
bool     KPocketPopAt(process_t *proc, uint64_t pos);
bool     KPocketIsEmpty(process_t *proc);
uint32_t KPocketCount(process_t *proc);

/* Kernel-side ResultRing producer.
 *   kresult_push  — ensures the destination slot page is mapped in the
 *                   target's cabin (lazy first-touch demand allocation),
 *                   writes the Result, bumps tail, wakes the target if
 *                   it is PROC_WAITING. Returns false on full ring or
 *                   allocation failure. */
bool KResultPush(process_t *target, const Result *r);

/* The stopwatch (make MEASURE=on; nothing at all in any other build): the
 * moment `proc`'s call came to the gate (KCallGate), the moment it was taken
 * to be served — by a K-Core, or on one core by the gate itself (KCallTaken)
 * — and, just before the answer to it is pushed, those two and the moment it
 * was answered onto its reply ring's stopwatch line (KCallAnswered,
 * result_ring.h). */
#ifdef CONFIG_MEASURE
void KCallGate(process_t *proc);
void KCallTaken(process_t *proc);
void KCallAnswered(process_t *proc);
#else
static inline void KCallGate(process_t *proc)     { (void)proc; }
static inline void KCallTaken(process_t *proc)    { (void)proc; }
static inline void KCallAnswered(process_t *proc) { (void)proc; }
#endif

/* Push, or take the answer as a DEBT — the entry point for a message whose
 * payload has ALREADY been deposited in the target's cabin heap (system_ops.c,
 * push_ipc_result). Returns true when the answer has been TAKEN: either it is
 * in the ring, or the kernel is holding it and will hand it over at the
 * target's next entry into the kernel. False means it was not taken at all and
 * the caller still owns the deposit — see kring.c for what it must do with it.
 *
 * KResultPush itself is untouched by any of this: an answer the kernel is not
 * holding a deposit for has nothing to release, and every other producer keeps
 * the plain push. */
bool KResultPushOrOwe(process_t *target, const Result *r);

/* Hand over what the reply ring refused, in the order it refused it. Called at
 * the syscall gate (idt.c) — the door every entry into the kernel passes,
 * including the YIELD short-circuit that never reaches the guide. Stops the
 * moment the ring refuses again, so one visit can never push more than the
 * consumer just drained. Guarded by a relaxed load of result_owed_count at the
 * call site: nothing is owed almost always. */
void ResultOwedHandOver(process_t *proc);

/* Free everything still owed to a dying strand — the Result descriptors AND
 * the cabin-heap deposits they name — and clear its slip. Called from
 * process_cleanup_immediate while the cabin's address space is still up. */
void ResultOwedRelease(process_t *proc);

/* True when the ResultRing holds a published, unconsumed reply carrying a
 * non-zero cloakroom token (an unanswered-to-its-owner submit completion).
 * process_set_state consults this before committing PROC_WAITING — a
 * process must never sleep past its own pending reply. */
bool KResultRingHasPendingReply(process_t *proc);
/* Ask the strand holding a core to yield it (1), or withdraw the ask (0):
 * ResultRingHeader.step_aside. A strand without a reply ring is ignored. */
void KResultStepAside(process_t *proc, bool step_aside);

/* True when the slot at the ring's head is published and unconsumed — any
 * record, token or not. Turn In refuses a sleep across it: the cursors alone
 * miss a slot claimed before the sleeper's mark and released after its look.
 * A claim still in flight at the head does not count (its producer wakes the
 * sleeper after releasing it). See the definition for the measured case. */
bool KResultRingHasUnreadAtHead(process_t *proc);


#endif /* KRING_H */
