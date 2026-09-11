
#include "system_deck.h"
#include "chit.h"
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
#include "kring.h"
#include "result.h"
#include "boxos_kctx.h"
#include "error.h"
#include "klib.h"
#include "atomics.h"
#include "amp.h"
#include "pid_allocator.h"
#include "lapic.h"
#include "irqchip.h"
#include "scheduler.h"
#include "boxos_decks.h"

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

    uintptr_t phys = 0;
    if (ctx->proc->cabin) {
        phys = vmm_virt_to_phys(ctx->proc->cabin->vmm, user_va);
    }
    if (phys == 0) return ERR_INVALID_ADDRESS;

    volatile uint64_t *kaddr = (volatile uint64_t *)vmm_phys_to_virt(phys);
    uint64_t actual = __atomic_load_n(kaddr, __ATOMIC_ACQUIRE);
    if (actual != expected) return ERR_ADDR_VALUE_MISMATCH;

    AddrWaitEntry *entry = &ctx->proc->addr_wait_entry;
    AddrWaitUnlinkIfLinked(entry);

    ChitGive(ctx, "system.addr.park", (uint64_t)phys);

    uintptr_t space = (uintptr_t)ctx->proc->cabin->vmm;
    AddrWaitBucket *bucket = AddrWaitGetBucket(space, user_va);
    if (!bucket) return ERR_NO_MEMORY;

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
    entry->user_va   = user_va;
    entry->expected  = expected;
    entry->timed     = (timeout_ms > 0) ? 1u : 0u;
    entry->fire_at   = fire_at;
    AddrWaitLink(bucket, entry);
    spin_unlock(&bucket->lock);

    uint32_t park_seq = __atomic_add_fetch(&ctx->proc->park_seq, 1,
                                           __ATOMIC_ACQ_REL);
    if (timeout_ms > 0)
        TouchQueueWakeAfter(ctx->proc->pid, fire_at, 1u, park_seq);

    process_set_state(ctx->proc, PROC_WAITING);

    uint64_t recheck = __atomic_load_n(kaddr, __ATOMIC_ACQUIRE);
    bool completed   = (__atomic_load_n(&entry->done, __ATOMIC_ACQUIRE) != 0);

    if (!completed && recheck != expected) {
        if (AddrWaitClaim(entry)) {
            process_set_state(ctx->proc, PROC_WORKING);
            return ERR_ADDR_VALUE_MISMATCH;
        }
        completed = true;
    }

    if (completed) {
        process_set_state(ctx->proc, PROC_WORKING);
        return ERR_WOULD_BLOCK;
    }

    {
        struct __attribute__((packed)) { uint64_t phys; uint32_t pid; } ev;
        ev.phys = (uint64_t)phys;
        ev.pid  = ctx->proc->pid;
        TouchPublish("strand:parked", &ev, sizeof(ev));
    }

    return ERR_WOULD_BLOCK;
}

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

    if (!ctx->proc->cabin || vmm_virt_to_phys(ctx->proc->cabin->vmm, user_va) == 0)
        return ERR_INVALID_ADDRESS;
    uintptr_t space = (uintptr_t)ctx->proc->cabin->vmm;

    AddrWaitBucket *bucket = AddrWaitGetBucket(space, user_va);
    if (!bucket) return OK;

    AddrWaitEntry  *claimed      = NULL;
    AddrWaitEntry **claimed_tail = &claimed;
    uint32_t        wake_count   = 0;

    spin_lock(&bucket->lock);
    AddrWaitEntry *e = bucket->head;
    while (e && (count == 0 || wake_count < count))
    {
        AddrWaitEntry *next = e->next;
        if (e->space == space && e->user_va == user_va &&
            AddrWaitClaimLocked(bucket, e)) {
            process_ref_inc(e->proc);
            e->claimed_next = NULL;
            *claimed_tail   = e;
            claimed_tail    = &e->claimed_next;
            wake_count++;
        }
        e = next;
    }
    spin_unlock(&bucket->lock);

    uint32_t waker_pid = ctx->proc->pid;
    while (claimed) {
        AddrWaitEntry *next   = claimed->claimed_next;
        process_t     *target = claimed->proc;
        uint32_t       cookie = claimed->submit_cookie;
        claimed = next;

        if (!target->destroying) {
            Result r;
            memset(&r, 0, sizeof(r));
            r.error_code = OK;
            r.sender_pid = 0;
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

void SyncTimeoutDeliver(void *ctx)
{
    process_t *target = (process_t *)ctx;

    __atomic_store_n(&target->deadline_passed, 0u, __ATOMIC_SEQ_CST);

    uint64_t now    = __atomic_load_n(&g_global_tick, __ATOMIC_RELAXED);
    uint32_t cookie = 0;
    if (!target->destroying &&
        AddrWaitClaimDue(&target->addr_wait_entry, now, &cookie)) {
        Result r;
        memset(&r, 0, sizeof(r));
        r.error_code = ERR_TIMEOUT;
        r.sender_pid = 0;
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

    process_ref_dec(target);
}

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
        return ERR_INVALID_ARGUMENT;

    process_t *target = process_find_ref(pid);
    if (!target || target->generation != want_gen) {
        if (target) process_ref_dec(target);
        switch (pid_life(pid, want_gen)) {
        case PID_LIFE_DEPARTED: return OK;
        case PID_LIFE_LIVE:
            return ERR_BUSY;
        default:                return ERR_PROCESS_NOT_FOUND;
        }
    }

    GoneWaiter *w = (GoneWaiter *)kmalloc(sizeof(GoneWaiter));
    if (!w) { process_ref_dec(target); return ERR_NO_MEMORY; }
    w->waiter          = ctx->proc;
    w->want_generation = want_gen;
    w->submit_cookie   = ctx->submit_cookie;

    ChitGive(ctx, "system.process.gone", ((uint64_t)want_gen << 32) | pid);

    spin_lock(&target->gone_lock);
    if (__atomic_load_n(&target->touch_cleaned, __ATOMIC_ACQUIRE)) {
        spin_unlock(&target->gone_lock);
        kfree(w);
        process_ref_dec(target);
        return OK;
    }
    w->next              = target->gone_waiters;
    target->gone_waiters = w;
    process_ref_inc(ctx->proc);
    spin_unlock(&target->gone_lock);

    process_set_state(ctx->proc, PROC_WAITING);

    return ERR_WOULD_BLOCK;
}

void ProcessGoneDeliver(process_t *proc, int32_t exit_code)
{
    if (!proc) return;

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
        r.data_length = (uint32_t)exit_code;
        r.context     = KCTX_PACK24(KCTX_GUIDE, list->submit_cookie);
        ChitDue(list->waiter, list->submit_cookie);
        KResultPush(list->waiter, &r);

        process_ref_dec(list->waiter);
        process_ref_dec(proc);
        kfree(list);
        list = next;
    }
}

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

void AddrWaitSelfTest(void)
{
    kprintf("[ADDR_WAIT TEST] begin\n");
    int pass = 0, fail = 0;

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

    {
        volatile uint64_t dummy2 = 0;
        uintptr_t phys_sim2 = (uintptr_t)&dummy2 + 8;
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

            AddrWaitUnlinkIfLinked(&e2);
            bool unlinked_once = (e2.linked == 0) && (b2->head != &e2);

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