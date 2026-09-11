#ifndef KRING_H
#define KRING_H

#include "ktypes.h"
#include "pocket.h"
#include "pocket_ring.h"
#include "result_ring.h"

typedef struct process_t process_t;

void KRingPocketInit(PocketRing *hdr);
void KRingResultInit(ResultRing *hdr);
void KRingPocketInitAt(PocketRing *hdr, uint64_t slots_base, uint32_t slot_count_max);
void KRingResultInitAt(ResultRing *hdr, uint64_t slots_base, uint32_t slot_count_max);

Pocket  *KPocketPeek(process_t *proc, uint64_t *pos_out);
bool     KPocketPopAt(process_t *proc, uint64_t pos);
bool     KPocketIsEmpty(process_t *proc);
uint32_t KPocketCount(process_t *proc);

bool KResultPush(process_t *target, const Result *r);

bool KResultRingHasPendingReply(process_t *proc);

bool KResultRingHasUnreadAtHead(process_t *proc);


#endif