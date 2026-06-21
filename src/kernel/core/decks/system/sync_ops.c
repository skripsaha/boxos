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
 *   - TIMEOUT: armed via TouchQueueWakeAfter — a light, IRQ-safe reschedule
 *     (PROC_WORKING + IPI; NO ResultRing/VMM work in the PIT tick). The
 *     rescheduled caller's boxlib result_wait then ends via its own call_timeout
 *     (ERR_TIMEOUT). Delivering the timeout Result from the timer IRQ was tried
 *     and REVERTED: doing VMM work in interrupt context starved cores from
 *     ACKing TLB shootdowns under 16-core load (shootdown-timeout panic).
 *
 * Lock ordering (never violated here):
 *   vmm translate (holds ctx->lock) completes BEFORE bucket lock is taken.
 *   TouchPublish happens OUTSIDE the bucket lock.
 *   No scheduler_lock / process_lock is held across the bucket lock.
 */

#include "system_deck.h"
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

    AddrWaitBucket *bucket = AddrWaitGetBucket(phys);
    if (!bucket) return ERR_NO_MEMORY;

    /* Arm the entry UNDER the bucket lock — the lock that guards the chain
     * it is about to join — so a concurrent SysAddrWake walking this bucket
     * never observes a half-armed entry.  (Between the unlink above and this
     * link the entry is in no chain, so wakers cannot reach it.)  Previously
     * these stores sat outside the lock, correct only by statement order. */
    spin_lock(&bucket->lock);
    entry->proc      = ctx->proc;
    entry->phys_addr = phys;
    entry->done      = 0;
    AddrWaitLink(bucket, entry);
    spin_unlock(&bucket->lock);

    /* Step 4: arm an IRQ-safe timeout reschedule (same helper SysTouchAwait
     * uses). On expiry the PIT tick only flips PROC_WORKING + IPI — it does NO
     * ResultRing/VMM work in interrupt context. The rescheduled caller's boxlib
     * result_wait then ends via its own call_timeout (timeout_ms + margin) with
     * ERR_TIMEOUT. Early wake is the event-driven path: addr_wake KResultPushes
     * from the K-Core (see SysAddrWake). */
    if (timeout_ms > 0) {
        uint64_t delay = ((uint64_t)timeout_ms * SCHEDULER_DEFAULT_TICK_HZ)
                         / 1000ULL;
        if (delay == 0) delay = 1;
        uint64_t fire_at = __atomic_load_n(&g_global_tick, __ATOMIC_RELAXED)
                           + delay;
        TouchQueueWakeAfter(ctx->proc->pid, fire_at);
    }

    /* Step 5: park (identical to SysTouchAwait line 275). */
    process_set_state(ctx->proc, PROC_WAITING);

    /* Step 6: lost-wakeup recheck — async-completion arbitration.
     *
     * addr_park is an ASYNC op: the caller blocks in boxlib result_wait() on
     * its ResultRing, and the COMPLETION is a real Result pushed by whoever
     * claims this entry (AddrWaitClaim: done 0->1 + unlink, atomically). The
     * claim is the single arbitration point for the WAKE Result — exactly one of
     * {addr_wake, this parker} claims+delivers it. (The timeout does NOT claim; it
     * only reschedules, and result_wait's call_timeout then yields ERR_TIMEOUT.)
     * Here the parker races a concurrent addr_wake after setting PROC_WAITING:
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
            /* (a) We own delivery — finish synchronously through guide. Our armed
             * timeout is harmless if it later fires (PROC_WORKING reschedule of an
             * already-running strand; at worst a benign spurious wake if re-parked). */
            process_set_state(ctx->proc, PROC_WORKING);
            return ERR_ADDR_VALUE_MISMATCH;
        }
        completed = true;   /* lost the claim — a waker owns delivery */
    }

    if (completed) {
        /* (b) A concurrent addr_wake claimed us and will KResultPush the
         * completion. Reschedule to WORKING and wait async for it to arrive. */
        process_set_state(ctx->proc, PROC_WORKING);
        if (ctx->async_owns_crates) *ctx->async_owns_crates = true;
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
     * now (async_owns_crates set); the parked caller's result_wait blocks until
     * addr_wake / the park-timeout KResultPushes the real completion. */
    if (ctx->async_owns_crates) *ctx->async_owns_crates = true;
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

    uintptr_t phys = 0;
    if (ctx->proc->cabin) {
        phys = vmm_virt_to_phys(ctx->proc->cabin->vmm, user_va);
    }
    if (phys == 0) return ERR_INVALID_ADDRESS;

    /* Look up bucket.  If no slab exists for this address yet, there are no
     * waiters — return OK silently (no waiters is not an error). */
    AddrWaitBucket *bucket = AddrWaitGetBucket(phys);
    if (!bucket) return OK;

    /* Claim up to `count` entries under the lock. Claiming a waiter means
     * setting done=1 AND unlinking it atomically (here, under this lock) — the
     * claimer then OWNS that waiter's single completion Result. Unlinking on
     * claim (rather than leaving the parked side to self-remove) is what makes
     * a later claimer — a stale park-timeout, or a re-park — unable to match an
     * already-completed waiter. We snapshot ref-counted proc pointers and
     * deliver outside the lock to keep the critical section short. */
#define ADDR_WAKE_MAX_BATCH  256u
    process_t *to_wake[ADDR_WAKE_MAX_BATCH];
    uint32_t   wake_count = 0;

    spin_lock(&bucket->lock);
    AddrWaitEntry *e = bucket->head;
    while (e && (count == 0 || wake_count < count) && wake_count < ADDR_WAKE_MAX_BATCH)
    {
        AddrWaitEntry *next = e->next;   /* save: AddrWaitUnlink nulls e->next */
        if (!e->done) {
            e->done = 1;                 /* claim — we now own this waiter */
            AddrWaitUnlink(bucket, e);   /* unlink under the same lock */
            process_ref_inc(e->proc);
            to_wake[wake_count++] = e->proc;
        }
        e = next;
    }
    spin_unlock(&bucket->lock);

    /* Fire wakes outside the bucket lock — mirrors touch_queue_fire_wake
     * (touch_queue.c:105-119) exactly. */
    uint32_t waker_pid = ctx->proc->pid;
    for (uint32_t i = 0; i < wake_count; i++) {
        process_t *target = to_wake[i];

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
            r.context    = KCTX_GUIDE;
            KResultPush(target, &r);

            /* The target's armed timeout (TouchQueueWakeAfter) is left in place;
             * if it fires later it only does a PROC_WORKING reschedule (no Result,
             * no entry touch) — at worst a benign spurious wakeup for a re-parked
             * strand, which re-checks its predicate. No IRQ-context cleanup. */
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
                uint64_t phys;
                uint32_t pid;
                uint32_t waker_pid;
            } ev;
            ev.phys      = (uint64_t)phys;
            ev.pid       = target->pid;
            ev.waker_pid = waker_pid;
            TouchPublish("strand:woken", &ev, sizeof(ev));
        }

        process_ref_dec(target);
    }

    return OK;
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
        { SYSTEM_OP_ADDR_PARK, SysAddrPark, OP_AUTH_APP, "system.addr.park" },
        { SYSTEM_OP_ADDR_WAKE, SysAddrWake, OP_AUTH_APP, "system.addr.wake" },
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
        AddrWaitBucket *b = AddrWaitGetBucket(phys_sim);
        if (!b) {
            kprintf("[ADDR_WAIT TEST] FAIL (a): bucket alloc failed\n");
            fail++;
        } else {
            AddrWaitEntry e;
            e.done      = 0;
            e.linked    = 0;
            e.proc      = NULL;
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
        AddrWaitBucket *b2 = AddrWaitGetBucket(phys_sim2);
        if (!b2) {
            kprintf("[ADDR_WAIT TEST] FAIL (b): bucket alloc failed\n");
            fail++;
        } else {
            AddrWaitEntry e2;
            e2.done      = 0;
            e2.linked    = 0;
            e2.proc      = NULL;
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
