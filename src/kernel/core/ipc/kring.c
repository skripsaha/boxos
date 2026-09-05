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

Pocket *KPocketPeek(process_t *proc, uint64_t *pos_out)
{
    PocketRing *r = kring_pocket_hdr(proc);
    if (!r) return NULL;
    uint64_t head = __atomic_load_n(&r->hdr.head, __ATOMIC_RELAXED);
    uint64_t tail = __atomic_load_n(&r->hdr.tail, __ATOMIC_ACQUIRE);
    if (head == tail) return NULL;
    if (pos_out) *pos_out = head;

    uintptr_t uvaddr = pocket_ring_slot_uvaddr(r, head);
    return (Pocket *)vmm_translate_user_addr(proc->cabin->vmm, uvaddr, sizeof(Pocket));
}

/* Take the ring past ONE named position, and only while it is still the head.
 *
 * A strand's pocket ring has one producer and, it turns out, TWO consumers:
 * the K-Core guide that drains it, and the syscall gate on the strand's own
 * core, which pops a YIELD pocket it finds at the head so the strand can give
 * its core away without a K-Core round trip (idt.c). Each is single-threaded
 * with itself and neither with the other, and the pop that stood here moved
 * head on from WHATEVER IT READ AT THAT MOMENT. So when both had looked at the
 * same yield — the K-Core between its peek and its pop, the gate arriving for
 * the strand's next submit — the second pop did not take the yield, which was
 * already gone; it took the pocket BEHIND it, which nobody had read.
 *
 * MEASURED, on a machine frozen by exactly that (BIOS 16c, bench): head 0xcbf
 * and tail 0xcbf, four YIELD pockets at 0xcba..0xcbd stamped by the guide,
 * and at 0xcbe a system.broadcast with token 0x46f, pid still 0 and its
 * manifest_size still 0x24 — never read — with its owner spinning for an answer
 * to a question no one had opened. Under a deadline that was a thirty-second
 * stall and a false "failed" from the shell; without one it is a strand that
 * never returns, and Nightwatch names it: ANSWER OWED.
 *
 * A compare-and-swap from the position the caller actually looked at cannot
 * step over anything: if head has moved, the other consumer owns that position
 * and this call takes nothing. Two consumers may still both LOOK at one yield,
 * and that is harmless — a yield carries no work and produces no Result. Every
 * other kind of pocket is read only by the guide, so its position is only ever
 * taken once. The RELEASE keeps the userspace producer's view of "room in the
 * ring" behind the slot that was consumed. */
