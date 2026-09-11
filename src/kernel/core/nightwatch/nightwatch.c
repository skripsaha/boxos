
#include "nightwatch.h"
#include "process.h"
#include "addr_wait.h"
#include "cabin.h"
#include "amp.h"
#include "vmm.h"
#include "scheduler.h"
#include "kcore.h"
#include "result_ring.h"
#include "chit.h"
#include "pocket_ring.h"
#include "touch_ring.h"
#include "brook.h"
#include "pit.h"
#include "klib.h"

#define NIGHTWATCH_LOOK_MS  10000u

static volatile uint8_t  g_core_idle[MAX_CORES];
static volatile uint32_t g_idle_cores;
static volatile uint8_t  g_armed;
static volatile uint64_t g_last_look_ms;
static volatile uint8_t  g_walking;

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
    uint64_t  actual;
    uint8_t   wait_reason;
    uint8_t   kcore_pending;
    uint8_t   served;
    uint32_t  awaiting;
    ChitView  chit;
    uint64_t  result_ring_phys;
    uint64_t  touch_ring_phys;
    uint64_t  pocket_ring_phys;
} NightwatchProbe;

typedef struct
{
    uint32_t pid;
    uint32_t generation;
    uint64_t mark;
    uint8_t  kind;
    uint64_t seen_ms;
} NightwatchSuspect;

#define NIGHTWATCH_KINDS 7u
static NightwatchSuspect *g_suspects;
static uint32_t           g_suspect_count;

#define NIGHTWATCH_SAID 8u
static volatile uint32_t g_said_pid[NIGHTWATCH_SAID];
static volatile uint32_t g_said_gen[NIGHTWATCH_SAID];
static volatile uint32_t g_said_token[NIGHTWATCH_SAID];
static volatile uint32_t g_said_next;

static bool nightwatch_already_said(uint32_t pid, uint32_t gen, uint32_t token)
{
    for (uint32_t i = 0; i < NIGHTWATCH_SAID; i++)
        if (__atomic_load_n(&g_said_token[i], __ATOMIC_RELAXED) == token &&
            __atomic_load_n(&g_said_pid[i],   __ATOMIC_RELAXED) == pid   &&
            __atomic_load_n(&g_said_gen[i],   __ATOMIC_RELAXED) == gen)
            return true;
    return false;
}

static void nightwatch_mark_said(uint32_t pid, uint32_t gen, uint32_t token)
{
    uint32_t i = __atomic_load_n(&g_said_next, __ATOMIC_RELAXED) % NIGHTWATCH_SAID;
    __atomic_store_n(&g_said_pid[i],   pid,   __ATOMIC_RELAXED);
    __atomic_store_n(&g_said_gen[i],   gen,   __ATOMIC_RELAXED);
    __atomic_store_n(&g_said_token[i], token, __ATOMIC_RELAXED);
    __atomic_store_n(&g_said_next,     i + 1u, __ATOMIC_RELEASE);
}

static uint64_t nightwatch_suspect_seen(uint32_t pid, uint32_t gen,
                                        uint64_t mark, uint8_t kind)
{
    for (uint32_t i = 0; i < g_suspect_count; i++)
        if (g_suspects[i].pid == pid && g_suspects[i].generation == gen &&
            g_suspects[i].mark == mark && g_suspects[i].kind == kind)
            return g_suspects[i].seen_ms;
    return 0;
}

static bool nightwatch_suspect_stuck(uint32_t pid, uint32_t gen, uint64_t mark,
                                     uint8_t kind, uint64_t now_ms, bool record,
                                     NightwatchSuspect *fresh, uint32_t *fresh_count,
                                     uint32_t fresh_cap)
{
    uint64_t since = nightwatch_suspect_seen(pid, gen, mark, kind);
    if (record)
    {
        if (*fresh_count < fresh_cap)
        {
            NightwatchSuspect *f = &fresh[(*fresh_count)++];
            f->pid        = pid;
            f->generation = gen;
            f->mark       = mark;
            f->kind       = kind;
            f->seen_ms    = since ? since : now_ms;
        }
        else
            kprintf("[NIGHTWATCH] suspect list full at %u — a proof was added "
                    "and not counted in NIGHTWATCH_KINDS\n", fresh_cap);
    }
    return since != 0 && now_ms - since >= NIGHTWATCH_LOOK_MS;
}

