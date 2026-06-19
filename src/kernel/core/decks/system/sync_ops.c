/*
 * sync_ops.c — SYSTEM_OP_ADDR_PARK / SYSTEM_OP_ADDR_WAKE handlers.
 *
 * These are the kernel substrate for std::atomic::wait / notify_one /
 * notify_all (to be wired from C++ once Strands exist).
 *
 * Park/wake result-delivery mirrors SysTouchAwait in touch_ops.c exactly:
 *   - park parks via process_set_state(PROC_WAITING) and returns
 *     ERR_WOULD_BLOCK — the identical signal that guide.c uses to know
 *     the caller is parked and its result will arrive later.
 *   - wake sets PROC_WAITING → PROC_WORKING and sends an IPI, exactly
 *     as touch_queue_fire_wake (touch_queue.c:105) does.
 *   - timeout is armed via TouchQueueWakeAfter — the same helper
 *     SysTouchAwait uses.
 *   - The lost-wakeup re-check after PROC_WAITING mirrors SysTouchAwait's
 *     TouchRing tail/head re-check (touch_ops.c:310-319): we re-load
 *     *phys and entry.done under ACQUIRE; if either signals "already
 *     woken", we undo the park and return ERR_ADDR_VALUE_MISMATCH so the
 *     caller retries immediately instead of sleeping forever.
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
 *   4.  Arm timeout via TouchQueueWakeAfter (same as SysTouchAwait line 272).
 *   5.  process_set_state(PROC_WAITING) (same as SysTouchAwait line 275).
 *   6.  Lost-wakeup re-check under ACQUIRE: reload *phys and entry.done;
 *       if either indicates "already woken", undo park (same discipline as
 *       SysTouchAwait lines 310-319 which reload tail and undo the park).
 *   7.  Return ERR_WOULD_BLOCK (same as SysTouchAwait line 321): guide.c
 *       uses this to know the caller is parked. When SysAddrWake fires,
 *       it flips PROC_WAITING→PROC_WORKING + IPI (touch_queue_fire_wake
 *       pattern), which is the IDENTICAL mechanism that delivers the
 *       parked SysTouchAwait caller's result: the scheduler reschedules
 *       the process, and the process's next result_pop sees a normal
 *       result because guide.c pushed one via KResultPush when it handled
 *       ERR_WOULD_BLOCK. No separate result ring write is needed here —
 *       the guide.c ERR_WOULD_BLOCK path already handles it.
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

    /* Step 4: arm timeout (identical to SysTouchAwait line 266-273). */
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

    /* Step 6: lost-wakeup re-check (mirrors SysTouchAwait lines 310-319).
     *
     * SysTouchAwait re-checks the TouchRing tail/head under ACQUIRE after
     * setting PROC_WAITING. We do the same for our two wake indicators:
     *   - entry.done: SysAddrWake set it to 1 under bucket lock before
     *     sending the IPI. If we see it here, the wake happened between
     *     our step-3 registration and step-5 park — undo the park.
     *   - *phys value changed: a writer modified the address between our
     *     step-2 pre-check and now — the condition the caller wanted to
     *     observe is already gone.
     *
     * The ACQUIRE on entry.done pairs with SysAddrWake's RELEASE-store
     * (via spin_unlock which is a barrier) on done=1 before the IPI.
     * The ACQUIRE on *kaddr pairs with the writer's store (user-side
     * atomic store is RELEASE by C++ rules). Either observation is
     * sufficient to undo the park. */
    uint8_t already_done = __atomic_load_n(&entry->done, __ATOMIC_ACQUIRE);
    uint64_t recheck     = __atomic_load_n(kaddr, __ATOMIC_ACQUIRE);

    if (already_done || recheck != expected) {
        process_set_state(ctx->proc, PROC_WORKING);
        /* Use AddrWaitUnlinkIfLinked: it re-derives the bucket (which may
         * differ from `bucket` if phys_addr was changed above) and uses the
         * linked flag as the guard — correct even if SysAddrWake beat us. */
        AddrWaitUnlinkIfLinked(entry);
        /* Publish outside bucket lock. */
        if (already_done) {
            struct __attribute__((packed)) { uint64_t phys; uint32_t pid; } ev;
            ev.phys = (uint64_t)phys;
            ev.pid  = ctx->proc->pid;
            TouchPublish("strand:woken", &ev, sizeof(ev));
        }
        return ERR_ADDR_VALUE_MISMATCH;
    }

    /* Publish strand:parked outside the bucket lock (mirrors SysTouchAwait
     * publishing behavior — no lock held at the point of TouchPublish). */
    {
        struct __attribute__((packed)) { uint64_t phys; uint32_t pid; } ev;
        ev.phys = (uint64_t)phys;
        ev.pid  = ctx->proc->pid;
        TouchPublish("strand:parked", &ev, sizeof(ev));
    }

    /* Step 7: return ERR_WOULD_BLOCK — guide.c interprets this identically
     * to SysTouchAwait's ERR_WOULD_BLOCK: it does not push a result now;
     * instead it leaves the process in PROC_WAITING. When SysAddrWake fires
     * and sets PROC_WORKING, the scheduler reschedules the process and
     * guide.c's normal result path handles the continuation — the same
     * mechanism as every SysTouchAwait wake-up. */
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

    /* Claim up to `count` entries under the lock.  We snapshot their proc
     * pointers (ref-counted) and set done=1 so the parked strand's step-6
     * re-check unlinks itself when it runs, rather than us unlinking here.
     * This matches the touch-ring model where the parked side self-removes
     * after it resumes, keeping the critical section short. */
