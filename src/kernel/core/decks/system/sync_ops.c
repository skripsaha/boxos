/*
 * sync_ops.c — SYSTEM_OP_ADDR_PARK / SYSTEM_OP_ADDR_WAKE handlers.
 *
 * These are the kernel substrate for std::atomic::wait / notify_one /
 * notify_all and (via __boxcxx_atomic_wait_cycle) every C++ blocking primitive.
 *
 * ASYNC-COMPLETION MODEL (the load-bearing invariant — see boxcxx audit):
 * the parked caller blocks in boxlib result_wait() on ITS ResultRing, so a wake
 * MUST write that same ResultRing or the caller never sees it. (The earlier
 * design relied on guide.c's transient ERR_WOULD_BLOCK ack — but result_wait
 * filters that ack out, so notify/addr_wake could not break the wait early and
 * timed waits slept to their deadline. That was the Ф20d timed-wake bug.)
 *
 *   - park: register an AddrWaitEntry, arm a timeout reschedule, set PROC_WAITING,
 *     and go ASYNC (set *async_owns_crates so guide does NOT push a reply) —
 *     the completion arrives later, exactly like the write_job async path.
 *   - EARLY WAKE (notify): addr_wake CLAIMS the entry (AddrWaitClaim: done 0->1 +
 *     unlink, atomically under the bucket lock) and KResultPushes ONE OK Result,
 *     then flips PROC_WAITING -> PROC_WORKING + IPI. result_wait returns at once.
 *     addr_wake runs on the K-Core, so KResultPush (which touches the cabin VMM)
 *     is safe there. The parker's step-6 lost-wakeup recheck can also CLAIM (when
 *     the watched value already changed) and complete synchronously.
 *   - TIMEOUT: armed via TouchQueueWakeAfter. When the timer fires (PIT IRQ),
 *     touch_queue_fire_wake passes the process's own baton (a never-drop
 *     embedded node, baton.h) — it does NO ResultRing/VMM work in the PIT
 *     tick. SyncTimeoutDeliver then runs on a K-Core: it CLAIMS the entry by
 *     its state — linked, timed, deadline passed — (arbitrating against a
 *     racing addr_wake — exactly one delivers) and, on a win, KResultPushes
 *     ONE ERR_TIMEOUT Result, so the caller's result_wait returns AT the
 *     deadline. Delivering the timeout Result FROM the timer IRQ was tried and
 *     REVERTED: doing VMM work in interrupt context starved cores from ACKing
 *     TLB shootdowns under 16-core load (shootdown-timeout panic) — hence the
 *     K-Core hand-off, where KResultPush (which touches the cabin VMM) is safe.
 *     A claim LOSS means addr_wake already delivered, the parker's recheck
 *     did, or the strand re-parked with a deadline still ahead — nothing owed.
 *
 * Lock ordering (never violated here):
 *   vmm translate (holds ctx->lock) completes BEFORE bucket lock is taken.
 *   TouchPublish happens OUTSIDE the bucket lock.
 *   No scheduler_lock / process_lock is held across the bucket lock.
 */

#include "system_deck.h"
#include "chit.h"       /* ChitGive / ChitDue — the promise of an answer, and its falling due */
#include "sync_ops.h"
#include "addr_wait.h"
#include "op_registry.h"
#include "manifest_auth.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "process.h"
#include "vmm.h"
#include "touch.h"
#include "touch_queue.h"
#include "kring.h"        /* KResultPush — deliver the wake completion to ResultRing */
#include "result.h"       /* Result */
#include "boxos_kctx.h"   /* KCTX_GUIDE */
#include "error.h"
#include "klib.h"
#include "atomics.h"
#include "amp.h"
#include "pid_allocator.h"   /* pid_life — the monotone per-slot generation decides "gone" exactly */
#include "lapic.h"
#include "irqchip.h"
#include "scheduler.h"   /* g_global_tick, SCHEDULER_DEFAULT_TICK_HZ */
#include "boxos_decks.h"

