/*
 * KRing — kernel-side helpers for the lazy-growable Pocket/Result rings.
 *
 * The header sits at proc->pocket_ring_phys / proc->result_ring_phys (one
 * physical page each, accessible to the kernel via vmm_phys_to_virt). The
 * slot region is a separately reserved virtual range whose pages are mapped
 * on demand: PocketRing slots fault in via the user page-fault handler;
 * ResultRing slots are mapped proactively here in KResultPush.
 *
 * ResultRing is MPSC: multiple K-Cores can land in KResultPush concurrently
 * for the same target (e.g. two senders deliver IPC replies, plus the
 * sender's own manifest confirmation, all racing for one cabin's ring).
 * The producer side uses a per-slot Vyukov-style generation counter so the
 * slot[tail] write cannot stomp another producer's payload, and consumers
 * cannot read a half-written slot.
 */

#include "kring.h"
#include "klib.h"
#include "vmm.h"
#include "pmm.h"
#include "process.h"
#include "result.h"
#include "kresult.h"
#include "atomics.h"
#include "error.h"
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

/* Translate a target user vaddr to a writable kernel pointer for one
 * ResultSlot. Returns NULL if the translation fails (which post-ensure
 * should be impossible barring catastrophic memory pressure). */
static ResultSlot *kring_translate_slot(process_t *target, uintptr_t uvaddr)
{
    return (ResultSlot *)vmm_translate_user_addr(target->cabin, uvaddr,
                                                  sizeof(ResultSlot));
}

bool KResultPush(process_t *target, const Result *r)
{
    if (!target || !r) return false;

    ResultRing *rr = kring_result_hdr(target);
    if (!rr) return false;

    uint32_t cap = rr->hdr.slot_count_max;
    if (cap == 0) return false;

    /* (1) Cheap fullness pre-check. Best-effort; we re-validate after the
     *     atomic reservation. ACQUIRE on head so we see the consumer's
     *     most recent advance. */
    uint64_t head      = __atomic_load_n(&rr->hdr.head, __ATOMIC_ACQUIRE);
    uint64_t tail_snap = __atomic_load_n(&rr->hdr.tail, __ATOMIC_RELAXED);
    if ((tail_snap - head) >= cap) return false;

    /* (2) Pre-map the slot page our snapshot points at. Most cross-K-Core
     *     races land on the same 4-KiB page (128 slots/page), so this one
     *     map call usually covers our final pos. */
    uintptr_t uvaddr_pre = result_ring_slot_uvaddr(rr, tail_snap);
    if (vmm_ensure_user_page(target->cabin, uvaddr_pre, /*writable=*/true) != 0) {
        return false;
    }

    /* (3) Atomic reservation — MPSC linearisation point. Even with N
     *     concurrent K-Cores each gets a unique pos. Use ACQ_REL so the
     *     slot writes that follow are ordered after this fetch. */
    uint64_t pos = __atomic_fetch_add(&rr->hdr.tail, 1, __ATOMIC_ACQ_REL);

    /* (4) Re-check fullness against our reserved pos. Concurrent reservations
     *     may have pushed us past capacity; if so, publish a benign error
     *     into the slot so the consumer drains it instead of deadlocking
     *     on slot.seq forever. */
    bool overflow = (pos - head) >= cap;

    /* (5) Cross-page case: pos may have advanced into a slot page that
     *     wasn't pre-mapped. Map it. This race window is bounded by the
     *     number of concurrent producers — typically 1-4 K-Cores — far
     *     below 128 slots-per-page, so cross-page is rare. */
    uintptr_t uvaddr = result_ring_slot_uvaddr(rr, pos);
    if (uvaddr != uvaddr_pre) {
        if (vmm_ensure_user_page(target->cabin, uvaddr, /*writable=*/true) != 0) {
            /* Catastrophic: PMM exhausted between our pre-map and now.
             * The reserved slot has no kernel-writable mapping, so we
             * cannot publish even an error result. The ring will stall
             * for this target — log loudly. PMM exhaustion at IPC time
             * is system-fatal anyway. */
            kprintf("[KRP] FATAL: PMM exhausted at pos=%lu pid=%u (ring stuck)\n",
                    (unsigned long)pos, (unsigned int)target->pid);
            return false;
        }
    }

    ResultSlot *slot = kring_translate_slot(target, uvaddr);
    if (!slot) {
        kprintf("[KRP] FATAL: translate failed pos=%lu pid=%u (ring stuck)\n",
                (unsigned long)pos, (unsigned int)target->pid);
        return false;
    }

    /* (6) Vyukov gate: wait until the slot's seq matches our round's
     *     "ready for write" value. Zero-init pages give seq == 0 ==
     *     2 * round for round 0 — round 0 producers proceed without
     *     waiting. Subsequent rounds wait for the consumer to publish
     *     seq = 2 * round, which it does after reading the prior round.
     *     With cap=32768 and a healthy consumer, this is a single load
     *     in steady state.
     *
     *     If the consumer is genuinely stuck and we burn enough spins,
     *     bail out. The consumer will eventually re-sync once it
     *     resumes; we drop this Result (caller treats false as "ring
     *     full"). Avoids burning a K-Core forever on a misbehaving
     *     userspace process. */
    uint64_t round    = pos / cap;
    uint64_t expected = 2u * round;

    enum { KRESULT_PUSH_SPIN_LIMIT = 1u << 20 };
    uint64_t spins = 0;
    for (;;) {
        uint64_t seq = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
        if (seq == expected) break;
        if (++spins > KRESULT_PUSH_SPIN_LIMIT) {
            kprintf("[KRP] WARN: spin limit on pid=%u pos=%lu seq=%lu expected=%lu\n",
                    (unsigned int)target->pid, (unsigned long)pos,
                    (unsigned long)seq, (unsigned long)expected);
            return false;
        }
        cpu_pause();
    }

    /* (7) Write payload. On overflow, write a synthetic error so the
     *     consumer drains the slot and the ring continues. */
    if (overflow) {
        slot->r.error_code  = ERR_RESULT_RING_FULL;
        slot->r.data_length = 0;
        slot->r.data_addr   = 0;
        slot->r.sender_pid  = 0;
        slot->r.context     = KCTX_GUIDE;
    } else {
        slot->r = *r;
    }

    /* (8) Publish — release-store the slot's seq so the consumer's
     *     ACQUIRE-load sees the payload write. */
    __atomic_store_n(&slot->seq, expected + 1u, __ATOMIC_RELEASE);

    /* (9) Wake target if it was sleeping. If overflow we still wake — the
     *     consumer needs to drain the error slot to keep the ring moving. */
    if (process_get_state(target) == PROC_WAITING) {
        process_set_state(target, PROC_WORKING);
    }
    return !overflow;
}
