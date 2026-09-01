/*
 * Nightwatch — implementation. See nightwatch.h for what it is and, more
 * importantly, for what it deliberately is not.
 */

#include "nightwatch.h"
#include "process.h"
#include "addr_wait.h"
#include "cabin.h"
#include "amp.h"
#include "vmm.h"
#include "scheduler.h"
#include "kcore.h"
#include "result_ring.h"
#include "touch_ring.h"
#include "clockboard.h"
#include "klib.h"

/*
 * How often Nightwatch is allowed to look, and how long total quiet must last
 * before it will describe a system that is merely stopped.
 *
 * Neither is a failure threshold — the three proofs stand on their own and are
 * true at the instant they are read. LOOK_MS is a rate limit so the walk costs
 * nothing on a working system; QUIET_MS is what separates "stopped" from a
 * legitimate all-idle transient (a wake IPI in flight, a shootdown draining),
 * which lasts microseconds.
 *
 * Looking only when EVERY core is idle was too narrow, and a real failure
 * proved it: a stall whose victim waits forever while unrelated processes spin
 * never reaches full quiet, so nothing was ever examined. Any idle core is
 * enough of an invitation — the pass is silent unless it can prove something.
 *
 * Time comes from the ClockBoard — the kernel's own published clock, which the
 * PIT IRQ advances on the BSP even while every core sleeps in HLT. Reading it
 * is a plain aligned load, so it costs nothing and needs no lock in a path
 * where taking one would be its own hazard. Using the ClockBoard rather than a
 * raw scheduler tick also means this threshold is honest milliseconds and does
 * not silently change meaning if the tick rate is ever retuned.
 */
#define NIGHTWATCH_QUIET_MS 10000u
#define NIGHTWATCH_LOOK_MS  10000u

/* Per-core idle marks. A plain byte per core rather than a bitmask: MAX_CORES
 * is 256, so no single word covers it, and a byte store needs no atomic. */
static volatile uint8_t  g_core_idle[MAX_CORES];
static volatile uint32_t g_idle_cores;
static volatile uint64_t g_quiet_since_ms;   /* uptime when the LAST core went idle */
static volatile uint8_t  g_reported;      /* one report per stall episode */
static volatile uint8_t  g_armed;         /* init done */
/* How many cores must be idle for the system to be idle. Snapshotted at init,
 * AFTER the APs have booted: g_amp.total_cores counts cores the firmware
 * described, and one that never answered INIT-SIPI would never mark itself
 * idle — leaving the count permanently short and Nightwatch permanently
 * silent, which is the one failure mode a detector must not have. */
static uint32_t g_watch_cores = 1;
static volatile uint64_t g_last_look_ms;   /* rate limit for the proof pass */

/* One process, copied out from under process_lock so every check and every
 * print below runs with no lock held. */
typedef struct
{
    uint32_t  pid;
    uint32_t  generation;
    uint8_t   state;
    uint8_t   home_core;
    int16_t   on_cpu;
    uint8_t   linked;
    uint8_t   done;
    uint8_t   timed;
    uint32_t  seq;
    uint64_t  user_va;
    uint64_t  expected;
    uintptr_t phys_addr;
    uint8_t   wait_reason;
    uint64_t  result_ring_phys;
    uint64_t  touch_ring_phys;
} NightwatchProbe;

void nightwatch_init(void)
{
    for (uint32_t i = 0; i < MAX_CORES; i++)
        g_core_idle[i] = 0;
    g_idle_cores     = 0;
    g_quiet_since_ms = 0;
    g_reported       = 0;

    uint32_t online = 0;
    for (uint8_t c = 0; c < g_amp.total_cores; c++)
        if (amp_core_online(&g_amp.cores[c])) online++;
    g_watch_cores = online ? online : 1u;

    __atomic_store_n(&g_armed, 1, __ATOMIC_RELEASE);
    kprintf("[NIGHTWATCH] armed — %u core(s) watched, stall described after %u ms idle\n",
            g_watch_cores, (unsigned)NIGHTWATCH_QUIET_MS);
}

static const char *nightwatch_state_name(uint8_t s)
{
    switch ((process_state_t)s)
    {
        case PROC_CREATED: return "created";
        case PROC_WORKING: return "working";
        case PROC_WAITING: return "waiting";
        case PROC_STOPPED: return "stopped";
        case PROC_DONE:    return "done";
        case PROC_CRASHED: return "crashed";
        default:           return "?";
    }
}