/* -------------------------------------------------------------------------
 * SysAddrPark — park the calling process until *addr != expected or woken.
 *   params: [u64 user_va][u64 expected][u32 timeout_ms]  (20 bytes)
 *
 * Mirrors SysTouchAwait step-by-step (touch_ops.c:222-321):
 *   1.  VA → phys translate (before taking any bucket lock).
 *   2.  Value pre-check: if *phys != expected → return immediately.
 *   3.  Register AddrWaitEntry under bucket lock.
 *   4.  Arm an IRQ-safe timeout reschedule via TouchQueueWakeAfter (PROC_WORKING
 *       + IPI on expiry; the caller's result_wait call_timeout yields ERR_TIMEOUT).
 *   5.  process_set_state(PROC_WAITING).
 *   6.  Lost-wakeup recheck under ACQUIRE (reload *phys + entry.done) with
 *       claim arbitration — see the inline comment at step 6.
 *   7.  Go ASYNC (set *async_owns_crates) and return ERR_WOULD_BLOCK so guide
 *       pushes NO reply now. The early-wake completion is a real Result
 *       KResultPushed onto this caller's ResultRing by SysAddrWake (on a notify),
 *       which also flips PROC_WAITING -> PROC_WORKING + IPI. That Result is what
 *       ends the caller's result_wait; the IPI alone would only resume a poll.
 * ------------------------------------------------------------------------- */
