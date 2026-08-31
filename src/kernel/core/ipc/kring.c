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
#include "nightwatch.h"   /* a core mid-delivery is not an idle core */
#include "result.h"
#include "kresult.h"
#include "atomics.h"
#include "error.h"
#include "boxos_magic.h"
#include "amp.h"
#include "lapic.h"
#include "irqchip.h"

void KRingPocketInitAt(PocketRing *hdr, uint64_t slots_base, uint32_t slot_count_max)
{
    if (!hdr) return;
    /* Straddle invariant: every slot translation must stay inside one page,
     * which requires the per-strand Hammock slots_base (a runtime VA, not the
     * page-aligned cabin_layout.h constant) to be page-aligned. Geometry
     * proves the per-slot translation is straddle-safe only on top of this. */
    if (slots_base & (VMM_PAGE_SIZE - 1))
        panic("ring slots_base 0x%lx not page-aligned - straddle invariant broken",
              (unsigned long)slots_base);
    memset(hdr, 0, sizeof(*hdr));
    hdr->hdr.head           = 0;
    hdr->hdr.tail           = 0;
    hdr->hdr.slots_base     = slots_base;
    hdr->hdr.slot_size      = POCKET_SLOT_SIZE;
    hdr->hdr.slot_count_max = slot_count_max;
    hdr->hdr.magic          = POCKET_RING_MAGIC;
}

void KRingResultInitAt(ResultRing *hdr, uint64_t slots_base, uint32_t slot_count_max)
{
    if (!hdr) return;
    if (slots_base & (VMM_PAGE_SIZE - 1))
        panic("ring slots_base 0x%lx not page-aligned - straddle invariant broken",
              (unsigned long)slots_base);
    memset(hdr, 0, sizeof(*hdr));
    hdr->hdr.head           = 0;
    hdr->hdr.tail           = 0;
    hdr->hdr.slots_base     = slots_base;
    hdr->hdr.slot_size      = RESULT_SLOT_SIZE;
    hdr->hdr.slot_count_max = slot_count_max;
    hdr->hdr.magic          = RESULT_RING_MAGIC;
}

void KRingPocketInit(PocketRing *hdr)
{
    KRingPocketInitAt(hdr, CABIN_POCKET_SLOTS_BASE, (uint32_t)POCKET_RING_SLOT_MAX);
}

void KRingResultInit(ResultRing *hdr)
{
    KRingResultInitAt(hdr, CABIN_RESULT_SLOTS_BASE, (uint32_t)RESULT_RING_SLOT_MAX);
}

/* -------------------------------------------------------------------------
 * PocketRing consumer
 * ------------------------------------------------------------------------- */

static PocketRing *kring_pocket_hdr(process_t *proc)
{
    /* P5a: route by the PER-STRAND ring (proc->pocket_ring_phys), which for
     * the main strand aliases the cabin ring and for a spawned strand is its
     * own Hammock-carved ring. The cabin guard stays because downstream paths
     * (KPocketPeek translate) deref the shared proc->cabin->vmm. */
    if (!proc || !proc->cabin || !proc->pocket_ring_phys) return NULL;
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
    return (Pocket *)vmm_translate_user_addr(proc->cabin->vmm, uvaddr, sizeof(Pocket));
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
    /* P5a: per-strand ResultRing (see kring_pocket_hdr). */
    if (!proc || !proc->cabin || !proc->result_ring_phys) return NULL;
    return (ResultRing *)vmm_phys_to_virt(proc->result_ring_phys);
}

/* Translate a target user vaddr to a writable kernel pointer for one
 * ResultSlot. Returns NULL if the translation fails (which post-ensure
 * should be impossible barring catastrophic memory pressure). */
static ResultSlot *kring_translate_slot(process_t *target, uintptr_t uvaddr)
{
    return (ResultSlot *)vmm_translate_user_addr(target->cabin->vmm, uvaddr,
                                                  sizeof(ResultSlot));
}

/* Cross-core wake helper — mirrors touch_wake_remote in touch.c.
 *
 * Real-HW rationale: process_set_state(target, PROC_WORKING) below
 * enqueues the target on its home_core's runqueue, but the core itself
 * may be in HLT / MWAIT / UMWAIT idle. Without an explicit IPI the
 * target only resumes on the next LAPIC timer tick (~1 ms at 1 kHz
 * scheduling, longer if the BIOS configured a lower tick rate). On real
 * Intel silicon with deeper C-states (intel_idle C3+) the wakeup tail
 * can stretch to tens of ms — fatal for IPC reply latency.
 *
 * IPI_WAKE_VECTOR is the existing AMP-wide doorbell; its handler is a
 * no-op acknowledger that triggers a reschedule probe on the receiver.
 * Idempotent — extra IPIs to an already-running core are a few cycles
 * each; missed IPIs are the real risk and we err on the side of always
 * sending. Skip the IPI when target's home is this same core (we'll
 * pick up the reschedule on return to userspace) or when SMP is
 * single-core (no remote core to wake). */