static bool nightwatch_any_kcore_idle(void)
{
    for (uint8_t c = 0; c < g_amp.total_cores; c++)
        if (g_amp.cores[c].is_kcore && g_core_idle[c])
            return true;
    return false;
}

static bool nightwatch_any_core_idle(void)
{
    return __atomic_load_n(&g_idle_cores, __ATOMIC_ACQUIRE) > 0;
}

void nightwatch_init(void)
{
    for (uint32_t i = 0; i < MAX_CORES; i++)
        g_core_idle[i] = 0;
    g_idle_cores = 0;

    uint32_t online = 0;
    for (uint8_t c = 0; c < g_amp.total_cores; c++)
        if (amp_core_online(&g_amp.cores[c])) online++;

    __atomic_store_n(&g_armed, 1, __ATOMIC_RELEASE);
    kprintf("[NIGHTWATCH] armed — %u core(s) watched; speaks only with proof\n",
            online);
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

static uint64_t nightwatch_peek(uintptr_t phys)
{
    volatile uint64_t *p = (volatile uint64_t *)vmm_phys_to_virt(phys);
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static bool nightwatch_answer_owed(const NightwatchProbe *e, uint64_t now_ms,
                                   bool record, bool speak,
                                   NightwatchSuspect *fresh, uint32_t *fresh_count,
                                   uint32_t fresh_cap)
{
    if (e->awaiting == 0 || e->served ||
        e->state == (uint8_t)PROC_DONE || e->state == (uint8_t)PROC_CRASHED)
        return false;

    if (e->pocket_ring_phys)
    {
        const PocketRingHeader *ph =
            (const PocketRingHeader *)vmm_phys_to_virt(e->pocket_ring_phys);
        if (__atomic_load_n(&ph->tail, __ATOMIC_ACQUIRE) !=
            __atomic_load_n(&ph->head, __ATOMIC_ACQUIRE))
            return false;
    }

    const ChitView *c    = &e->chit;
    const bool      held = (c->cookie == e->awaiting);
    if (held && c->state == CHIT_PENDING)
    {
        if (speak)
            kprintf("  pid %u gen %u %s — waits on %s (0x%lx) for token 0x%06x: the "
                    "event is owed outside the kernel, pending %lu ms — lawful\n",
                    e->pid, e->generation, nightwatch_state_name(e->state),
                    c->holder ? c->holder : "?", (unsigned long)c->detail,
                    (unsigned)e->awaiting, (unsigned long)(now_ms - c->since_ms));
        return false;
    }
    if (held && c->state == CHIT_KEPT) return false;

    const bool due  = held;
    uint64_t   mark = (uint64_t)e->awaiting |
                      ((uint64_t)(due ? CHIT_DUE : CHIT_NONE) << 32);
    if (!nightwatch_suspect_stuck(e->pid, e->generation, mark, 3u, now_ms, record,
                                  fresh, fresh_count, fresh_cap))
        return false;

    if (speak && !nightwatch_already_said(e->pid, e->generation, e->awaiting))
    {
        nightwatch_mark_said(e->pid, e->generation, e->awaiting);
        if (due)
            kprintf("  pid %u gen %u %s — ‼ ANSWER OWED: %s took the event for token "
                    "0x%06x %lu ms ago (0x%lx) and the answer was never delivered "
                    "— the kernel owes it and nothing is producing it\n",
                    e->pid, e->generation, nightwatch_state_name(e->state),
                    c->holder ? c->holder : "?", (unsigned)e->awaiting,
                    (unsigned long)(now_ms - c->since_ms), (unsigned long)c->detail);
        else
            kprintf("  pid %u gen %u %s — ‼ ANSWER OWED: waiting on token 0x%06x "
                    "across two looks; its pocket ring is empty, no K-Core serves "
                    "it, and no handler left a chit for it — the work was taken "
                    "and nobody promised the answer\n",
                    e->pid, e->generation, nightwatch_state_name(e->state),
                    (unsigned)e->awaiting);
    }
    return true;
}

static void nightwatch_verdict(void)
{
    uint32_t  slots = process_get_count() + 8u;
    NightwatchProbe *probe = kmalloc(slots * sizeof(NightwatchProbe));
    const uint32_t     fresh_cap = slots * NIGHTWATCH_KINDS;
    NightwatchSuspect *fresh     = probe ? kmalloc(fresh_cap * sizeof(NightwatchSuspect))
                                         : NULL;
    if (!probe || !fresh)
    {
        kfree(probe);
        kprintf("[NIGHTWATCH] no memory to describe this look — it is skipped\n");
        return;
    }
    const uint64_t now_ms = pit_get_uptime_ms();

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
        e->kcore_pending    = __atomic_load_n(&p->kcore_pending, __ATOMIC_ACQUIRE);
        e->served           = kcore_is_serving(p) ? 1u : 0u;
        e->awaiting = 0;
        if (p->result_ring_phys)
        {
            const ResultRing *arr =
                (const ResultRing *)vmm_phys_to_virt(p->result_ring_phys);
            if (arr)
                e->awaiting = (uint32_t)__atomic_load_n(&arr->hdr.awaiting,
                                                        __ATOMIC_ACQUIRE);
        }
        e->result_ring_phys = p->result_ring_phys;
        e->touch_ring_phys  = p->touch_ring_phys;
        e->pocket_ring_phys = p->pocket_ring_phys;
        ChitPeek(p, &e->chit);
    }
    process_list_unlock();


    uint32_t parked = 0, lost = 0, timed = 0, undelivered = 0;
    uint32_t unserved = 0, unsched = 0, owed = 0;
    uint32_t fresh_count = 0;

    for (uint32_t pass = 0; pass < 2; pass++)
    {
    bool speak = (pass == 1);
    parked = lost = timed = undelivered = 0;
    unserved = unsched = owed = 0;
    for (uint32_t i = 0; i < n; i++)
    {
        NightwatchProbe *e = &probe[i];

        if (e->pocket_ring_phys && !e->served && nightwatch_any_kcore_idle() &&
            e->state != (uint8_t)PROC_DONE && e->state != (uint8_t)PROC_CRASHED)
        {
            const PocketRingHeader *ph =
                (const PocketRingHeader *)vmm_phys_to_virt(e->pocket_ring_phys);
            uint64_t pk_head = __atomic_load_n(&ph->head, __ATOMIC_ACQUIRE);
            uint64_t pk_tail = __atomic_load_n(&ph->tail, __ATOMIC_ACQUIRE);
            if (pk_tail != pk_head)
            {
                bool stuck = nightwatch_suspect_stuck(e->pid, e->generation, pk_head, 1u,
                                                      now_ms, pass == 0, fresh, &fresh_count,
                                                      fresh_cap);
                if (stuck)
                {
                    unserved++;
                    if (speak)
                        kprintf("  pid %u gen %u %s — ‼ POCKET UNSERVED: ring "
                                "head=%lu tail=%lu, kcore_pending=%u, two looks "
                                "and no K-Core came\n",
                                e->pid, e->generation,
                                nightwatch_state_name(e->state),
                                (unsigned long)pk_head, (unsigned long)pk_tail,
                                (unsigned)e->kcore_pending);
                }
            }
        }

        if (e->state == (uint8_t)PROC_WORKING && !e->on_cpu &&
            nightwatch_any_core_idle())
        {
            bool stuck = nightwatch_suspect_stuck(e->pid, e->generation, 1u, 2u,
                                                  now_ms, pass == 0, fresh, &fresh_count,
                                                  fresh_cap);
            if (stuck)
            {
                unsched++;
                if (speak)
                    kprintf("  pid %u gen %u working — ‼ RUNNABLE UNSCHEDULED: "
                            "off-cpu across two looks (home=%u)\n",
                            e->pid, e->generation, e->home_core);
            }
        }

        if (nightwatch_answer_owed(e, now_ms, pass == 0, speak,
                                   fresh, &fresh_count, fresh_cap))
            owed++;

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
        bool     bell_unrung = false;
        uint16_t bell_tag    = 0;
        uint64_t bk_head = 0, bk_tail = 0;
        if (e->state == (uint8_t)PROC_WAITING)
            bell_unrung = BrookBellUnrung(e->pid, &bell_tag, &bk_head, &bk_tail);

        if (touch_ready) {
            touch_ready = nightwatch_suspect_stuck(e->pid, e->generation, tr_head, 5u,
                                                   now_ms, pass == 0, fresh, &fresh_count,
                                                   fresh_cap);
        }
        if (result_ready) {
            result_ready = nightwatch_suspect_stuck(e->pid, e->generation, rr_head, 4u,
                                                    now_ms, pass == 0, fresh, &fresh_count,
                                                    fresh_cap);
        }
        if (bell_unrung) {
            bell_unrung = nightwatch_suspect_stuck(e->pid, e->generation, bk_head, 6u,
                                                   now_ms, pass == 0, fresh, &fresh_count,
                                                   fresh_cap);
        }

        if (touch_ready)  undelivered++;
        if (result_ready) undelivered++;
        if (bell_unrung)  undelivered++;

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
            if (bell_unrung && speak)
                kprintf("      ‼ BELL UNRUNG — this strand went to sleep on a brook "
                        "with its bell hung out, and tag %u could have served it "
                        "(head=%lu tail=%lu): either frames it has not read, or "
                        "room it was never told about. A cursor moved for a "
                        "sleeper and nothing rang\n",
                        (unsigned)bell_tag,
                        (unsigned long)bk_head, (unsigned long)bk_tail);
            continue;
        }

        parked++;
        if (pass == 0) e->actual = nightwatch_peek(e->phys_addr);
        const uint64_t actual = e->actual;

        bool lost_wake = false;
        if (actual != e->expected && !e->timed)
            lost_wake = nightwatch_suspect_stuck(e->pid, e->generation,
                                                 (uint64_t)e->seq, 7u, now_ms,
                                                 pass == 0, fresh, &fresh_count,
                                                 fresh_cap);
        if (lost_wake) lost++;
        if (e->timed)  timed++;

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
        if (bell_unrung)
            kprintf("      ‼ BELL UNRUNG — asleep with a brook bell hung out while "
                    "tag %u could have served it (head=%lu tail=%lu)\n",
                    (unsigned)bell_tag,
                    (unsigned long)bk_head, (unsigned long)bk_tail);
        }
    }

    bool proven = (lost || undelivered || unserved || unsched || owed);
    if (pass == 0 && !proven)
    {
        kfree(g_suspects);
        g_suspects      = fresh;
        g_suspect_count = fresh_count;
        kfree(probe);
        return;
    }
    if (pass == 0)
        kprintf("[NIGHTWATCH] %s at uptime %lu ms — %u process(es):\n",
                "EVIDENCE",
                (unsigned long)now_ms, n);
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

    if (lost || undelivered || unserved || unsched || owed)
        kprintf("[NIGHTWATCH] VERDICT: %u lost wake(s), "
                "%u undelivered result(s), %u unserved pocket(s), "
                "%u unscheduled runnable(s), %u answer(s) owed of %u parked "
                "— a defect, not a slow test\n",
                lost, undelivered, unserved, unsched, owed, parked);
    else if (parked && parked == timed)
        kprintf("[NIGHTWATCH] VERDICT: %u parked, all with deadlines — will recover\n",
                parked);
    else
        kprintf("[NIGHTWATCH] VERDICT: quiescent — %u parked, none provably stuck. "
                "If this is a hang, it is neither an addr-park nor a pending result\n",
                parked);

    kfree(g_suspects);
    g_suspects      = fresh;
    g_suspect_count = fresh_count;

    kfree(probe);
}