static int SysAddrPark(const ManifestOp *op, Crate *crates,
                       uint16_t crate_count, const OpContext *ctx)
{
    (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc)       return ERR_INVALID_ARGUMENT;
    if (op->param_size < 20)      return ERR_INVALID_ARGUMENT;

    uint64_t user_va;
    uint64_t expected;
    uint32_t timeout_ms;
    memcpy(&user_va,    op->params,      sizeof(uint64_t));
    memcpy(&expected,   op->params + 8,  sizeof(uint64_t));
    memcpy(&timeout_ms, op->params + 16, sizeof(uint32_t));

    /* Step 1: VA → phys.  vmm_virt_to_phys holds ctx->lock internally;
     * it must complete before we take any bucket lock. */
    uintptr_t phys = 0;
    if (ctx->proc->cabin) {
        phys = vmm_virt_to_phys(ctx->proc->cabin->vmm, user_va);
    }
    if (phys == 0) return ERR_INVALID_ADDRESS;

    /* Step 2: value pre-check — no park if value already changed. */
    volatile uint64_t *kaddr = (volatile uint64_t *)vmm_phys_to_virt(phys);
    uint64_t actual = __atomic_load_n(kaddr, __ATOMIC_ACQUIRE);
    if (actual != expected) return ERR_ADDR_VALUE_MISMATCH;

    /* Step 3: register the embedded per-process entry (never stack-allocated).
     * Drop any stale linkage from a previous park whose wake/timeout left
     * the entry in an inert-linked state. */
    AddrWaitEntry *entry = &ctx->proc->addr_wait_entry;
    AddrWaitUnlinkIfLinked(entry);

    /* The chit BEFORE the link, and before any lock: from the link onward a
     * waker can claim this entry, and a claim marks due the promise it finds —
     * so the promise must already be there. This also sets the async flag for
     * every exit below; a path that answers synchronously (an early error, or
     * the recheck winning its own claim) is simply pushed by the guide, and
     * that push keeps the chit. Nothing else here writes the flag. */
    ChitGive(ctx, "system.addr.park", (uint64_t)phys);

    uintptr_t space = (uintptr_t)ctx->proc->cabin->vmm;
    AddrWaitBucket *bucket = AddrWaitGetBucket(space, user_va);
    if (!bucket) return ERR_NO_MEMORY;

    /* Arm the entry UNDER the bucket lock — the lock that guards the chain
     * it is about to join — so a concurrent SysAddrWake walking this bucket
     * never observes a half-armed entry.  (Between the unlink above and this
     * link the entry is in no chain, so wakers cannot reach it.)  Previously
     * these stores sat outside the lock, correct only by statement order. */
    /* The deadline, if the caller named one, in the tick's own clock. Fixed
     * before the link so the entry carries it from its first instant on the
     * chain: the delivery judges by it, under this same lock. */
    uint64_t fire_at = 0;
    if (timeout_ms > 0) {
        uint64_t delay = ((uint64_t)timeout_ms * SCHEDULER_DEFAULT_TICK_HZ)
                         / 1000ULL;
        if (delay == 0) delay = 1;
        fire_at = __atomic_load_n(&g_global_tick, __ATOMIC_RELAXED) + delay;
    }

    spin_lock(&bucket->lock);
    entry->proc          = ctx->proc;
    entry->space         = space;
    entry->phys_addr     = phys;
    entry->done          = 0;
    entry->submit_cookie = ctx->submit_cookie;
    /* Nightwatch's evidence, armed with the entry so a stalled system can be
     * described without re-deriving what this park was waiting for. */
    entry->user_va   = user_va;
    entry->expected  = expected;
    entry->timed     = (timeout_ms > 0) ? 1u : 0u;
    entry->fire_at   = fire_at;
    AddrWaitLink(bucket, entry);   /* bumps entry->seq for THIS park */
    spin_unlock(&bucket->lock);

    /* Step 4: arm the deadline (same helper SysTouchAwait uses). On expiry the
     * PIT tick reschedules us (PROC_WORKING + IPI, in-IRQ, no VMM) AND passes
     * this process's own baton (process_t.deadline_baton) to a K-Core, where
     * SyncTimeoutDeliver claims the park by its state — linked, timed, deadline
     * passed — and KResultPushes ERR_TIMEOUT, so the caller's result_wait
     * returns AT the deadline. The pass rides an embedded node, so it cannot
     * be dropped; the caller waits for that Result without any clock of its
     * own. Early wake takes the K-Core delivery path through addr_wake (see
     * SysAddrWake); the claim arbitrates which one delivers.
     *
     * Claim this sleep's number before arming anything for it. Every wake in
     * the queue carries the number of the sleep it was armed for, and the
     * tick acts only while the two agree — so the wake left behind by a wait
     * that ended early cannot reach the park that follows it. */
    uint32_t park_seq = __atomic_add_fetch(&ctx->proc->park_seq, 1,
                                           __ATOMIC_ACQ_REL);
    if (timeout_ms > 0)
        TouchQueueWakeAfter(ctx->proc->pid, fire_at, /*owes_result=*/1u, park_seq);

    /* Step 5: park (identical to SysTouchAwait line 275). */
    process_set_state(ctx->proc, PROC_WAITING);

    /* Step 6: lost-wakeup recheck — async-completion arbitration.
     *
     * addr_park is an ASYNC op: the caller blocks in boxlib result_wait() on
     * its ResultRing, and the COMPLETION is a real Result pushed by whoever
     * claims this entry (AddrWaitClaim: done 0->1 + unlink, atomically). The
     * claim is the single arbitration point — exactly one of {addr_wake (OK),
     * the deferred timeout (ERR_TIMEOUT), this parker's recheck (OK)} claims and
     * delivers the one completion Result. Here the parker races both a
     * concurrent addr_wake and the timeout after setting PROC_WAITING:
     *
     *   (a) value already changed AND we win the claim: no addr_wake will
     *       deliver, so complete SYNCHRONOUSLY — return a non-async error so
     *       guide pushes it as our Result.
     *   (b) a concurrent addr_wake already claimed us (done set), or we LOSE the
     *       value-changed claim to one: that winner OWNS delivery — go ASYNC and
     *       let its KResultPush end our result_wait.
     *   (c) genuine park (value==expected, unclaimed): go ASYNC and block; a
     *       future addr_wake claims+delivers, or the timeout reschedules us out.
     *
     * Going async means setting *async_owns_crates so guide does NOT push the
     * transient ERR_WOULD_BLOCK ack as our reply — the real completion arrives
     * later (mirrors the write_job async path). The ACQUIRE on *kaddr pairs
     * with the writer's RELEASE store; the claim's bucket lock orders done. */
    uint64_t recheck = __atomic_load_n(kaddr, __ATOMIC_ACQUIRE);
    bool completed   = (__atomic_load_n(&entry->done, __ATOMIC_ACQUIRE) != 0);

    if (!completed && recheck != expected) {
        if (AddrWaitClaim(entry)) {
            /* (a) We own delivery — finish synchronously through guide. If our
             * armed timeout later fires, SyncTimeoutDeliver finds the entry
             * already claimed (AddrWaitClaim fails) and only does a bare
             * PROC_WORKING reschedule — idempotent for an already-running
             * strand (at worst a benign spurious wake if it re-parked). */
            process_set_state(ctx->proc, PROC_WORKING);
            return ERR_ADDR_VALUE_MISMATCH;
        }
        completed = true;   /* lost the claim — a waker owns delivery */
    }

    if (completed) {
        /* (b) A concurrent addr_wake claimed us and will KResultPush the
         * completion. Reschedule to WORKING and wait async for it to arrive
         * (the async flag was set with the chit above). */
        process_set_state(ctx->proc, PROC_WORKING);
        return ERR_WOULD_BLOCK;
    }

    /* (c) Genuine park. The entry stays linked (done=0); a claimer unlinks it
     * and delivers our completion Result. Publish strand:parked outside any
     * lock (mirrors SysTouchAwait). */
    {
        struct __attribute__((packed)) { uint64_t phys; uint32_t pid; } ev;
        ev.phys = (uint64_t)phys;
        ev.pid  = ctx->proc->pid;
        TouchPublish("strand:parked", &ev, sizeof(ev));
    }

    /* Step 7: go async and return ERR_WOULD_BLOCK. guide does NOT push a reply
     * now (the async flag went up with the chit); the parked caller's
     * result_wait blocks until addr_wake / the park-timeout KResultPushes the
     * real completion, which keeps the chit. */
    return ERR_WOULD_BLOCK;
}

