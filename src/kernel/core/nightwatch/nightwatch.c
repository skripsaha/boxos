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
#include "chit.h"       /* ChitPeek — the kernel's half of the cloakroom token */
#include "pocket_ring.h"
#include "touch_ring.h"
#include "brook.h"       /* BrookBellUnrung — the surface the kernel never sees deliver */
#include "pit.h"        /* pit_get_uptime_ms — a clock that runs whether or not IRQ0 has reached the BSP yet */
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
 * Time is pit_get_uptime_ms(): the HPET main counter where there is one,
 * running from hpet_init, so it counts before IRQ0 has reached the BSP and
 * while every core sleeps in HLT. It is honest milliseconds, not a tick count,
 * so this spacing does not silently change meaning if the tick rate is ever
 * retuned. (The ClockBoard's uptime that used to be read here is the PIT tick
 * mirror, which stands at zero until the BSP takes its first IRQ0 — see
 * nightwatch_core_idle.)
 */
#define NIGHTWATCH_LOOK_MS  10000u

/* Per-core idle marks. A plain byte per core rather than a bitmask: MAX_CORES
 * is 256, so no single word covers it, and a byte store needs no atomic. */
static volatile uint8_t  g_core_idle[MAX_CORES];
static volatile uint32_t g_idle_cores;
static volatile uint8_t  g_armed;         /* init done */
static volatile uint64_t g_last_look_ms;   /* rate limit for the proof pass */
static volatile uint8_t  g_walking;        /* a core is inside nightwatch_verdict */

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
    uint64_t  actual;            /* the parked word, read ONCE in the judging pass */
    uint8_t   wait_reason;
    uint8_t   kcore_pending;
    uint8_t   served;            /* a K-Core is inside the guide for this strand */
    uint32_t  awaiting;          /* cloakroom token this strand holds out for */
    ChitView  chit;              /* the kernel's record of what was promised for it */
    uint64_t  result_ring_phys;
    uint64_t  touch_ring_phys;
    uint64_t  pocket_ring_phys;
} NightwatchProbe;

/* Persistence for the oscillation-proof oracles. A busy box makes an
 * unserved pocket or an unscheduled runnable a microsecond transient; a
 * WEDGED box keeps them frozen. One stranded strand spinning in
 * result_wait keeps a scheduler ticking, which keeps resetting the
 * all-quiet clock — the exact blindness that hid the kcore_pending
 * wedge — so these proofs cannot depend on quiet at all. Instead a
 * suspect must be seen in the SAME stuck position on two consecutive
 * periodic looks (>= NIGHTWATCH_LOOK_MS apart) before it is a verdict. */
typedef struct
{
    uint32_t pid;
    uint32_t generation;
    uint64_t mark;      /* pocket head (unserved), 1 (unscheduled), the awaited
                         * token (answer owed), the consumer cursor that has
                         * not moved (undelivered), or the park's seq (lost wake) */
    uint8_t  kind;      /* 1 = pocket unserved, 2 = runnable unscheduled,
                         * 3 = answer owed, 4 = result unread, 5 = touch unread,
                         * 6 = brook bell unrung, 7 = lost wake */
    uint64_t seen_ms;   /* when it was FIRST seen in this position; a verdict
                         * needs that to be a full NIGHTWATCH_LOOK_MS ago */
} NightwatchSuspect;

/* How many proofs can name one process in one look. Each proof below asks
 * each probe exactly once, so a look can never write down more than
 * processes × proofs suspects: the list is sized from that, with the probes,
 * and replaces the previous look's list at the end. What stood here was a
 * static table of 32, and the 33rd candidate was simply not written down —
 * a strand that could never be convicted, in silence. Once every parked
 * strand is a candidate (LOST WAKE below), a brigade wider than the table
 * would have stepped over its own lost wake. */
#define NIGHTWATCH_KINDS 7u
static NightwatchSuspect *g_suspects;       /* what the previous look wrote down */
static uint32_t           g_suspect_count;

/* Stalls already announced.
 *
 * Keyed by the STALL, not by a single token: one shared "last token said"
 * variable is wrong the moment two strands are owed at once, because each
 * overwrites the other's and both then speak on every look — the very
 * repetition the suppression exists to stop.
 *
 * Written only from the verdict, which one core at a time enters through the
 * CAS on g_last_look_ms. The accesses are still spelled out atomically rather
 * than left as plain loads and stores: the writer is serialised but it is not
 * always the SAME core, and state handed between cores must say so in the
 * code, not rely on the reader knowing that x86 stores happen to be visible.
 * A small ring — a machine with more than this many distinct stalls at once
 * has one story, not eight, and the oldest entry is the one worth losing. */
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