/* Read the watched word through the Pull Map. Pure arithmetic — no VMM lock,
 * no page walk — so this is safe to do anywhere, including a stalled system
 * where taking another lock would be its own risk. */
static uint64_t nightwatch_peek(uintptr_t phys)
{
    volatile uint64_t *p = (volatile uint64_t *)vmm_phys_to_virt(phys);
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

/*
 * The verdict pass. Runs with no locks held except where noted, and prints
 * nothing while holding process_lock — kprintf takes the console lock, and
 * that ordering is not one this kernel establishes anywhere else.
 */
static void nightwatch_verdict(bool all_quiet)
{
    /* Size the snapshot before taking the lock: allocating under process_lock
     * would nest the heap lock inside it for no reason. A few spare slots
     * absorb processes created between the count and the walk; any that still
     * do not fit are named in the summary rather than silently dropped. */
    uint32_t  slots = process_get_count() + 8u;
    NightwatchProbe *probe = kmalloc(slots * sizeof(NightwatchProbe));
    if (!probe)
    {
        kprintf("[NIGHTWATCH] STALL: every core idle, and no memory to describe it\n");
        return;
    }

    uint32_t n = 0, skipped = 0;

    process_list_lock();
    for (process_t *p = process_get_first(); p; p = p->next)
    {
        if (n >= slots) { skipped++; continue; }
        NightwatchProbe *e = &probe[n++];
        e->pid        = p->pid;
        e->generation = p->generation;
        e->state      = (uint8_t)p->state;
        e->home_core  = p->home_core;
        e->on_cpu     = __atomic_load_n(&p->on_cpu, __ATOMIC_RELAXED);
        e->linked     = p->addr_wait_entry.linked;
        e->done       = p->addr_wait_entry.done;
        e->timed      = p->addr_wait_entry.timed;
        e->seq        = p->addr_wait_entry.seq;
        e->user_va    = p->addr_wait_entry.user_va;
        e->expected   = p->addr_wait_entry.expected;
        e->phys_addr  = p->addr_wait_entry.phys_addr;
        e->wait_reason      = (uint8_t)p->wait_reason;
        e->result_ring_phys = p->result_ring_phys;
        e->touch_ring_phys  = p->touch_ring_phys;
    }
    process_list_unlock();

    uint32_t parked = 0, lost = 0, unreachable = 0, timed = 0, undelivered = 0;

    /* Two passes: judge in silence, then speak only with evidence. A pass that
     * finds nothing must cost nothing on the console — that is what lets this
     * run whenever any core is idle instead of only when all of them are. */
    for (uint32_t pass = 0; pass < 2; pass++)
    {
    bool speak = (pass == 1);
    parked = lost = unreachable = timed = undelivered = 0;
    for (uint32_t i = 0; i < n; i++)
    {
        NightwatchProbe *e = &probe[i];

        /* PENDING RESULT. Not every block is an addr-park: a process can be
         * waiting for a Result instead. If its ring already holds one — tail
         * moved past head — the answer it blocked for HAS been delivered and
         * it was never made runnable. With every core idle for the quiet
         * period nothing is going to schedule it either, so this is the same
         * kind of proof as LOST WAKE, for the other way of waiting. */
        bool result_ready = false;
        uint64_t rr_head = 0, rr_tail = 0;
        if (e->state == (uint8_t)PROC_WAITING && e->result_ring_phys)
        {
            const ResultRingHeader *h =
                (const ResultRingHeader *)vmm_phys_to_virt(e->result_ring_phys);
            rr_head = __atomic_load_n(&h->head, __ATOMIC_ACQUIRE);
            rr_tail = __atomic_load_n(&h->tail, __ATOMIC_ACQUIRE);
            result_ready = (rr_tail != rr_head);
        }
        /* Same proof for the other delivery surface. A supervisor blocked on
         * process:died waits on its Touch ring, not on a Result — and a death
         * sitting unread in that ring while its reader stays asleep is the same
         * defect wearing different clothes. */
        bool touch_ready = false;
        uint64_t tr_head = 0, tr_tail = 0;
        if (e->state == (uint8_t)PROC_WAITING && e->touch_ring_phys)
        {
            const TouchRingHeader *t =
                (const TouchRingHeader *)vmm_phys_to_virt(e->touch_ring_phys);
            tr_head = __atomic_load_n(&t->head, __ATOMIC_ACQUIRE);
            tr_tail = __atomic_load_n(&t->tail, __ATOMIC_ACQUIRE);
            touch_ready = (tr_tail != tr_head);
        }
        if (touch_ready) undelivered++;
        if (result_ready) undelivered++;

        if (!e->linked || e->done)
        {
            if (speak)
            kprintf("  pid %u gen %u %s home=%u on_cpu=%d wait=%u — not parked\n",
                    e->pid, e->generation, nightwatch_state_name(e->state),
                    e->home_core, (int)e->on_cpu, (unsigned)e->wait_reason);
            if (result_ready && speak)
            {
                kprintf("      ‼ RESULT UNDELIVERED — ring holds head=%lu tail=%lu, "
                        "yet this process still waits\n",
                        (unsigned long)rr_head, (unsigned long)rr_tail);
                /* Name the record itself. head/tail prove SOMETHING lies
                 * unread; whose reply it is decides where to look next, and
                 * evidence that stops one question short of the answer sends
                 * the reader off to guess it. Best-effort: the slot lives at
                 * a cabin VA, so the walk can fail mid-teardown — then the
                 * line simply does not print. */
                process_t *owner = process_find_ref(e->pid);
                if (owner && owner->cabin && e->result_ring_phys)
                {
                    const ResultRing *rr =
                        (const ResultRing *)vmm_phys_to_virt(e->result_ring_phys);
                    uintptr_t slot_uva = result_ring_slot_uvaddr(rr, rr_head);
                    const ResultSlot *slot = (const ResultSlot *)
                        vmm_translate_user_addr(owner->cabin->vmm, slot_uva,
                                                sizeof(ResultSlot));
                    if (slot)
                        kprintf("        head slot: seq=%lu ctx=%u sender=%u "
                                "err=%u len=%u addr=0x%lx\n",
                                (unsigned long)__atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE),
                                (unsigned)slot->r.context,
                                (unsigned)slot->r.sender_pid,
                                (unsigned)slot->r.error_code,
                                (unsigned)slot->r.data_length,
                                (unsigned long)slot->r.data_addr);
                }
                if (owner) process_ref_dec(owner);
            }
            if (touch_ready && speak)
                kprintf("      ‼ TOUCH UNDELIVERED — ring holds head=%lu tail=%lu, "
                        "yet this process still waits\n",
                        (unsigned long)tr_head, (unsigned long)tr_tail);
            continue;
        }

        parked++;
        uint64_t actual = nightwatch_peek(e->phys_addr);

        /* LOST WAKE. The value it parked on has changed, nothing is going to
         * time this park out, and the entry is still in its bucket: the wake
         * was owed and never delivered. Proven, not inferred. */
        bool lost_wake = (actual != e->expected) && !e->timed;
        if (lost_wake) lost++;
        if (e->timed)  timed++;

        /* UNREACHABLE. Re-resolve the VA the parker gave us. A different
         * physical page means the wait table has this entry filed under an
         * address no waker will ever compute. */
        bool moved = false;
        uintptr_t phys_now = 0;
        process_t *ref = process_find_ref(e->pid);
        if (ref)
        {
            if (ref->cabin && ref->cabin->vmm && e->user_va)
            {
                phys_now = vmm_virt_to_phys(ref->cabin->vmm, e->user_va);
                moved = (phys_now != 0) && (phys_now != e->phys_addr);
            }
            process_ref_dec(ref);
        }
        if (moved) unreachable++;

        if (speak) {
        kprintf("  pid %u gen %u %s home=%u on_cpu=%d parked seq=%u%s\n",
                e->pid, e->generation, nightwatch_state_name(e->state),
                e->home_core, (int)e->on_cpu, e->seq,
                e->timed ? " (deadline armed)" : " (no deadline)");
        kprintf("      va=0x%lx phys=0x%lx expected=0x%lx actual=0x%lx\n",
                (unsigned long)e->user_va, (unsigned long)e->phys_addr,
                (unsigned long)e->expected, (unsigned long)actual);
        if (lost_wake)
            kprintf("      ‼ LOST WAKE — value already changed, no deadline to save it\n");
        if (moved)
            kprintf("      ‼ UNREACHABLE — va now resolves to phys 0x%lx, filed under 0x%lx\n",
                    (unsigned long)phys_now, (unsigned long)e->phys_addr);
        }
    }

    /* Nothing proven and the box is not even stopped: say nothing at all. */
    bool proven = (lost || unreachable || undelivered);
    if (pass == 0 && !proven && !all_quiet)
    {
        kfree(probe);
        return;
    }
    if (pass == 0)
        kprintf("[NIGHTWATCH] %s at uptime %lu ms — %u process(es):\n",
                proven ? "EVIDENCE" : "every core idle",
                (unsigned long)clockboard_uptime_ms(), n);
    }

    for (uint8_t c = 0; c < g_amp.total_cores; c++)
    {
        scheduler_state_t *s = scheduler_get_core(c);
        if (!s) continue;
        process_t *cur = __atomic_load_n(&s->current_process, __ATOMIC_ACQUIRE);
        kprintf("  core %u: current=%u kcore_depth=%u\n",
                (unsigned)c, cur ? cur->pid : 0u, kcore_queue_depth(c));
    }

    if (skipped)
        kprintf("[NIGHTWATCH] %u process(es) appeared mid-walk and were not described\n",
                skipped);

    if (lost || unreachable || undelivered)
        kprintf("[NIGHTWATCH] VERDICT: %u lost wake(s), %u unreachable park(s), "
                "%u undelivered result(s) of %u parked — a defect, not a slow test\n",
                lost, unreachable, undelivered, parked);
    else if (parked && parked == timed)
        kprintf("[NIGHTWATCH] VERDICT: %u parked, all with deadlines — will recover\n",
                parked);
    else
        kprintf("[NIGHTWATCH] VERDICT: quiescent — %u parked, none provably stuck. "
                "If this is a hang, it is neither an addr-park nor a pending result\n",
                parked);

    kfree(probe);
}