/* -------------------------------------------------------------------------
 * SysAddrWake — wake up to `count` processes parked on `user_va`.
 *   params: [u64 user_va][u32 count]  (12 bytes; count==0 means all)
 *
 * Mirrors touch_queue_fire_wake (touch_queue.c:105) for each wakee.
 * ------------------------------------------------------------------------- */
static int SysAddrWake(const ManifestOp *op, Crate *crates,
                       uint16_t crate_count, const OpContext *ctx)
{
    (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc)  return ERR_INVALID_ARGUMENT;
    if (op->param_size < 12) return ERR_INVALID_ARGUMENT;

    uint64_t user_va;
    uint32_t count;
    memcpy(&user_va, op->params,     sizeof(uint64_t));
    memcpy(&count,   op->params + 8, sizeof(uint32_t));

    /* The word must be the caller's to name: a VA with nothing behind it is
     * a caller error, said as such. The translation is only that check —
     * the wait is keyed by (space, va), not by the page. */
    if (!ctx->proc->cabin || vmm_virt_to_phys(ctx->proc->cabin->vmm, user_va) == 0)
        return ERR_INVALID_ADDRESS;
    uintptr_t space = (uintptr_t)ctx->proc->cabin->vmm;

    /* Look up bucket.  If no slab exists for this address yet, there are no
     * waiters — return OK silently (no waiters is not an error). */
    AddrWaitBucket *bucket = AddrWaitGetBucket(space, user_va);
    if (!bucket) return OK;

    /* Claim every waiter parked on THIS address now — up to `count`, all of
     * them for 0 — under one hold of the bucket lock, and deliver outside it.
     * Claiming a waiter means setting done=1 AND unlinking it atomically
     * (here, under this lock) — the claimer then OWNS that waiter's single
     * completion Result. Unlinking on claim (rather than leaving the parked
     * side to self-remove) is what makes a later claimer — a stale
     * park-timeout, or a re-park — unable to match an already-completed
     * waiter.
     *
     * The claimed entries ride to their delivery on their own link
     * (claimed_next): no tray between the claim and the push, so nothing to
     * size. What stood here was a stack array of 256 and a walk that stopped
     * when it was full — a notify_all with more sleepers than that woke the
     * first 256 and left the rest asleep for ever, and nothing said so. */
    AddrWaitEntry  *claimed      = NULL;
    AddrWaitEntry **claimed_tail = &claimed;
    uint32_t        wake_count   = 0;

    spin_lock(&bucket->lock);
    AddrWaitEntry *e = bucket->head;
    while (e && (count == 0 || wake_count < count))
    {
        AddrWaitEntry *next = e->next;   /* save: AddrWaitUnlink nulls e->next */
        /* The bucket is a HASH of (space, va), so a chain can hold waiters
         * parked on DIFFERENT words that collide. Wake only waiters on THIS
         * exact word — otherwise a colliding waiter consumes the (count==1)
         * notify_one budget and the intended waiter misses its wake (lost
         * wakeup for a mutex/semaphore handoff). */
        if (e->space == space && e->user_va == user_va &&
            AddrWaitClaimLocked(bucket, e)) {
            /* Claimed — we own this waiter's one completion, and its chit is
             * DUE from here (AddrWaitClaimLocked marks it under this lock). */
            process_ref_inc(e->proc);
            e->claimed_next = NULL;
            *claimed_tail   = e;
            claimed_tail    = &e->claimed_next;
            wake_count++;
        }
        e = next;
    }
    spin_unlock(&bucket->lock);

    /* Fire wakes outside the bucket lock — mirrors touch_queue_fire_wake
     * (touch_queue.c:105-119) exactly. */
    uint32_t waker_pid = ctx->proc->pid;
    while (claimed) {
        /* Everything this delivery needs is read BEFORE the push. The Result
         * is the only thing that lets the parked strand run again — the
         * timeout arm finds the entry already claimed and pushes nothing —
         * and its next park rewrites this entry, link included. */
        AddrWaitEntry *next   = claimed->claimed_next;
        process_t     *target = claimed->proc;
        uint32_t       cookie = claimed->submit_cookie;
        claimed = next;

        if (!target->destroying) {
            /* We won the claim, so we OWN this waiter's completion: deliver
             * exactly one Result onto its ResultRing — the channel its boxlib
             * result_wait actually monitors. This is the Ф20d fix; the IPI
             * alone only reschedules the waiter into a futile poll loop. Push
             * BEFORE the state flip so the woken strand finds its Result the
             * instant it is rescheduled (mirrors write_job wjob_finalize). */
            Result r;
            memset(&r, 0, sizeof(r));
            r.error_code = OK;
            r.sender_pid = 0;
            /* Echo the PARK's own cloakroom token — this Result answers the
             * parked strand's submit, and its paired wait adopts only that. */
            r.context    = KCTX_PACK24(KCTX_GUIDE, cookie);
            KResultPush(target, &r);

            /* The target's armed timeout (TouchQueueWakeAfter) is left in place;
             * if it fires later, SyncTimeoutDeliver finds this entry already
             * claimed (we won here) — AddrWaitClaim fails, so it only does a bare
             * PROC_WORKING reschedule (no Result, no ERR_TIMEOUT). At worst a
             * benign spurious wakeup for a re-parked strand, which re-checks its
             * predicate. No IRQ-context cleanup. */
            if (process_get_state(target) == PROC_WAITING) {
                process_set_state(target, PROC_WORKING);
                if (g_amp.total_cores > 1) {
                    uint8_t core = target->home_core;
                    if (core < g_amp.total_cores && core != amp_get_core_index()) {
                        lapic_send_ipi(g_amp.cores[core].lapic_id, IPI_WAKE_VECTOR);
                    }
                }
            }
        }

        /* Publish outside bucket lock (per spec). */
        {
            struct __attribute__((packed)) {
                uint64_t va;
                uint32_t pid;
                uint32_t waker_pid;
            } ev;
            ev.va        = user_va;
            ev.pid       = target->pid;
            ev.waker_pid = waker_pid;
            TouchPublish("strand:woken", &ev, sizeof(ev));
        }

        process_ref_dec(target);
    }

    return OK;
}