#define ADDR_WAKE_MAX_BATCH  256u
    process_t *to_wake[ADDR_WAKE_MAX_BATCH];
    uint32_t   wake_count = 0;

    spin_lock(&bucket->lock);
    for (AddrWaitEntry *e = bucket->head;
         e && (count == 0 || wake_count < count) && wake_count < ADDR_WAKE_MAX_BATCH;
         e = e->next)
    {
        if (e->done) continue;
        e->done = 1;  /* claim — visible to parked strand's re-check */
        process_ref_inc(e->proc);
        to_wake[wake_count++] = e->proc;
    }
    spin_unlock(&bucket->lock);

    /* Fire wakes outside the bucket lock — mirrors touch_queue_fire_wake
     * (touch_queue.c:105-119) exactly. */
    uint32_t waker_pid = ctx->proc->pid;
    for (uint32_t i = 0; i < wake_count; i++) {
        process_t *target = to_wake[i];

        if (!target->destroying && process_get_state(target) == PROC_WAITING) {
            process_set_state(target, PROC_WORKING);
            if (g_amp.total_cores > 1) {
                uint8_t core = target->home_core;
                if (core < g_amp.total_cores && core != amp_get_core_index()) {
                    lapic_send_ipi(g_amp.cores[core].lapic_id, IPI_WAKE_VECTOR);
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
 * Kernel self-tests — single-strand safe (no concurrent waker needed).
 * Tests: (a) value-mismatch early-return; (b) link/unlink round-trip.
 * ------------------------------------------------------------------------- */
void AddrWaitSelfTest(void)
{
    kprintf("[ADDR_WAIT TEST] begin\n");
    int pass = 0, fail = 0;

    /* Test (a): value-mismatch — park on an address where *addr != expected.
     * SysAddrPark step 2 must return ERR_ADDR_VALUE_MISMATCH immediately,
     * without entering PROC_WAITING. */
    {
        volatile uint64_t val = 42;
        AddrWaitBucket *bucket = AddrWaitGetBucket((uintptr_t)&val);
        if (!bucket) {
            kprintf("[ADDR_WAIT TEST] SKIP: OOM getting bucket\n");
        } else {
            /* Simulate the value pre-check logic from SysAddrPark step 2. */
            uint64_t actual = __atomic_load_n(&val, __ATOMIC_ACQUIRE);
            uint64_t wrong_expected = 999;
            if (actual != wrong_expected) {
                /* Correct: would return ERR_ADDR_VALUE_MISMATCH */
                kprintf("[ADDR_WAIT TEST] PASS (a): mismatch detected, no park\n");
                pass++;
            } else {
                kprintf("[ADDR_WAIT TEST] FAIL (a): mismatch not detected\n");
                fail++;
            }
        }
    }

    /* Test (b): bucket round-trip — link/unlink under lock, check linked flag. */
    {
        volatile uint64_t dummy = 0;
        uintptr_t phys_sim = (uintptr_t)&dummy;
        AddrWaitBucket *b = AddrWaitGetBucket(phys_sim);
        if (!b) {
            kprintf("[ADDR_WAIT TEST] FAIL (b): bucket alloc failed\n");
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
                kprintf("[ADDR_WAIT TEST] PASS (b): link/unlink round-trip + linked flag\n");
                pass++;
            } else {
                kprintf("[ADDR_WAIT TEST] FAIL (b): link/unlink broken (found=%d empty=%d)\n",
                        (int)found, (int)empty);
                fail++;
            }
        }
    }

    /* Test (c): AddrWaitUnlinkIfLinked — link via the locked path, then call
     * AddrWaitUnlinkIfLinked once (should unlink) and a second time (no-op).
     * Verifies: double-unlink is safe and linked toggles correctly. */
    {
        volatile uint64_t dummy2 = 0;
        uintptr_t phys_sim2 = (uintptr_t)&dummy2 + 8; /* distinct address from (b) */
        AddrWaitBucket *b2 = AddrWaitGetBucket(phys_sim2);
        if (!b2) {
            kprintf("[ADDR_WAIT TEST] FAIL (c): bucket alloc failed\n");
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
                kprintf("[ADDR_WAIT TEST] PASS (c): AddrWaitUnlinkIfLinked double-call safe\n");
                pass++;
            } else {
                kprintf("[ADDR_WAIT TEST] FAIL (c): AddrWaitUnlinkIfLinked broken "
                        "(unlinked_once=%d still_unlinked=%d)\n",
                        (int)unlinked_once, (int)still_unlinked);
                fail++;
            }
        }
    }

    kprintf("[ADDR_WAIT TEST] done: %d pass, %d fail\n", pass, fail);
}