static inline void kring_wake_remote(process_t *target)
{
    if (!target) return;
    if (g_amp.total_cores <= 1) return;
    uint8_t core = target->home_core;
    if (core >= g_amp.total_cores) return;
    if (core == amp_get_core_index()) return;
    lapic_send_ipi(g_amp.cores[core].lapic_id, IPI_WAKE_VECTOR);
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
    /* Delivering is work, and a core doing it is not a core with nothing to
     * do. The mark matters because this runs from interrupt context as well
     * as from a K-Core, and an interrupt does not otherwise disturb the idle
     * mark — so Nightwatch could look during the gap between reserving a slot
     * (step 3) and publishing it (step 8), read a ring that is not empty and
     * an owner not yet woken, and call a delivery in progress an undelivered
     * result. It said so once, out loud, the first hour this machine was able
     * to reach idle at all. One byte store, on the same path that already
     * pays for a page walk. */
    nightwatch_core_busy(amp_get_core_index());

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
     *     ring stays consistent.
     *
     *     With 32-byte ResultSlot stride and 4 KiB pages there are 128
     *     slots per page. The reserved `pos` returned by fetch_add at
     *     step (3) can be at most "tail_snap + concurrent_producers - 1"
     *     ahead of our snapshot — so the actually-used page is either
     *     uvaddr_pre's page or its immediate 4 KiB successor. */
    uintptr_t uvaddr_pre     = result_ring_slot_uvaddr(rr, tail_snap);
    uintptr_t one_page_ahead = uvaddr_pre + 4096u;
    if (vmm_ensure_user_page(target->cabin->vmm, uvaddr_pre, /*writable=*/true) != 0) {
        __atomic_add_fetch(&g_krp_premap_fail, 1, __ATOMIC_RELAXED);
        return false;
    }
    /* Best-effort pre-map of the next page — ONLY when it is still inside
     * this ring's own slot region. The small eager-mapped per-strand rings
     * (P5a) end with an unmapped guard page; pre-mapping past the region would
     * silently fault that guard in and defeat it. It is also pointless: the
     * reserved `pos` always resolves (via modulo) to an in-region page, which
     * the cross-page step below maps if it differs from uvaddr_pre. Harmless
     * for the large lazily-mapped cabin region (the page-ahead is in-region
     * except at the very end, where the wrap is handled the same way). If it
     * fails, the crosspg path re-attempts with a synthetic ERR if needed. */
    uint64_t slots_end = rr->hdr.slots_base + (uint64_t)cap * rr->hdr.slot_size;
    if (one_page_ahead < slots_end) {
        (void)vmm_ensure_user_page(target->cabin->vmm, one_page_ahead, /*writable=*/true);
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
     *     wasn't pre-mapped. Try to map; if THAT fails, treat the slot
     *     as overflow so we publish a synthetic ERR (no strand). */
    uintptr_t uvaddr = result_ring_slot_uvaddr(rr, pos);
    if (uvaddr != uvaddr_pre && uvaddr != one_page_ahead) {
        if (vmm_ensure_user_page(target->cabin->vmm, uvaddr, /*writable=*/true) != 0) {
            __atomic_add_fetch(&g_krp_crosspg_fail, 1, __ATOMIC_RELAXED);
            kprintf("[KRP] WARN: cross-page map failed at pos=%lu pid=%u — "
                    "synthesizing ERR slot to avoid stranding the ring\n",
                    (unsigned long)pos, (unsigned int)target->pid);
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
     *     "ready for write" value.
     *
     *     `tail` was already incremented at step (3), so we may NEVER
     *     abandon the slot without publishing a seq advance — the
     *     consumer expects every reserved slot to eventually transition
     *     `2*round → 2*round+1`, and a forgotten slot would freeze the
     *     ring's modulo position forever.
     *
     *     But we also cannot spin unbounded. If the userspace consumer
     *     has crashed, frozen, or simply fallen catastrophically behind
     *     under real-HW IRQ pressure, an unbounded `for(;;)` here stalls
     *     every K-Core that lands in KResultPush for the same target.
     *     The fix is two-pronged: (a) cap the spin at KRP_SPIN_BUDGET
     *     iterations of cpu_pause(), and (b) periodically probe
     *     `target->destroying` so we bail out early once the cabin is
     *     known to be tearing down.
     *
     *     On budget exhaustion or destroying-detect we set `overflow`
     *     and fall through to publish a synthetic ERR_RESULT_RING_FULL
     *     at step (7). The slot's seq still advances at step (8), the
     *     consumer (if any) sees a benign error, and the ring stays
     *     consistent. Empirically the worst observed spin under 16-core
     *     stress is well below 1<<14 iterations on STRICT QEMU; real
     *     silicon is similar. The 1<<14 budget gives ~500 µs headroom
     *     at 3 GHz with ~100-cycle PAUSE — long enough for any sane
     *     consumer to drain its prior slot, short enough that a frozen
     *     consumer doesn't wedge producer K-Cores for tens of ms (the
     *     historical 1<<20 ≈ 33 ms tail). With the IPI wake at step
     *     (9) the consumer is poked as soon as we publish, so the long
     *     tail is no longer needed. */
    uint64_t round    = pos / cap;
    uint64_t expected = 2u * round;

    enum { KRP_SPIN_BUDGET = 1u << 14 };
    /* Probe `target->destroying` every 4 K PAUSE iterations (≈ 125 µs at
     * 3 GHz with Skylake+-class PAUSE ≈ 140 cycles); 4 probes over the
     * 16 K budget keeps the "early bail when cabin tears down" promise
     * meaningful instead of a single check at spin 0. Mask MUST be
     * strictly smaller than KRP_SPIN_BUDGET-1 or the check degrades to
     * once-per-spin. */
    enum { KRP_DESTROYING_PROBE_MASK = 0x0FFFu };
    bool consumer_lost = false;
    for (uint64_t spins = 0; ; spins++) {
        uint64_t seq = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
        if (seq == expected) break;
        if (spins >= KRP_SPIN_BUDGET) {
            __atomic_add_fetch(&g_krp_spin_limit, 1, __ATOMIC_RELAXED);
            kprintf("[KRP] WARN: spin budget exhausted pid=%u pos=%lu seq=%lu expected=%lu — publishing synthetic ERR\n",
                    (unsigned int)target->pid, (unsigned long)pos,
                    (unsigned long)seq, (unsigned long)expected);
            consumer_lost = true;
            break;
        }
        if ((spins & KRP_DESTROYING_PROBE_MASK) == 0 &&
            __atomic_load_n(&target->destroying, __ATOMIC_ACQUIRE)) {
            __atomic_add_fetch(&g_krp_spin_limit, 1, __ATOMIC_RELAXED);
            consumer_lost = true;
            break;
        }
        cpu_pause();
    }

    /* (7) Write payload. On overflow OR consumer-lost, write a synthetic
     *     error so the consumer (if any wakes up) drains the slot and
     *     the ring continues. Slot is NEVER abandoned mid-round — the
     *     seq advance at step (8) is the linearisation guarantee for
     *     the consumer. */
    if (overflow || consumer_lost) {
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

    /* (9) Wake target if it was sleeping. If overflow we still wake —
     *     the consumer needs to drain the error slot to keep the ring
     *     moving. Skip the wake when consumer_lost since either the
     *     cabin is destroying (state writes during teardown race the
     *     teardown logic, harmless but pointless) or it has been frozen
     *     long enough that one more wake won't help.
     *
     *     IPI follows the state flip: process_set_state enqueues the
     *     target on its home_core's runqueue, but the home core may be
     *     HLT/MWAIT-idle and miss the new work until the next LAPIC
     *     tick. The IPI is the doorbell that fires the reschedule
     *     immediately. Mirrors the touch_wake_remote pattern in
     *     touch.c TouchRestDeliver — without it, ResultRing replies
     *     pick up a multi-ms wakeup tail on real silicon with deep
     *     C-states. Always-send-on-publish is correct: even when the
     *     target is currently running on home_core, an extra IPI is a
     *     few cycles and avoids the race window where state lookup
     *     sees PROC_WORKING but the consumer is in fact idle. */
    if (!consumer_lost) {
        if (process_get_state(target) == PROC_WAITING) {
            process_set_state(target, PROC_WORKING);
        }
        kring_wake_remote(target);
    }
    return !overflow && !consumer_lost;
}