/* -------------------------------------------------------------------------
 * SyncTimeoutDeliver — the deadline's continuation, run on a K-Core.
 *
 * Passed by touch_queue_fire_wake (the PIT-tick WAKE path) on the process's
 * own baton; ctx is the process, and the pass holds a ref on it that is
 * released here. The reschedule out of PROC_WAITING already happened in the
 * IRQ; this runs purely to deliver the Result, where KResultPush (cabin VMM)
 * is safe — NEVER in IRQ context (the 16c shootdown-panic lesson).
 *
 * It is the deadline arm of the same claim arbitration SysAddrWake uses, and
 * it judges by the park's STATE, not by a token from the fire that woke it:
 *
 *   - CLAIM WON (entry linked, timed, deadline at or before now): a real
 *     addr_park waiter whose deadline elapsed before any notify. We OWN
 *     delivery — KResultPush ONE ERR_TIMEOUT onto its ResultRing (the channel
 *     boxlib result_wait monitors). result_wait returns with ERR_TIMEOUT at
 *     the deadline. A defensive re-flip handles the rare case the IRQ
 *     reschedule was undone by a re-park before this push.
 *   - CLAIM LOST: SysAddrWake already delivered an OK Result, OR the parker's
 *     own recheck completed it, OR the strand re-parked with a deadline still
 *     ahead. In every case no Result is owed here — do nothing.
 *
 * One baton per process, gated by deadline_passed: the gate is cleared FIRST,
 * with a full fence, so a deadline that fires from here on passes the baton
 * afresh, and one that fired before the clear is answered by the state read
 * below — the fire's CAS on the gate is itself a full fence, so the two
 * orders cannot both miss (Dekker). A fire that landed between the park's
 * link and its sleep is answered the same way: the park's deadline has
 * passed, and it is claimed.
 * ------------------------------------------------------------------------- */
