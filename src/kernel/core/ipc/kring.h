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
 *   kpocket_peek  — returns kernel VA of head slot, or NULL if empty.
 *                   Walks the cabin's user page tables to translate the
 *                   user-vaddr slot to a kernel pointer. The caller must NOT
 *                   advance head; use kpocket_pop after fully processing.
 *   kpocket_pop   — bumps head by one. */
/* Look at the pocket at the head of the ring; `*pos_out` names the position
 * looked at, and it is the ONLY thing KPocketPopAt will take. */
Pocket  *KPocketPeek(process_t *proc, uint64_t *pos_out);
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

/* True when the ResultRing holds a published, unconsumed reply carrying a
 * non-zero cloakroom token (an unanswered-to-its-owner submit completion).
 * process_set_state consults this before committing PROC_WAITING — a
 * process must never sleep past its own pending reply. */
bool KResultRingHasPendingReply(process_t *proc);

/* True when the slot at the ring's head is published and unconsumed — any
 * record, token or not. Turn In refuses a sleep across it: the cursors alone
 * miss a slot claimed before the sleeper's mark and released after its look.
 * A claim still in flight at the head does not count (its producer wakes the
 * sleeper after releasing it). See the definition for the measured case. */
bool KResultRingHasUnreadAtHead(process_t *proc);

/* Diagnostic: snapshot per-return-path counters
 *   out[0]=null_args, [1]=no_hdr, [2]=zero_cap, [3]=pre_full, [4]=premap_fail,
 *   [5]=crosspg_fail, [6]=translate_fail, [7]=spin_warn, [8]=overflow, [9]=success */
void KResultPushStats(uint64_t out[9]);

#endif /* KRING_H */