void nightwatch_core_idle(uint8_t core)
{
    if (!__atomic_load_n(&g_armed, __ATOMIC_ACQUIRE)) return;
    if (core >= MAX_CORES) return;

    if (!g_core_idle[core])
    {
        g_core_idle[core] = 1;
        uint32_t now_idle = __atomic_add_fetch(&g_idle_cores, 1, __ATOMIC_ACQ_REL);
        if (now_idle >= g_watch_cores)
            __atomic_store_n(&g_quiet_since_ms, clockboard_uptime_ms(),
                             __ATOMIC_RELEASE);
        return;   /* just went quiet — nothing to judge yet */
    }

    /* Already marked idle: this is a wake-up (a timer tick, or simply the next
     * turn round the idle loop), the natural moment to look without a timer of
     * our own. */
    uint64_t now = clockboard_uptime_ms();

    /* Is the WHOLE box stopped? That still earns a description even when the
     * proofs come up empty — "everything is asleep and I cannot say why" is
     * itself worth printing once. */
    bool all_quiet = false;
    if (__atomic_load_n(&g_idle_cores, __ATOMIC_ACQUIRE) >= g_watch_cores)
    {
        uint64_t since = __atomic_load_n(&g_quiet_since_ms, __ATOMIC_ACQUIRE);
        all_quiet = (since != 0) && (now - since >= NIGHTWATCH_QUIET_MS) &&
                    !__atomic_load_n(&g_reported, __ATOMIC_ACQUIRE);
    }

    /* Otherwise this is the cheap periodic look: any idle core will do, rate
     * limited, and silent unless it can prove something. */
    if (!all_quiet)
    {
        uint64_t last = __atomic_load_n(&g_last_look_ms, __ATOMIC_ACQUIRE);
        if (last != 0 && now - last < NIGHTWATCH_LOOK_MS) return;
    }

    /* One core does the walk. */
    uint64_t last = __atomic_load_n(&g_last_look_ms, __ATOMIC_RELAXED);
    if (!__atomic_compare_exchange_n(&g_last_look_ms, &last, now, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    if (all_quiet)
        __atomic_store_n(&g_reported, 1, __ATOMIC_RELEASE);

    nightwatch_verdict(all_quiet);
}

void nightwatch_core_busy(uint8_t core)
{
    if (!__atomic_load_n(&g_armed, __ATOMIC_ACQUIRE)) return;
    if (core >= MAX_CORES) return;
    if (!g_core_idle[core]) return;

    g_core_idle[core] = 0;
    __atomic_sub_fetch(&g_idle_cores, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&g_quiet_since_ms, 0, __ATOMIC_RELEASE);
    /* Progress happened, so a LATER stall is a new episode and deserves its
     * own report. */
    __atomic_store_n(&g_reported, 0, __ATOMIC_RELEASE);
}
