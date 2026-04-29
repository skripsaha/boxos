/*
 * KRing — kernel-side helpers for the lazy-growable Pocket/Result rings.
 *
 * The header sits at proc->pocket_ring_phys / proc->result_ring_phys (one
 * physical page each, accessible to the kernel via vmm_phys_to_virt). The
 * slot region is a separately reserved virtual range whose pages are mapped
 * on demand: PocketRing slots fault in via the user page-fault handler;
 * ResultRing slots are mapped proactively here in KResultPush.
 */

#include "kring.h"
#include "klib.h"
#include "vmm.h"
#include "pmm.h"
#include "process.h"
#include "result.h"
#include "kresult.h"
#include "boxos_magic.h"

void KRingPocketInit(PocketRing *hdr)
{
    if (!hdr) return;
    memset(hdr, 0, sizeof(*hdr));
    hdr->hdr.head           = 0;
    hdr->hdr.tail           = 0;
    hdr->hdr.slots_base     = CABIN_POCKET_SLOTS_BASE;
    hdr->hdr.slot_size      = POCKET_SLOT_SIZE;
    hdr->hdr.slot_count_max = (uint32_t)POCKET_RING_SLOT_MAX;
    hdr->hdr.magic          = POCKET_RING_MAGIC;
}

void KRingResultInit(ResultRing *hdr)
{
    if (!hdr) return;
    memset(hdr, 0, sizeof(*hdr));
    hdr->hdr.head           = 0;
    hdr->hdr.tail           = 0;
    hdr->hdr.slots_base     = CABIN_RESULT_SLOTS_BASE;
    hdr->hdr.slot_size      = RESULT_SLOT_SIZE;
    hdr->hdr.slot_count_max = (uint32_t)RESULT_RING_SLOT_MAX;
    hdr->hdr.magic          = RESULT_RING_MAGIC;
}

/* -------------------------------------------------------------------------
 * PocketRing consumer
 * ------------------------------------------------------------------------- */

static PocketRing *kring_pocket_hdr(process_t *proc)
{
    if (!proc || !proc->pocket_ring_phys) return NULL;
    return (PocketRing *)vmm_phys_to_virt(proc->pocket_ring_phys);
}

bool KPocketIsEmpty(process_t *proc)
{
    PocketRing *r = kring_pocket_hdr(proc);
    return !r || pocket_ring_is_empty(r);
}

uint32_t KPocketCount(process_t *proc)
{
    PocketRing *r = kring_pocket_hdr(proc);
    return r ? pocket_ring_count(r) : 0;
}

Pocket *KPocketPeek(process_t *proc)
{
    PocketRing *r = kring_pocket_hdr(proc);
    if (!r || pocket_ring_is_empty(r)) return NULL;

    uintptr_t uvaddr = pocket_ring_slot_uvaddr(r, r->hdr.head);
    return (Pocket *)vmm_translate_user_addr(proc->cabin, uvaddr, sizeof(Pocket));
}

void KPocketPop(process_t *proc)
{
    PocketRing *r = kring_pocket_hdr(proc);
    if (!r || pocket_ring_is_empty(r)) return;
    r->hdr.head++;
}

/* -------------------------------------------------------------------------
 * ResultRing producer
 * ------------------------------------------------------------------------- */

static ResultRing *kring_result_hdr(process_t *proc)
{
    if (!proc || !proc->result_ring_phys) return NULL;
    return (ResultRing *)vmm_phys_to_virt(proc->result_ring_phys);
}

bool KResultPush(process_t *target, const Result *r)
{
    if (!target || !r) return false;

    ResultRing *rr = kring_result_hdr(target);
    if (!rr) return false;
    if (result_ring_is_full(rr)) return false;

    uintptr_t uvaddr = result_ring_slot_uvaddr(rr, rr->hdr.tail);

    /* Ensure the slot page is mapped user RW in the target's cabin. */
    if (vmm_ensure_user_page(target->cabin, uvaddr, /*writable=*/true) != 0) {
        return false;
    }

    Result *slot = (Result *)vmm_translate_user_addr(target->cabin, uvaddr,
                                                     sizeof(Result));
    if (!slot) return false;

    /* Slot stride is RESULT_SLOT_SIZE (32B); we write only sizeof(Result)
     * (24B) — the trailing 8 bytes per slot remain whatever they were. The
     * consumer reads sizeof(Result), so the padding never escapes. */
    *slot = *r;
    __sync_synchronize();
    rr->hdr.tail++;

    if (process_get_state(target) == PROC_WAITING) {
        process_set_state(target, PROC_WORKING);
    }
    return true;
}
