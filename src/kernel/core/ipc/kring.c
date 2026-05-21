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

/* Diagnostic: per-return-path counters. Snapshot via KResultPushStats(). */
static volatile uint64_t g_krp_null_args;
static volatile uint64_t g_krp_no_hdr;
static volatile uint64_t g_krp_zero_cap;
static volatile uint64_t g_krp_pre_full;
static volatile uint64_t g_krp_premap_fail;
static volatile uint64_t g_krp_crosspg_fail;
static volatile uint64_t g_krp_translate_fail;
static volatile uint64_t g_krp_spin_limit;
static volatile uint64_t g_krp_overflow;
static volatile uint64_t g_krp_success;

void KResultPushStats(uint64_t out[10])
{
    out[0] = __atomic_load_n(&g_krp_null_args,      __ATOMIC_RELAXED);
    out[1] = __atomic_load_n(&g_krp_no_hdr,         __ATOMIC_RELAXED);
    out[2] = __atomic_load_n(&g_krp_zero_cap,       __ATOMIC_RELAXED);
    out[3] = __atomic_load_n(&g_krp_pre_full,       __ATOMIC_RELAXED);
    out[4] = __atomic_load_n(&g_krp_premap_fail,    __ATOMIC_RELAXED);
    out[5] = __atomic_load_n(&g_krp_crosspg_fail,   __ATOMIC_RELAXED);
    out[6] = __atomic_load_n(&g_krp_translate_fail, __ATOMIC_RELAXED);
    out[7] = __atomic_load_n(&g_krp_spin_limit,     __ATOMIC_RELAXED);
    out[8] = __atomic_load_n(&g_krp_overflow,       __ATOMIC_RELAXED);
    out[9] = __atomic_load_n(&g_krp_success,        __ATOMIC_RELAXED);
}