void SyncTimeoutDeliver(void *ctx)
{
    process_t *target = (process_t *)ctx;

    __atomic_store_n(&target->deadline_passed, 0u, __ATOMIC_SEQ_CST);

    uint64_t now    = __atomic_load_n(&g_global_tick, __ATOMIC_RELAXED);
    uint32_t cookie = 0;
    if (!target->destroying &&
        AddrWaitClaimDue(&target->addr_wait_entry, now, &cookie)) {
        /* We own this waiter's single completion — deliver ERR_TIMEOUT, the
         * deadline Result. Push BEFORE any state flip so the woken strand
         * finds its Result the instant it is rescheduled (mirrors
         * SysAddrWake's OK-delivery tail). */
        Result r;
        memset(&r, 0, sizeof(r));
        r.error_code = ERR_TIMEOUT;
        r.sender_pid = 0;
        /* Same token the claimed park armed — see the wake path above. */
        r.context    = KCTX_PACK24(KCTX_GUIDE, cookie);
        KResultPush(target, &r);

        if (process_get_state(target) == PROC_WAITING) {
            process_set_state(target, PROC_WORKING);
            if (g_amp.total_cores > 1) {
                uint8_t core = target->home_core;
                if (core < g_amp.total_cores && core != amp_get_core_index()) {
                    lapic_send_ipi(g_amp.cores[core].lapic_id, IPI_WAKE_VECTOR);
                }
            }
        }
    }

    process_ref_dec(target);   /* the pass's ref */
}

/* -------------------------------------------------------------------------
 * SysProcessGone — wait until a named incarnation (pid, generation) is gone.
 *
 * The question is about a STATE, so it is answered from state, and the answer
 * is exact in every case without keeping any history:
 *
 *   never issued  — the generation is ahead of everything that pid slot has
 *                   ever handed out. A real error: saying "finished" about a
 *                   process that never ran reads exactly like success.
 *   departed      — the slot has moved on, or holds this generation and is
 *                   free. Answered immediately, no park.
 *   live          — park, and let the one place a life ends deliver.
 *
 * The park is armed against the death's own commit point, not against a
 * separate flag: TouchCleanupProcess claims `touch_cleaned` exactly once and
 * only then drains the gone list. Reading that flag while holding gone_lock
 * therefore has exactly two outcomes — the death is already committed (answer
 * now, never park) or it is not (we are on the list before the drain can run).
 * There is no third interleaving, hence no lost wake and no need for the
 * value-recheck dance addr_park has to do against an unlocked user word.
 * ------------------------------------------------------------------------- */
static int SysProcessGone(const ManifestOp *op, Crate *crates,
                          uint16_t crate_count, const OpContext *ctx)
{
    (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc)  return ERR_INVALID_ARGUMENT;
    if (op->param_size < 8)  return ERR_INVALID_ARGUMENT;

    uint32_t pid, want_gen;
    memcpy(&pid,      op->params,     sizeof(uint32_t));
    memcpy(&want_gen, op->params + 4, sizeof(uint32_t));

    if (pid == ctx->proc->pid && want_gen == ctx->proc->generation)
        return ERR_INVALID_ARGUMENT;   /* waiting for your own end never ends */

    process_t *target = process_find_ref(pid);
    if (!target || target->generation != want_gen) {
        if (target) process_ref_dec(target);
        /* No live record of that incarnation — the allocator's monotone
         * per-slot counter still decides it exactly. */
        switch (pid_life(pid, want_gen)) {
        case PID_LIFE_DEPARTED: return OK;
        case PID_LIFE_LIVE:
            /* The number is issued to exactly this generation, yet the record
             * is not in the table: the only window where that holds is inside
             * process_create, between pid_alloc and the hash insert — and a
             * caller cannot be in it, because the pid is not handed out until
             * creation has finished. Reported as BUSY rather than WOULD_BLOCK
             * on purpose: boxlib filters WOULD_BLOCK out of the reply stream
             * (it is the async-park ack), so returning it here would leave the
             * caller waiting on an answer that was thrown away. */
            return ERR_BUSY;
        default:                return ERR_PROCESS_NOT_FOUND;
        }
    }

    GoneWaiter *w = (GoneWaiter *)kmalloc(sizeof(GoneWaiter));
    if (!w) { process_ref_dec(target); return ERR_NO_MEMORY; }
    w->waiter          = ctx->proc;
    w->want_generation = want_gen;
    w->submit_cookie   = ctx->submit_cookie;

    /* The chit before the list, outside gone_lock (ChitGive may speak, and
     * gone_lock is taken alone). It also raises the async flag; the "death
     * already committed" exit below then answers synchronously through the
     * guide, whose push keeps the chit. */
    ChitGive(ctx, "system.process.gone", ((uint64_t)want_gen << 32) | pid);

    spin_lock(&target->gone_lock);
    if (__atomic_load_n(&target->touch_cleaned, __ATOMIC_ACQUIRE)) {
        /* Death already committed — its drain has run or is running, and it
         * will not see us. Answer here instead of sleeping on a debt nobody
         * still owes. */
        spin_unlock(&target->gone_lock);
        kfree(w);
        process_ref_dec(target);
        return OK;
    }
    w->next              = target->gone_waiters;
    target->gone_waiters = w;
    /* The WAITER's record must outlive this park too. The death delivers into
     * that record's ResultRing, so a strand killed while parked here would be
     * freed — rings included — underneath KResultPush. (This is the same
     * reason the IPC and storage completion paths ref their targets before
     * pushing.) Delivery to a corpse is already harmless: process_set_state's
     * death-guard refuses to resurrect one, so the push becomes a no-op. What
     * must not happen is the struct disappearing while it is written to. */
    process_ref_inc(ctx->proc);
    spin_unlock(&target->gone_lock);

    /* Hold the awaited record for as long as someone waits on it: the list
     * lives inside it, and the drain must find a struct, not a corpse. The
     * matching ref_dec is ProcessGoneDeliver's, one per delivered waiter. */
    process_set_state(ctx->proc, PROC_WAITING);

    /* Async: guide pushes no reply now; the death delivers the real one. */
    return ERR_WOULD_BLOCK;
}

