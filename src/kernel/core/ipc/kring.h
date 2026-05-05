#ifndef KRING_H
#define KRING_H

#include "ktypes.h"
#include "pocket.h"
#include "pocket_ring.h"
#include "result_ring.h"

typedef struct process_t process_t;

/* Initialise a freshly-allocated ring header page. The caller owns the
 * physical page; this writes head=tail=0, slots_base, slot_size, etc. */
void KRingPocketInit(PocketRing *hdr);
void KRingResultInit(ResultRing *hdr);

/* Kernel-side PocketRing consumer.
 *   kpocket_peek  — returns kernel VA of head slot, or NULL if empty.
 *                   Walks the cabin's user page tables to translate the
 *                   user-vaddr slot to a kernel pointer. The caller must NOT
 *                   advance head; use kpocket_pop after fully processing.
 *   kpocket_pop   — bumps head by one. */
Pocket  *KPocketPeek(process_t *proc);
void     KPocketPop(process_t *proc);
bool     KPocketIsEmpty(process_t *proc);
uint32_t KPocketCount(process_t *proc);

/* Kernel-side ResultRing producer.
 *   kresult_push  — ensures the destination slot page is mapped in the
 *                   target's cabin (lazy first-touch demand allocation),
 *                   writes the Result, bumps tail, wakes the target if
 *                   it is PROC_WAITING. Returns false on full ring or
 *                   allocation failure. */
bool KResultPush(process_t *target, const Result *r);

/* Diagnostic: snapshot per-return-path counters
 *   out[0]=null_args, [1]=no_hdr, [2]=zero_cap, [3]=pre_full, [4]=premap_fail,
 *   [5]=crosspg_fail, [6]=translate_fail, [7]=spin_warn, [8]=overflow, [9]=success */
void KResultPushStats(uint64_t out[10]);

#endif /* KRING_H */