bool KResultPush(process_t *target, const Result *r)
{
    if (!target || !r) {
        __atomic_add_fetch(&g_krp_null_args, 1, __ATOMIC_RELAXED);
        return false;
    }

    ResultRing *rr = kring_result_hdr(target);
    if (!rr) {
        __atomic_add_fetch(&g_krp_no_hdr, 1, __ATOMIC_RELAXED);
        return false;
    }

    uint32_t cap = rr->hdr.slot_count_max;
    if (cap == 0) {
        __atomic_add_fetch(&g_krp_zero_cap, 1, __ATOMIC_RELAXED);
        return false;
    }

    /* (1) Cheap fullness pre-check. Best-effort; we re-validate after the
     *     atomic reservation. ACQUIRE on head so we see the consumer's
     *     most recent advance. */
    uint64_t head      = __atomic_load_n(&rr->hdr.head, __ATOMIC_ACQUIRE);
    uint64_t tail_snap = __atomic_load_n(&rr->hdr.tail, __ATOMIC_RELAXED);
    if ((tail_snap - head) >= cap) {
        __atomic_add_fetch(&g_krp_pre_full, 1, __ATOMIC_RELAXED);
        return false;
    }

    /* (2) Pre-map BOTH the snapshot's slot page AND the next page.
     *
     *     Why two: after the atomic fetch_add at step (3), concurrent
     *     K-Cores may have pushed our reserved `pos` onto the next 4-KiB
     *     page. The historical code only pre-mapped one page, then tried
     *     to map the other lazily AFTER the reservation — if that lazy
     *     map failed (e.g. PMM transiently exhausted), the slot was
     *     stranded and the consumer would spin on slot.seq forever,
     *     freezing the entire cabin's IPC. By pre-mapping the next page
     *     too, we move that map call BEFORE the reservation, so any
     *     failure happens without state change: we return false, the
     *     ring stays consistent. With ~128 slots/page and ~4 producers,
     *     the actually-used page can only be `uvaddr_pre` or its
     *     immediate successor. */
    uintptr_t uvaddr_pre  = result_ring_slot_uvaddr(rr, tail_snap);
    uintptr_t uvaddr_next = result_ring_slot_uvaddr(rr, tail_snap + cap /* same offset, next page if any */);
    /* The "next page" we care about is whichever page tail_snap+1's
     * worst-case neighbour spans. Compute via the slot AFTER N producers
     * worth of pushes (cap as a safe upper bound is overkill; one page
     * stride is enough). 4 KiB / sizeof(ResultSlot) slots per page. */
    uintptr_t one_page_ahead = uvaddr_pre + 4096u;
    (void)uvaddr_next;
    if (vmm_ensure_user_page(target->cabin, uvaddr_pre, /*writable=*/true) != 0) {
        __atomic_add_fetch(&g_krp_premap_fail, 1, __ATOMIC_RELAXED);
        return false;
    }
    /* Best-effort pre-map of the next page — if it fails the historical
     * crosspg path below will re-attempt with a synthetic ERR if needed. */
    (void)vmm_ensure_user_page(target->cabin, one_page_ahead, /*writable=*/true);

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
     *     wasn't pre-mapped. Try to map; if THAT fails, treat the slot
     *     as overflow so we publish a synthetic ERR (no strand). */
    uintptr_t uvaddr = result_ring_slot_uvaddr(rr, pos);
    bool crosspg_failed = false;
    if (uvaddr != uvaddr_pre && uvaddr != one_page_ahead) {
        if (vmm_ensure_user_page(target->cabin, uvaddr, /*writable=*/true) != 0) {
            __atomic_add_fetch(&g_krp_crosspg_fail, 1, __ATOMIC_RELAXED);
            kprintf("[KRP] WARN: cross-page map failed at pos=%lu pid=%u — "
                    "synthesizing ERR slot to avoid stranding the ring\n",
                    (unsigned long)pos, (unsigned int)target->pid);
            crosspg_failed = true;
            overflow = true;   /* force the synthetic ERR path below */
        }
    }

    ResultSlot *slot = kring_translate_slot(target, uvaddr);
    if (!slot) {
        /* Residual strand window: vmm_ensure_user_page succeeded but
         * translate_slot couldn't walk the user PT. The only way this
         * happens is a concurrent unmap from process_destroy or a
         * page-table corruption — both are bugs upstream that we
         * cannot recover from here without holding a hard reference
         * on the user page. We log and return false; the consumer
         * sees the slot stuck on seq==2*round, which TouchAwait /
         * receive_wait treats as "nothing here, retry later". A
         * subsequent push to a different slot (different round)
         * makes forward progress for the same producer; this single
         * slot remains stuck until process exit.
         *
         * Mitigation: pre-map both possible pages above so the
         * common path never hits this branch. Residual cases are
         * rare-enough to not justify the complexity of a per-page
         * refcount today. */
        __atomic_add_fetch(&g_krp_translate_fail, 1, __ATOMIC_RELAXED);
        kprintf("[KRP] WARN: translate failed pos=%lu pid=%u (slot stuck — "
                "consumer treats as empty)\n",
                (unsigned long)pos, (unsigned int)target->pid);
        return false;
    }

    /* (6) Vyukov gate: wait until the slot's seq matches our round's
     *     "ready for write" value. tail was already incremented, so
     *     abandoning the slot would strand it and the consumer would
     *     spin forever. We never give up — cpu_pause keeps the core
     *     friendly under contention. */
    uint64_t round    = pos / cap;
    uint64_t expected = 2u * round;

    uint64_t spins = 0;
    for (;;) {
        uint64_t seq = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
        if (seq == expected) break;
        cpu_pause();
        if (++spins == (1u << 24)) {
            __atomic_add_fetch(&g_krp_spin_limit, 1, __ATOMIC_RELAXED);
            kprintf("[KRP] WARN: long spin pid=%u pos=%lu seq=%lu expected=%lu\n",
                    (unsigned int)target->pid, (unsigned long)pos,
                    (unsigned long)seq, (unsigned long)expected);
        }
    }

    /* (7) Write payload. On overflow, write a synthetic error so the
     *     consumer drains the slot and the ring continues. */
    if (overflow) {
        slot->r.error_code  = ERR_RESULT_RING_FULL;
        slot->r.data_length = 0;
        slot->r.data_addr   = 0;
        slot->r.sender_pid  = 0;
        slot->r.context     = KCTX_GUIDE;
        __atomic_add_fetch(&g_krp_overflow, 1, __ATOMIC_RELAXED);
    } else {
        slot->r = *r;
        __atomic_add_fetch(&g_krp_success, 1, __ATOMIC_RELAXED);
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