bool KPocketPopAt(process_t *proc, uint64_t pos)
{
    PocketRing *r = kring_pocket_hdr(proc);
    if (!r) return false;
    uint64_t expected = pos;
    return __atomic_compare_exchange_n(&r->hdr.head, &expected, pos + 1,
                                       /*weak=*/false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
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

/* True when the process's ResultRing holds a PUBLISHED, unconsumed entry
 * whose context carries a non-zero cloakroom token — i.e. an answer to a
 * synchronous submit its owner has not read yet. Such a process must never
 * be committed to PROC_WAITING: every kernel-parked wait (addr_park,
 * touch_await, storage) ends exactly when this reply is consumed, so
 * sleeping past it is the "undelivered result" Nightwatch names — the
 * lost-wake wedge. process_set_state uses this as the final futex-style
 * re-check at the single place sleep is committed. Reads only the ring
 * header and slot seq/context; the owner's userspace is suspended for the
 * duration of the syscall that is asking, so nothing races the scan. */
bool KResultRingHasPendingReply(process_t *proc)
{
    ResultRing *rr = kring_result_hdr(proc);
    if (!rr) return false;
    uint32_t cap = rr->hdr.slot_count_max;
    if (cap == 0) return false;

    uint64_t head = __atomic_load_n(&rr->hdr.head, __ATOMIC_ACQUIRE);
    uint64_t tail = __atomic_load_n(&rr->hdr.tail, __ATOMIC_ACQUIRE);
    if (head == tail) return false;
    uint64_t scan_end = tail;
    if (scan_end - head > cap) scan_end = head + cap;   /* defensive clamp */

    for (uint64_t pos = head; pos < scan_end; pos++) {
        uintptr_t   uva  = result_ring_slot_uvaddr(rr, pos);
        ResultSlot *slot = kring_translate_slot(proc, uva);
        if (!slot) continue;
        uint64_t round = pos / cap;
        if (__atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE) != 2u * round + 1u)
            continue;                    /* in-flight reservation or consumed */
        if (KCTX_COOKIE24(slot->r.context) != 0) return true;
    }
    return false;
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
static volatile uint64_t g_krp_full;
static volatile uint64_t g_krp_map_fail;
static volatile uint64_t g_krp_translate_fail;
static volatile uint64_t g_krp_contended;
static volatile uint64_t g_krp_seq_broken;
static volatile uint64_t g_krp_success;

void KResultPushStats(uint64_t out[9])
{
    out[0] = __atomic_load_n(&g_krp_null_args,      __ATOMIC_RELAXED);
    out[1] = __atomic_load_n(&g_krp_no_hdr,         __ATOMIC_RELAXED);
    out[2] = __atomic_load_n(&g_krp_zero_cap,       __ATOMIC_RELAXED);
    out[3] = __atomic_load_n(&g_krp_full,           __ATOMIC_RELAXED);
    out[4] = __atomic_load_n(&g_krp_map_fail,       __ATOMIC_RELAXED);
    out[5] = __atomic_load_n(&g_krp_translate_fail, __ATOMIC_RELAXED);
    out[6] = __atomic_load_n(&g_krp_contended,      __ATOMIC_RELAXED);
    out[7] = __atomic_load_n(&g_krp_seq_broken,     __ATOMIC_RELAXED);
    out[8] = __atomic_load_n(&g_krp_success,        __ATOMIC_RELAXED);
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

    /* ── Claim a position, or refuse. Nothing here waits on the consumer. ──
     *
     * The claim used to be an unconditional fetch_add, and everything that
     * followed was repair work for a claim taken before it was known to be
     * good: a re-check for having passed the consumer, a cross-page map that
     * could fail with the position already spent, and a wait on the slot's
     * Vyukov gate capped at a guessed 16384 PAUSEs. Past the guess the code
     * declared the consumer lost and published a synthetic ERR in place of the
     * caller's answer. The guess is a lie in both directions on a loaded host
     * — long enough to hold a K-Core for half a millisecond, short enough to
     * discard a live reply because a vCPU was descheduled — and its failure
     * mode was to answer a syscall with an error it never earned.
     *
     * The claim is now taken only when it is provably good, by CAS on the
     * tail, and it rests on one fact about the consumer:
     *
     *   THE CONSUMER RELEASES A SLOT'S seq BEFORE IT ADVANCES head.
     *   (boxlib result.c: `slot->seq = expected+1` RELEASE, then
     *    `hdr.head = pos+1` RELEASE — in that order, always.)
     *
     * So every position in [head, head + cap) names a slot the consumer has
     * already finished with. A producer that claims only inside that window
     * claims a slot that is ALREADY free: no gate spin, no budget, no
     * destroying-probe woven through it, no synthetic ERR, no "consumer lost".
     *
     * Every failure — full ring, no page, no translation — now happens BEFORE
     * the claim, so a claimed slot is always filled. That closes the window
     * this function used to document and accept: "this single slot remains
     * stuck until process exit", which froze one modulo position of a cabin's
     * reply ring for the life of the process.
     *
     * `head` lives in a page the guest can write. A guest that reports a head
     * it has not reached makes the kernel reuse a slot it is still reading —
     * and corrupts its OWN reply stream, in its OWN page. It is counted and
     * said out loud rather than waited on; the previous code's answer to the
     * same lie was to stall for half a millisecond and corrupt it anyway. */
    /* ── The answer's room is reserved, so an answer is never refused. ─────
     *
     * Two different things arrive in a strand's reply ring, and only one of
     * them has somebody asleep on it:
     *
     *   an ANSWER carries a cloakroom token (KCTX_COOKIE24) — it is the reply
     *   to a Pocket this strand submitted, and its owner is parked on it. Lose
     *   it and the strand sleeps forever on a question already answered.
     *
     *   UNSOLICITED traffic — IPC another process chose to send — has no
     *   token. Nobody is committed to it, and its sender learns of the refusal
     *   and may act on it.
     *
     * A strand can have at most `pocket_cap` submits outstanding, because that
     * is how many Pockets fit in the ring it submits through — so if the last
     * `pocket_cap` slots of the reply ring are kept for answers, an answer can
     * never find the ring full. The geometry already grants it (a Hammock
     * strand has 256 Pocket slots against 512 Result slots; the cabin's rings
     * are the same 1 MiB against 1 MiB, and a Result slot is the smaller), so
     * this reserves nothing that unsolicited traffic had any right to.
     *
     * That is the invariant the rest of the system's honesty rests on: it is
     * what makes it correct for a caller to wait for an answer WITHOUT a
     * deadline, and a deadline on a guaranteed answer is exactly how a slow
     * machine turns a completed operation into a false refusal — while the
     * abandoned caller's stack-resident Manifest and Crates are still going to
     * be read and written by the K-Core (see boxlib manifest.c ManifestSubmit).
     * Reserve the room here, and that whole class of bug has nowhere to live. */
    uint32_t limit = cap;
    if (KCTX_COOKIE24(r->context) == 0) {
        PocketRing *pr = kring_pocket_hdr(target);
        uint32_t reserve = pr ? pr->hdr.slot_count_max : (cap / 2u);
        limit = (cap > reserve) ? (cap - reserve) : (cap / 2u);
        if (limit == 0) limit = 1u;
    }

    ResultSlot *slot    = NULL;
    uintptr_t   ensured = 0;   /* page whose mapping this call has secured */
    uint32_t    turns   = 0;
    uint64_t    pos     = 0;

    for (;;) {
        /* head BEFORE tail, and never the other way round.
         *
         * Both cursors only ever grow and tail >= head is the ring's own
         * invariant — but that is a statement about the ring at one instant,
         * not about two loads taken at two. Read tail first and the consumer
         * can advance head past it in between; the snapshot then has head >
         * pos, `pos - head` wraps to a colossal unsigned number, and the ring
         * declares itself full when it is in fact empty. Reading head FIRST
         * makes the order do the work: tail is sampled later, so it cannot be
         * behind the head we already have.
         *
         * MEASURED, not reasoned into place afterwards: with the loads the
         * other way round this refused an ANSWER once in a two-hour matrix,
         * reporting "full at 4294967295 of 32768 slots", and the strand
         * waiting on that answer never woke. */
        uint64_t head = __atomic_load_n(&rr->hdr.head, __ATOMIC_ACQUIRE);
        pos           = __atomic_load_n(&rr->hdr.tail, __ATOMIC_RELAXED);
        if (pos - head >= limit) {
            uint64_t n = __atomic_fetch_add(&g_krp_full, 1, __ATOMIC_RELAXED);
            /* Refusing an ANSWER is not a statistic. Somebody is parked on it,
             * and every caller of this function drops the return value, so the
             * refusal would otherwise be perfectly silent — and the strand
             * would simply never wake. Say it, name the victim, and rate-limit
             * so a saturated ring cannot drown the console it is warning. */
            if (KCTX_COOKIE24(r->context) != 0 && (n & 0x3Fu) == 0) {
                kprintf("[KRP] ERROR: dropped an ANSWER for pid %u (token %u) — "
                        "reply ring full at %u of %u slots. The strand waiting "
                        "on that answer will not be woken by it\n",
                        (unsigned int)target->pid,
                        (unsigned int)KCTX_COOKIE24(r->context),
                        (unsigned int)(pos - head), (unsigned int)cap);
            }
            return false;
        }

        uintptr_t uvaddr = result_ring_slot_uvaddr(rr, pos);
        uintptr_t page   = uvaddr & ~(uintptr_t)(VMM_PAGE_SIZE - 1);
        if (page != ensured) {
            if (vmm_ensure_user_page(target->cabin->vmm, uvaddr,
                                     /*writable=*/true) != 0) {
                __atomic_add_fetch(&g_krp_map_fail, 1, __ATOMIC_RELAXED);
                return false;
            }
            ensured = page;
        }

        slot = kring_translate_slot(target, uvaddr);
        if (!slot) {
            __atomic_add_fetch(&g_krp_translate_fail, 1, __ATOMIC_RELAXED);
            return false;
        }

        /* The linearisation point. A failure here means ANOTHER PRODUCER won
         * the tail — the ring moved forward, so this is lock-free progress and
         * not a spin on someone else's liveness. The failing exchange refreshes
         * `pos` with the winner's value, so the next turn considers the
         * position that actually came free. */
        if (__atomic_compare_exchange_n(&rr->hdr.tail, &pos, pos + 1,
                                        /*weak=*/true, __ATOMIC_ACQ_REL,
                                        __ATOMIC_RELAXED)) {
            break;
        }
        if (++turns == 1) __atomic_add_fetch(&g_krp_contended, 1, __ATOMIC_RELAXED);
    }

    uint64_t round    = pos / cap;
    uint64_t expected = 2u * round;

    /* The claim's own guarantee, read back once. One ACQUIRE load, and it is
     * the only thing that can tell a broken consumer from a healthy one — so
     * it is read, counted and named, never waited on. */
    if (__atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE) != expected) {
        uint64_t n = __atomic_fetch_add(&g_krp_seq_broken, 1, __ATOMIC_RELAXED);
        if ((n & 0x3FFu) == 0) {
            kprintf("[KRP] WARN: pid %u reported a head its consumer has not "
                    "reached (slot %lu of round %lu was not released); its own "
                    "reply stream is what gets overwritten\n",
                    (unsigned int)target->pid, (unsigned long)(pos % cap),
                    (unsigned long)round);
        }
    }

    slot->r = *r;
    __atomic_add_fetch(&g_krp_success, 1, __ATOMIC_RELAXED);

    /* Publish — the RELEASE store makes the payload above visible to the
     * consumer's ACQUIRE load of the same word. */
    __atomic_store_n(&slot->seq, expected + 1u, __ATOMIC_RELEASE);

    /* Wake. The home core may be HLT/MWAIT-idle and would not learn of the
     * reply until the next LAPIC tick; the IPI is the doorbell.
     *
     * A token-carrying Result ANSWERS a submit — its owner is kernel-parked on
     * it, or mid-transition to that park. The flip is unconditional for that
     * case: a conditional one lost the race against a parker that had not yet
     * committed PROC_WAITING, and the strand then slept forever on a reply
     * already published. For a running target the transition is a benign
     * no-op, and process_set_state's death-guard still refuses corpses. */
    if (KCTX_COOKIE24(r->context) != 0) {
        process_set_state(target, PROC_WORKING);
    } else if (process_get_state(target) == PROC_WAITING) {
        process_set_state(target, PROC_WORKING);
    }
    kring_wake_remote(target);
    return true;
}
