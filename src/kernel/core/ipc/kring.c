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

/* Reads of the producer-side cursor (tail) MUST use ACQUIRE so the
 * consumer sees the producer's slot store that preceded the tail bump.
 * On x86 TSO plain volatile reads happen to behave like ACQUIRE, but
 * the explicit semantics keep us honest on weaker memory models and
 * defeat any compiler reordering across the load. Audit 2026-04-29. */
bool KPocketIsEmpty(process_t *proc)
{
    PocketRing *r = kring_pocket_hdr(proc);
    if (!r) return true;
    uint64_t head = __atomic_load_n(&r->hdr.head, __ATOMIC_RELAXED);
    uint64_t tail = __atomic_load_n(&r->hdr.tail, __ATOMIC_ACQUIRE);
    return head == tail;
}

uint32_t KPocketCount(process_t *proc)
{
    PocketRing *r = kring_pocket_hdr(proc);
    if (!r) return 0;
    uint64_t head = __atomic_load_n(&r->hdr.head, __ATOMIC_RELAXED);
    uint64_t tail = __atomic_load_n(&r->hdr.tail, __ATOMIC_ACQUIRE);
    uint64_t n = tail - head;
    return n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)n;
}

Pocket *KPocketPeek(process_t *proc)
{
    PocketRing *r = kring_pocket_hdr(proc);
    if (!r) return NULL;
    uint64_t head = __atomic_load_n(&r->hdr.head, __ATOMIC_RELAXED);
    uint64_t tail = __atomic_load_n(&r->hdr.tail, __ATOMIC_ACQUIRE);
    if (head == tail) return NULL;

    uintptr_t uvaddr = pocket_ring_slot_uvaddr(r, head);
    return (Pocket *)vmm_translate_user_addr(proc->cabin, uvaddr, sizeof(Pocket));
}

void KPocketPop(process_t *proc)
{
    PocketRing *r = kring_pocket_hdr(proc);
    if (!r) return;
    uint64_t head = __atomic_load_n(&r->hdr.head, __ATOMIC_RELAXED);
    uint64_t tail = __atomic_load_n(&r->hdr.tail, __ATOMIC_ACQUIRE);
    if (head == tail) return;
    /* Publish the new head with RELEASE so the userspace producer's view
     * of "ring not full" cannot leapfrog ahead of the slot we already
     * consumed. */
    __atomic_store_n(&r->hdr.head, head + 1, __ATOMIC_RELEASE);
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
    /* ACQUIRE on head so we see the consumer's most recent advance; this
     * is what tells us whether the ring really has room. */
    uint64_t head = __atomic_load_n(&rr->hdr.head, __ATOMIC_ACQUIRE);
    uint64_t tail = __atomic_load_n(&rr->hdr.tail, __ATOMIC_RELAXED);
    if ((tail - head) >= rr->hdr.slot_count_max) return false;

    uintptr_t uvaddr = result_ring_slot_uvaddr(rr, tail);

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
    /* RELEASE pair with userspace ACQUIRE on tail: the consumer must not
     * see the new tail before the slot store is visible. Replaces the old
     * full fence + non-atomic tail bump pattern, which was the suspected
     * "non-manifest pocket flags=0x0" race source. */
    __atomic_store_n(&rr->hdr.tail, tail + 1, __ATOMIC_RELEASE);

    if (process_get_state(target) == PROC_WAITING) {
        process_set_state(target, PROC_WORKING);
    }
    return true;
}