void ProcessGoneDeliver(process_t *proc, int32_t exit_code)
{
    if (!proc) return;

    /* Splice the whole list out under the lock, deliver outside it: a
     * KResultPush touches the waiter's cabin VMM, which must never happen
     * with a leaf lock held. New waiters cannot appear after this point —
     * touch_cleaned is already committed, so SysProcessGone answers them
     * directly instead of linking. */
    spin_lock(&proc->gone_lock);
    GoneWaiter *list = proc->gone_waiters;
    proc->gone_waiters = NULL;
    spin_unlock(&proc->gone_lock);

    while (list) {
        GoneWaiter *next = list->next;

        Result r;
        memset(&r, 0, sizeof(r));
        r.error_code  = OK;
        r.sender_pid  = 0;
        /* The disposition rides in data_length: >=0 is the code the process
         * passed to exit(), negative is how it was ended (proc_exit.h). It is
         * a value, not an address — nothing here maps memory into the waiter,
         * which is what lets a death answer from any core. */
        r.data_length = (uint32_t)exit_code;
        r.context     = KCTX_PACK24(KCTX_GUIDE, list->submit_cookie);
        /* The death is the event: the answer is determined here and this
         * delivery is what the kernel owes. */
        ChitDue(list->waiter, list->submit_cookie);
        KResultPush(list->waiter, &r);

        process_ref_dec(list->waiter);  /* the waiter ref taken when it parked */
        process_ref_dec(proc);          /* the awaited record, one per waiter  */
        kfree(list);
        list = next;
    }
}

/* -------------------------------------------------------------------------
 * SyncOpsRegister — called from SystemDeckRegister.
 * ------------------------------------------------------------------------- */
error_t SyncOpsRegister(void)
{
    AddrWaitTableInit();

    struct {
        uint16_t    opcode;
        OpHandler   handler;
        uint32_t    auth;
        const char *name;
    } table[] = {
        { SYSTEM_OP_ADDR_PARK,    SysAddrPark,    OP_AUTH_APP, "system.addr.park" },
        { SYSTEM_OP_ADDR_WAKE,    SysAddrWake,    OP_AUTH_APP, "system.addr.wake" },
        { SYSTEM_OP_PROCESS_GONE, SysProcessGone, OP_AUTH_APP, "system.process.gone" },
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        error_t rc = OpRegistryRegister(OP_KIND(DECK_SYSTEM, table[i].opcode),
                                        table[i].handler,
                                        table[i].auth,
                                        table[i].name);
        if (rc != OK && rc != ERR_ALREADY_EXISTS) {
            kprintf("[SyncOps] register %s failed: %s\n",
                    table[i].name, ErrorString(rc));
            return rc;
        }
    }

    debug_printf("[SyncOps] registered addr_park/addr_wake (0xC0/0xC1)\n");
    return OK;
}