/* One suspect, one look. It is remembered for the next look (on the silent
 * pass only, so the speaking pass does not record it twice) together with
 * the time it was FIRST seen in this position, and it is stuck when that time
 * is a full NIGHTWATCH_LOOK_MS ago. TIME, not a count of looks. The looks are
 * rate-limited to that spacing, but the proof must not lean on the limiter:
 * with a clock that read zero the limiter let every idle turn look, "two
 * consecutive looks" were microseconds apart, and a shell's very first pocket
 * — in flight for a moment while the K-Cores were still starting — was named
 * POCKET UNSERVED ten times on UEFI 16c before the clock began to count. */
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
            /* Unreachable by construction (NIGHTWATCH_KINDS). If a proof is
             * ever added without being counted there, this says so instead of
             * losing the suspect in silence. */
            kprintf("[NIGHTWATCH] suspect list full at %u — a proof was added "
                    "and not counted in NIGHTWATCH_KINDS\n", fresh_cap);
    }
    return since != 0 && now_ms - since >= NIGHTWATCH_LOOK_MS;
}

/* An idle K-Core coexisting with a queued-but-unserved pocket is impossible
 * when the doorbell chain is healthy: the K-Core's CLI-gated recheck refuses
 * to sleep over a non-empty queue, and a submit IPIs it awake. So "a K-Core
 * sleeps while this pocket waits" upgrades a slow box to a broken one. */
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

/* Read the watched word through the Pull Map. Pure arithmetic — no VMM lock,
 * no page walk — so this is safe to do anywhere, including a stalled system
 * where taking another lock would be its own risk. */