void nightwatch_core_idle(uint8_t core)
{
    if (!__atomic_load_n(&g_armed, __ATOMIC_ACQUIRE)) return;
    if (core >= MAX_CORES) return;

    if (!g_core_idle[core])
    {
        g_core_idle[core] = 1;
        __atomic_add_fetch(&g_idle_cores, 1, __ATOMIC_ACQ_REL);
        return;
    }

    uint64_t now = pit_get_uptime_ms();

    if (now == 0) return;

    uint64_t last = __atomic_load_n(&g_last_look_ms, __ATOMIC_ACQUIRE);
    if (last != 0 && now - last < NIGHTWATCH_LOOK_MS) return;

    if (!__atomic_compare_exchange_n(&g_last_look_ms, &last, now, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    uint8_t nobody = 0;
    if (!__atomic_compare_exchange_n(&g_walking, &nobody, 1u, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;

    nightwatch_verdict();
    __atomic_store_n(&g_walking, 0, __ATOMIC_RELEASE);
}

void nightwatch_core_busy(uint8_t core)
{
    if (!__atomic_load_n(&g_armed, __ATOMIC_ACQUIRE)) return;
    if (core >= MAX_CORES) return;
    if (!g_core_idle[core]) return;

    g_core_idle[core] = 0;
    __atomic_sub_fetch(&g_idle_cores, 1, __ATOMIC_ACQ_REL);
}