/* -------------------------------------------------------------------------
 * Kernel self-tests — exercise the AddrWait bucket REGISTRY (link/unlink),
 * which is safe to check single-strand at boot. The end-to-end behavior — the
 * value pre-check, the actual park, and a concurrent wake breaking it early — is
 * covered on REAL strands by strandtest (test1 park->wake, test4 timed-wake); a
 * boot self-test cannot construct a second waker, so we do NOT fake it here.
 * (A former "test (a)" re-implemented the value pre-check inline and asserted
 * its own constants — a tautology that could never fail; removed.)
 * Tests: (a) link/unlink round-trip; (b) idempotent double-unlink.
 * ------------------------------------------------------------------------- */
void AddrWaitSelfTest(void)
{
    kprintf("[ADDR_WAIT TEST] begin\n");
    int pass = 0, fail = 0;

    /* Test (a): bucket round-trip — link/unlink under lock, check linked flag. */
    {
        volatile uint64_t dummy = 0;
        uintptr_t phys_sim = (uintptr_t)&dummy;
        AddrWaitBucket *b = AddrWaitGetBucket(0, (uint64_t)phys_sim);
        if (!b) {
            kprintf("[ADDR_WAIT TEST] FAIL (a): bucket alloc failed\n");
            fail++;
        } else {
            AddrWaitEntry e;
            e.done      = 0;
            e.linked    = 0;
            e.seq       = 0;
            e.proc      = NULL;
            e.space     = 0;
            e.user_va   = (uint64_t)phys_sim;
            e.phys_addr = phys_sim;
            e.next      = NULL;
            e.prev      = NULL;

            spin_lock(&b->lock);
            AddrWaitLink(b, &e);
            bool found   = (b->head == &e) && (e.linked == 1);
            AddrWaitUnlink(b, &e);
            bool empty   = (b->head == NULL) && (e.linked == 0);
            spin_unlock(&b->lock);

            if (found && empty) {
                kprintf("[ADDR_WAIT TEST] PASS (a): link/unlink round-trip + linked flag\n");
                pass++;
            } else {
                kprintf("[ADDR_WAIT TEST] FAIL (a): link/unlink broken (found=%d empty=%d)\n",
                        (int)found, (int)empty);
                fail++;
            }
        }
    }

    /* Test (b): AddrWaitUnlinkIfLinked — link via the locked path, then call
     * AddrWaitUnlinkIfLinked once (should unlink) and a second time (no-op).
     * Verifies: double-unlink is safe and linked toggles correctly. */
    {
        volatile uint64_t dummy2 = 0;
        uintptr_t phys_sim2 = (uintptr_t)&dummy2 + 8; /* distinct address from (b) */
        AddrWaitBucket *b2 = AddrWaitGetBucket(0, (uint64_t)phys_sim2);
        if (!b2) {
            kprintf("[ADDR_WAIT TEST] FAIL (b): bucket alloc failed\n");
            fail++;
        } else {
            AddrWaitEntry e2;
            e2.done      = 0;
            e2.linked    = 0;
            e2.seq       = 0;
            e2.proc      = NULL;
            e2.space     = 0;
            e2.user_va   = (uint64_t)phys_sim2;
            e2.phys_addr = phys_sim2;
            e2.next      = NULL;
            e2.prev      = NULL;

            spin_lock(&b2->lock);
            AddrWaitLink(b2, &e2);
            spin_unlock(&b2->lock);

            /* First call — should unlink. */
            AddrWaitUnlinkIfLinked(&e2);
            bool unlinked_once = (e2.linked == 0) && (b2->head != &e2);

            /* Second call — no-op, must not crash. */
            AddrWaitUnlinkIfLinked(&e2);
            bool still_unlinked = (e2.linked == 0);

            if (unlinked_once && still_unlinked) {
                kprintf("[ADDR_WAIT TEST] PASS (b): AddrWaitUnlinkIfLinked double-call safe\n");
                pass++;
            } else {
                kprintf("[ADDR_WAIT TEST] FAIL (b): AddrWaitUnlinkIfLinked broken "
                        "(unlinked_once=%d still_unlinked=%d)\n",
                        (int)unlinked_once, (int)still_unlinked);
                fail++;
            }
        }
    }

    kprintf("[ADDR_WAIT TEST] done: %d pass, %d fail\n", pass, fail);
}