static uint64_t nightwatch_peek(uintptr_t phys)
{
    volatile uint64_t *p = (volatile uint64_t *)vmm_phys_to_virt(phys);
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

/* ANSWER OWED. A strand is holding out for a reply to a submit, and nobody
 * holds a chit for it — or the one who does let it fall due and never paid.
 *
 * This is the one stall the other oracles cannot see. They look for work that
 * was never taken; this one is about work that WAS taken and never paid for —
 * the pocket is gone from the ring, no K-Core is serving the strand, and it
 * waits on a token that will never be answered. Waiting is correct behaviour
 * here, which is what makes it invisible: there is no deadline to expire and
 * nothing left queued to notice.
 *
 * Judged on FACTS about THIS strand, never on a clock and never on the state
 * of the machine around it. It used to lean on both — "every K-Core asleep"
 * and "no answer published anywhere since the last look" — and both are
 * witnesses, not proofs: a brigade of workers parked on their leader, who is
 * merely computing for a while, satisfies them completely (MEASURED,
 * 2026-09-06: seven workers of std::execution::par accused at once while the
 * test passed). The kernel's own record replaces them — chit.h:
 *
 *   awaiting C          the waiter says what it holds out for
 *   pocket ring empty   the work was taken, not queued (POCKET UNSERVED's domain)
 *   not served          no K-Core is inside the guide for this strand
 *   no chit for C       nobody promised the answer: dropped between the pop
 *                       and the push                                 → OWED
 *   chit DUE for C      the event came; the delivery never followed  → OWED,
 *                       and the holder is named
 *   chit PENDING for C  somebody outside the kernel owes the event — a peer, a
 *                       child, a device, a deadline. Lawful for as long as it
 *                       takes; described, never convicted
 *   chit KEPT for C     the answer is in the ring; a strand still asleep on it
 *                       is RESULT UNDELIVERED's finding, not this one's
 *
 * and all of it still so one full look later: the microseconds between a pop
 * and its push, or between a keep and the waiter clearing `awaiting`, cannot
 * span two looks. The mark carries the chit's state, so a promise first seen
 * unheld and then found due begins its own two looks.
 *
 * Deliberately NOT gated on being off-cpu — a strand PARKED on a reply and one
 * SPINNING in userspace for it are the same stall — nor on the presence of
 * K-Cores: a one-core machine runs the guide inline, and when the watch looks
 * no handler is mid-flight, so the same facts hold there. */
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
            return false;                            /* still queued */
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

    const bool due  = held;                          /* held, therefore DUE */
    uint64_t   mark = (uint64_t)e->awaiting |
                      ((uint64_t)(due ? CHIT_DUE : CHIT_NONE) << 32);
    if (!nightwatch_suspect_stuck(e->pid, e->generation, mark, 3u, now_ms, record,
                                  fresh, fresh_count, fresh_cap))
        return false;

    /* One line per stall, not one every ten seconds for as long as it lasts: a
     * watch that repeats itself buries the evidence it just produced. */
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

/*
 * The verdict pass. Runs with no locks held except where noted, and prints
 * nothing while holding process_lock — kprintf takes the console lock, and
 * that ordering is not one this kernel establishes anywhere else.
 */
static void nightwatch_verdict(void)
{
    /* Size the snapshot before taking the lock: allocating under process_lock
     * would nest the heap lock inside it for no reason. A few spare slots
     * absorb processes created between the count and the walk; any that still
     * do not fit are named in the summary rather than silently dropped. */
    uint32_t  slots = process_get_count() + 8u;
    NightwatchProbe *probe = kmalloc(slots * sizeof(NightwatchProbe));
    /* The suspects this look may write down: one per probe per proof, sized
     * from the same count as the probes (NIGHTWATCH_KINDS). */
    const uint32_t     fresh_cap = slots * NIGHTWATCH_KINDS;
    NightwatchSuspect *fresh     = probe ? kmalloc(fresh_cap * sizeof(NightwatchSuspect))
                                         : NULL;
    if (!probe || !fresh)
    {
        kfree(probe);
        kprintf("[NIGHTWATCH] no memory to describe this look — it is skipped\n");
        return;
    }
    /* One reading of the clock for the whole look: every suspect recorded
     * below is stamped with it and judged against it. Never zero here — the
     * caller does not look without a running clock. */
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
        /* What this strand says it is holding out for. The kernel cannot infer
         * it — a strand inside result_wait is PROC_WORKING or parked, and
         * neither says WHAT for — so the waiter publishes it into its own
         * reply-ring header and this reads it. A guest may write anything
         * there; it can only mislead the report about itself, which is why it
         * is a hint that must be corroborated by facts below and never trusted
         * on its own. Read through the direct map, so it cannot fault. */
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
        ChitPeek(p, &e->chit);   /* chit.lock: a leaf under process_list_lock */
    }
    process_list_unlock();

    /* What each strand was PROMISED is read with the snapshot above (ChitPeek):
     * the handler that put an answer off left a chit naming itself, the event
     * that made the answer marked it due, the push that delivered it kept it.
     * The ANSWER OWED oracle below judges on that record and on nothing about
     * the machine at large — see nightwatch_answer_owed.
     *
     * TURN IN is an open-ended wait and deliberately not a problem here: it is
     * submitted without a cloakroom token, so `awaiting` stays 0 and the oracle
     * — which convicts only on a token — steps over an idle sleeper without
     * needing to be told about it. What DOES speak for a turned-in strand is
     * the delivery evidence: a Result or Touch sitting unread in its ring, or a
     * Brook frame with its bell still hanging. Those are facts about a delivery
     * that happened, and they are exactly what a lost wake in this new sleep
     * looks like. */

    uint32_t parked = 0, lost = 0, unreachable = 0, timed = 0, undelivered = 0;
    uint32_t unserved = 0, unsched = 0, owed = 0;
    uint32_t fresh_count = 0;

    /* Two passes: judge in silence, then speak only with evidence. A pass that
     * finds nothing must cost nothing on the console — that is what lets this
     * run whenever any core is idle instead of only when all of them are. */
    for (uint32_t pass = 0; pass < 2; pass++)
    {
    bool speak = (pass == 1);
    parked = lost = unreachable = timed = undelivered = 0;
    unserved = unsched = owed = 0;
    for (uint32_t i = 0; i < n; i++)
    {
        NightwatchProbe *e = &probe[i];

        /* POCKET UNSERVED. The submit itself was never taken: the PocketRing
         * holds published pockets, no K-Core is serving their owner, and at
         * least one K-Core is asleep — which a healthy doorbell chain makes
         * impossible over a queued pocket. One strand waiting for such a
         * pocket keeps a scheduler ticking, which is exactly what kept the
         * all-quiet clock at zero and this defect invisible — so this proof
         * is judged on PERSISTENCE instead: the same pocket head, unserved,
         * seen on two consecutive periodic looks.
         *
         * Whether the owner is on a CPU is deliberately NOT asked. On more
         * than one core a pocket ring has one consumer, the K-Core guide, and
         * the strand that filled it cannot drain it however long it runs —
         * and the shape this exists for is an owner that IS on a CPU:
         * result_wait without WAITPKG spins on PAUSE and never leaves it,
         * with kcore_pending set so no notify of its own can ring the bell
         * again. Gated on being off-CPU, this proof stepped over the very
         * strand it describes, and a submit lost by the K-Core queue (the
         * claim taken from under its producer, kcore.c) stopped the machine
         * without a word from here. What separates "nobody came" from "the
         * guide is still working on it" (a proc_exec reading its image off a
         * slow medium keeps the pocket at the head for minutes) is the
         * K-Core's own record of whom it serves. */
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

        /* RUNNABLE UNSCHEDULED. WORKING, off-CPU, and still exactly there on
         * the next look: the runqueue lost it, or a wake never enqueued it.
         * Persistence gates it for the same oscillation reason as above. */
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
        /* And the surface that leaves no trace at all. A Brook push or pop is a
         * store into a shared page — the kernel never sees it happen, so a
         * strand asleep on one has both rings quiet and looks, by every other
         * measure here, like a strand with simply nothing to do. Both ends
         * qualify: a reader waiting for a frame and a writer waiting for a slot
         * are one defect in two hats. The bell is the only mark either leaves —
         * and the mark is that it hung one AT ALL, not that one is still up: the
         * peer takes the bell before it rings, so a lost ring leaves the header
         * looking clean. See BrookBellUnrung. */
        bool     bell_unrung = false;
        uint16_t bell_tag    = 0;
        uint64_t bk_head = 0, bk_tail = 0;
        if (e->state == (uint8_t)PROC_WAITING)
            bell_unrung = BrookBellUnrung(e->pid, &bell_tag, &bk_head, &bk_tail);

        /* ‼ ONE LOOK IS NOT EVIDENCE HERE, and it took a healthy machine to
         * show it. A delivery is two stores — the record, then the owner's
         * wake — and a walk that lands between them sees exactly what a lost
         * wake looks like: a strand asleep with something unread. That gap is
         * microseconds wide and used to be unreachable, because nothing slept
         * often enough to be caught in it. Once both ends of every Brook
         * started sleeping on bells, a print storm put sixteen strands through
         * it thousands of times a second, and a run that passed cleanly was
         * accused of five undelivered results.
         *
         * So the test is not "unread now" but "unread, and this consumer has
         * not moved its cursor a whole look later". A delivery in flight
         * completes in microseconds and cannot survive ten seconds; a lost
         * wake does, by definition, for ever. The cursor is the mark, so a
         * strand that woke, drained and went back to sleep is not the same
         * suspect — it is a different one, and it walks free.
         *
         * Nothing is lost by waiting: the second look is the same look. */
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
        /* The word is read ONCE, in the judging pass, and the speaking pass
         * prints that same value. It used to be read again for printing, tens
         * of milliseconds and several console lines later, so the number in
         * the evidence was not the number that had been judged. */
        if (pass == 0) e->actual = nightwatch_peek(e->phys_addr);
        const uint64_t actual = e->actual;

        /* LOST WAKE. The value it parked on has changed, nothing is going to
         * time this park out, the entry is still in its bucket — and all of
         * that is STILL so a full look later, for the same park.
         *
         * ‼ One look is not proof here either, and it took a passing test to
         * show it. "Parked, no deadline, value changed" was convicted on the
         * spot as a contradiction true at an instant. It is not one. A waker
         * stores first and asks for the wake in a SEPARATE call, so for the
         * length of one syscall every delivery looks exactly like this; and
         * this walk is not an instant at all — the park's expectation was
         * copied under process_lock above, the word is read here, after the
         * gone-list walk. MEASURED on BIOS 16c (2026-09-06): eleven workers of
         * the std::execution::par brigade named at once, the printed words
         * climbing along the walk (0xc1, 0xc3, 0xc7 … 0xd3 against one and
         * the same expectation) because the brigade kept running regions
         * while the watch walked and printed, and cxxtest passed the phase a
         * moment later.
         *
         * The mark is the park's own seq: a strand that was woken and parked
         * again is a different suspect and walks free. A delivery in flight
         * cannot last a whole look; a wake that was lost lasts for ever. */
        bool lost_wake = false;
        if (actual != e->expected && !e->timed)
            lost_wake = nightwatch_suspect_stuck(e->pid, e->generation,
                                                 (uint64_t)e->seq, 7u, now_ms,
                                                 pass == 0, fresh, &fresh_count,
                                                 fresh_cap);
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
        if (bell_unrung)
            kprintf("      ‼ BELL UNRUNG — asleep with a brook bell hung out while "
                    "tag %u could have served it (head=%lu tail=%lu)\n",
                    (unsigned)bell_tag,
                    (unsigned long)bk_head, (unsigned long)bk_tail);
        }
    }

    /* Nothing proven and the box is not even stopped: say nothing at all. */
    bool proven = (lost || unreachable || undelivered || unserved || unsched || owed);
    if (pass == 0 && !proven)
    {
        /* Nothing to say this look — but remember today's suspects so the
         * NEXT look can convict what stays frozen in place. */
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

    if (lost || unreachable || undelivered || unserved || unsched || owed)
        kprintf("[NIGHTWATCH] VERDICT: %u lost wake(s), %u unreachable park(s), "
                "%u undelivered result(s), %u unserved pocket(s), "
                "%u unscheduled runnable(s), %u answer(s) owed of %u parked "
                "— a defect, not a slow test\n",
                lost, unreachable, undelivered, unserved, unsched, owed, parked);
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
        return;   /* just went quiet — nothing to judge yet */
    }

    /* Already marked idle: this is a wake-up (a timer tick, or simply the next
     * turn round the idle loop), the natural moment to look without a timer of
     * our own. */
    uint64_t now = pit_get_uptime_ms();

    /* No clock, no look. This is the HPET main counter where there is one,
     * and it runs from hpet_init; without an HPET it is the PIT tick count,
     * which stands at zero until IRQ0 first reaches the BSP — well after
     * userspace has started on a big machine. A watch that cannot say how
     * long something has stood still has no persistence to judge by, and a
     * zero here also broke the rate limit below, whose "never looked" is
     * zero: every idle turn looked, and the looks were microseconds apart.
     * (The ClockBoard's uptime that used to be read here is that same PIT
     * tick count mirrored for userspace, HPET or not.) */
    if (now == 0) return;

    /* There used to be a second mode here: "are ALL cores asleep?", which
     * printed a description even with no proof to offer. It was removed
     * because it never once fired and could not have.
     *
     * MEASURED: four cores, seven minutes at an idle prompt — not a line. The
     * clock it depended on was reset by nightwatch_core_busy every time any
     * core picked up a real process, and on this machine something always
     * wakes: a daemon, a timer, a strand looking for work. Ten uninterrupted
     * seconds of universal sleep never happen.
     *
     * And it could not have earned its keep even if it had fired, because the
     * thing it claimed to detect is indistinguishable from health. An idle
     * prompt IS every core asleep and every strand parked — an x-ray of this
     * machine waiting for a keypress looks exactly like an x-ray of it wedged.
     * What separates them is not how many cores sleep but WHAT the sleepers
     * are waiting for, which is the question the ANSWER OWED oracle asks.
     *
     * So: one cheap periodic look from any idle core, and silence unless
     * something can be proven. A watch that cannot prove anything has nothing
     * to say, and saying it anyway is how a log teaches people to skim. */
    uint64_t last = __atomic_load_n(&g_last_look_ms, __ATOMIC_ACQUIRE);
    if (last != 0 && now - last < NIGHTWATCH_LOOK_MS) return;

    /* One core does the walk — and that is two facts, not one.
     *
     * The cadence: the mark moves from the value the time check was made ON.
     * It used to be re-read right before the CAS, and on sixteen cores that
     * let a second idle core, woken by the same tick, see the mark the first
     * had just set, pass its CAS on that fresh value, and walk at the same
     * time. While the suspect list was a static table the two walks merely
     * tore it; the moment the list became heap-owned, both freed it — a
     * double free in kfree, measured on BIOS 16c at cxxtest phase 68 and on
     * UEFI 16c at phase 81 (the dead core then failed a TLB shootdown ACK).
     *
     * The exclusion: g_walking is held for as long as the walk takes, so a
     * look cannot start over one still in progress whatever the cadence and
     * however long a walk spends printing or waiting on a lock. */
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
