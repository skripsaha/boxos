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
#include "kring.h"
#include "pocket_ring.h"
#include "touch_ring.h"
#include "brook.h"       /* BrookBellUnrung — the surface the kernel never sees deliver */
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
#define NIGHTWATCH_LOOK_MS  10000u

/* Per-core idle marks. A plain byte per core rather than a bitmask: MAX_CORES
 * is 256, so no single word covers it, and a byte store needs no atomic. */
static volatile uint8_t  g_core_idle[MAX_CORES];
static volatile uint32_t g_idle_cores;
static volatile uint8_t  g_armed;         /* init done */
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
    uint8_t   kcore_pending;
    uint32_t  awaiting;          /* cloakroom token this strand holds out for */
    uint64_t  result_ring_phys;
    uint64_t  touch_ring_phys;
    uint64_t  pocket_ring_phys;
} NightwatchProbe;

/* Persistence for the two oscillation-proof oracles. A busy box makes an
 * unserved pocket or an unscheduled runnable a microsecond transient; a
 * WEDGED box keeps them frozen. One stranded strand spinning in
 * result_wait keeps a scheduler ticking, which keeps resetting the
 * all-quiet clock — the exact blindness that hid the kcore_pending
 * wedge — so these proofs cannot depend on quiet at all. Instead a
 * suspect must be seen in the SAME stuck position on two consecutive
 * periodic looks (>= NIGHTWATCH_LOOK_MS apart) before it is a verdict. */
#define NIGHTWATCH_SUSPECTS 32u
typedef struct
{
    uint32_t pid;
    uint32_t generation;
    uint64_t mark;      /* pocket head (unserved) or 1 (unscheduled) */
    uint8_t  kind;      /* 1 = pocket unserved, 2 = runnable unscheduled */
} NightwatchSuspect;
static NightwatchSuspect g_suspects[NIGHTWATCH_SUSPECTS];
static uint32_t          g_suspect_count;

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

static bool nightwatch_was_suspect(uint32_t pid, uint32_t gen,
                                   uint64_t mark, uint8_t kind)
{
    for (uint32_t i = 0; i < g_suspect_count; i++)
        if (g_suspects[i].pid == pid && g_suspects[i].generation == gen &&
            g_suspects[i].mark == mark && g_suspects[i].kind == kind)
            return true;
    return false;
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

/* EVERY K-Core asleep — nobody in the kernel is working on anybody's submit.
 * Deliberately distinct from any_kcore_idle above: one sleeping K-Core proves
 * nothing while another is mid-op, and an op may be honest work of arbitrary
 * length (proc_exec reads the image INSIDE the call — on a throttled stick
 * that is minutes). Accusing on a clock would convict that work; asking
 * whether anyone is working on it convicts only a machine that has stopped. */
/* Has the machine delivered ANY answer since the last look?
 *
 * Every other fact this watch gathers describes one instant, and an instant is
 * not a stall: a strand can sit on one token for ten seconds while its own
 * process does useful work in other strands, with the pocket ring momentarily
 * empty and the K-Cores momentarily asleep. MEASURED — cxxtest was accused
 * forty-four times while it was passing phase after phase.
 *
 * Answers delivered is the one number that says the machine as a whole is
 * moving. If it has advanced since the previous look, nothing here is stuck,
 * whatever any single snapshot looked like. It is the difference between
 * "this strand is waiting" (ordinary) and "this strand is waiting and nothing
 * anywhere is being answered" (a stall). */
static bool nightwatch_answers_advanced(void)
{
    /* Serialised by the same CAS gate as the rest of the verdict, and spelled
     * atomically for the same reason: the single writer is not always the same
     * core. */
    static volatile uint64_t s_last;
    uint64_t stats[9];
    KResultPushStats(stats);
    uint64_t now  = stats[8];             /* successful publishes since boot */
    bool advanced = (now != __atomic_load_n(&s_last, __ATOMIC_RELAXED));
    __atomic_store_n(&s_last, now, __ATOMIC_RELEASE);
    return advanced;
}

static bool nightwatch_all_kcores_idle(void)
{
    bool any = false;
    for (uint8_t c = 0; c < g_amp.total_cores; c++)
        if (g_amp.cores[c].is_kcore)
        {
            if (!g_core_idle[c]) return false;
            any = true;
        }
    return any;
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
        e->kcore_pending    = __atomic_load_n(&p->kcore_pending, __ATOMIC_ACQUIRE);
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
    }
    process_list_unlock();

    /* Tokens whose silence is LAWFUL.
     *
     * A strand parked on process.gone is waiting for another process to die.
     * That answer is owed by an EVENT, not by anyone working right now, and a
     * child may legitimately run for hours — the shell waiting out a cxxtest
     * is exactly this, and a watch that convicts it is a watch nobody will
     * believe when it is finally right. So these tokens are collected and the
     * oracle below steps over them.
     *
     * Collected AFTER the snapshot and by reference, never under
     * process_list_lock: gone_lock is documented to nest inside nothing. If
     * the list does not fit, the oracle stands down for this look rather than
     * convict a strand it merely failed to look up — an oracle that guesses
     * when incomplete is worse than one that waits for the next look.
     *
     * ‼ KNOWN LIMIT, named rather than left to be rediscovered: process.gone
     * is the only lawful open-ended wait the kernel can currently RECOGNISE.
     * Storage reads, addr_park and touch_await also promise their answer for
     * later (the handlers that set async_owns_crates), and nothing records
     * that promise anywhere this walk can see it. Such a wait, if it were ever
     * both open-ended and alone on a sleeping machine, would be accused
     * wrongly. It has not happened — a full matrix and seven minutes at an
     * idle prompt both give owed=0, because storage waits run under load where
     * the progress witness sees answers flowing, and touch_await is bounded at
     * 30 s. Closing it properly means the kernel keeping a register of
     * promised answers, which is its own piece of work; until then this
     * paragraph is the honest edge of what the oracle knows.
     *
     * TURN IN is the newest open-ended wait and deliberately not a problem
     * here: it publishes no cloakroom token, so `awaiting` stays 0 and the
     * ANSWER OWED oracle below — which convicts only on a token — steps over
     * an idle sleeper without needing to be told about it. What DOES speak for
     * a turned-in strand is the delivery evidence: a Result or Touch sitting
     * unread in its ring, or a Brook frame with its bell still hanging. Those
     * are facts about a delivery that happened, and they are exactly what a
     * lost wake in this new sleep looks like. */
    /* Sampled exactly once per look: the call advances its own baseline, so a
     * second call in the same walk would always report "no progress". */
    const bool answers_moving = nightwatch_answers_advanced();

    uint32_t gone_tokens[NIGHTWATCH_SUSPECTS];
    uint32_t gone_count    = 0;
    bool     gone_complete = true;
    for (uint32_t i = 0; i < n; i++)
    {
        process_t *gp = process_find_ref(probe[i].pid);
        if (!gp) continue;
        spin_lock(&gp->gone_lock);
        for (GoneWaiter *w = gp->gone_waiters; w; w = w->next)
        {
            if (gone_count < NIGHTWATCH_SUSPECTS) gone_tokens[gone_count++] = w->submit_cookie;
            else                                  gone_complete = false;
        }
        spin_unlock(&gp->gone_lock);
        process_ref_dec(gp);
    }

    uint32_t parked = 0, lost = 0, unreachable = 0, timed = 0, undelivered = 0;
    uint32_t unserved = 0, unsched = 0, owed = 0;
    NightwatchSuspect fresh[NIGHTWATCH_SUSPECTS];
    uint32_t          fresh_count = 0;

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
         * holds published pockets, kcore_pending claims a K-Core owes a
         * visit, and the strand is not on a CPU — its userspace spins in
         * result_wait for an answer to a question no one has read. One such
         * spinner keeps a scheduler ticking, which is exactly what kept the
         * all-quiet clock at zero and this defect invisible — so this proof
         * is judged on PERSISTENCE instead: the same pocket head, unserved,
         * seen on two consecutive periodic looks. */
        if (e->pocket_ring_phys && !e->on_cpu && nightwatch_any_kcore_idle() &&
            e->state != (uint8_t)PROC_DONE && e->state != (uint8_t)PROC_CRASHED)
        {
            const PocketRingHeader *ph =
                (const PocketRingHeader *)vmm_phys_to_virt(e->pocket_ring_phys);
            uint64_t pk_head = __atomic_load_n(&ph->head, __ATOMIC_ACQUIRE);
            uint64_t pk_tail = __atomic_load_n(&ph->tail, __ATOMIC_ACQUIRE);
            if (pk_tail != pk_head)
            {
                bool stuck = nightwatch_was_suspect(e->pid, e->generation,
                                                    pk_head, 1u);
                if (pass == 0 && fresh_count < NIGHTWATCH_SUSPECTS)
                {
                    fresh[fresh_count].pid        = e->pid;
                    fresh[fresh_count].generation = e->generation;
                    fresh[fresh_count].mark       = pk_head;
                    fresh[fresh_count].kind       = 1u;
                    fresh_count++;
                }
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
            bool stuck = nightwatch_was_suspect(e->pid, e->generation, 1u, 2u);
            if (pass == 0 && fresh_count < NIGHTWATCH_SUSPECTS)
            {
                fresh[fresh_count].pid        = e->pid;
                fresh[fresh_count].generation = e->generation;
                fresh[fresh_count].mark       = 1u;
                fresh[fresh_count].kind       = 2u;
                fresh_count++;
            }
            if (stuck)
            {
                unsched++;
                if (speak)
                    kprintf("  pid %u gen %u working — ‼ RUNNABLE UNSCHEDULED: "
                            "off-cpu across two looks (home=%u)\n",
                            e->pid, e->generation, e->home_core);
            }
        }

        /* ANSWER OWED. A strand is holding out for a reply to a submit, and
         * nobody is producing it.
         *
         * This is the one stall the other oracles cannot see. They look for
         * work that was never taken; this one is about work that WAS taken and
         * never paid for — the pocket is gone from the ring, the doorbell is
         * quiet, and the strand waits on a token that will never be answered.
         * Waiting is correct behaviour here, which is what makes it invisible:
         * there is no deadline to expire and nothing left queued to notice.
         *
         * Judged on FACTS, never on a clock. A clock cannot work here at all:
         * proc_exec reads its image inside the call (minutes off a throttled
         * stick) and process.gone waits out a whole child's life (hours), and
         * both are owed an answer that is simply not due yet. So instead:
         *
         *   the pocket ring is empty          — the work was taken, not queued
         *   the doorbell is quiet             — no K-Core owes this strand a visit
         *   EVERY K-Core is asleep            — nobody is working on it now
         *   the token is not a lawful death-wait (above)
         *   and all of it is still true one full look later
         *
         * Together those say: the machine has stopped, and this strand is what
         * it stopped on. Any one of them alone is an ordinary busy moment. */
        /* Deliberately NOT gated on being off-cpu. The two shapes this has to
         * cover are opposites — a strand PARKED on a reply that never came,
         * and one SPINNING in userspace for it — and the spinner is on a CPU
         * by definition. Whether the waiter burns a core or sleeps says
         * nothing about whether anyone is producing its answer, which is the
         * only question here.
         *
         * On a uniprocessor this oracle stands down of its own accord, and
         * correctly: nightwatch_all_kcores_idle returns false when the machine
         * has no K-Cores at all, because "everyone who could be working on it
         * is asleep" is not a proof of anything when there is nobody in that
         * set — the one core does the work inline and may simply be busy
         * elsewhere. The other evidence here (a delivery lying unread, a bell
         * that went unrung) is about a delivery that HAPPENED and holds on any
         * number of cores; only this one needs K-Cores to mean what it says. */
        if (e->awaiting != 0 && gone_complete && !answers_moving &&
            e->kcore_pending == 0 &&
            e->state != (uint8_t)PROC_DONE && e->state != (uint8_t)PROC_CRASHED &&
            nightwatch_all_kcores_idle())
        {
            bool lawful = false;
            for (uint32_t g = 0; g < gone_count; g++)
                if (gone_tokens[g] == e->awaiting) { lawful = true; break; }

            bool pocket_empty = true;
            if (e->pocket_ring_phys)
            {
                const PocketRingHeader *oh =
                    (const PocketRingHeader *)vmm_phys_to_virt(e->pocket_ring_phys);
                pocket_empty = (__atomic_load_n(&oh->tail, __ATOMIC_ACQUIRE) ==
                                __atomic_load_n(&oh->head, __ATOMIC_ACQUIRE));
            }

            if (!lawful && pocket_empty)
            {
                bool stuck = nightwatch_was_suspect(e->pid, e->generation,
                                                    (uint64_t)e->awaiting, 3u);
                if (pass == 0 && fresh_count < NIGHTWATCH_SUSPECTS)
                {
                    fresh[fresh_count].pid        = e->pid;
                    fresh[fresh_count].generation = e->generation;
                    fresh[fresh_count].mark       = (uint64_t)e->awaiting;
                    fresh[fresh_count].kind       = 3u;
                    fresh_count++;
                }
                if (stuck)
                {
                    owed++;
                    /* One line per stall, not one every ten seconds for as
                     * long as it lasts: a watch that repeats itself buries the
                     * evidence it just produced. A different (pid, generation,
                     * token) is a different stall and speaks for itself. */
                    bool fresh_stall =
                        !nightwatch_already_said(e->pid, e->generation, e->awaiting);
                    if (speak && fresh_stall)
                    {
                        nightwatch_mark_said(e->pid, e->generation, e->awaiting);
                        kprintf("  pid %u gen %u %s — ‼ ANSWER OWED: waiting on "
                                "submit 0x%06x across two looks; its pocket ring "
                                "is empty, its K-Core doorbell is quiet, and every "
                                "K-Core is asleep. The work was taken and the "
                                "answer was never produced — no deadline can "
                                "expire on an answer nobody is making\n",
                                e->pid, e->generation,
                                nightwatch_state_name(e->state),
                                (unsigned)e->awaiting);
                    }
                }
            }
        }

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
        /* And the surface that leaves no trace at all. A Brook frame is a
         * store into a shared page — the kernel never sees it arrive, so a
         * reader asleep on one has both rings quiet and looks, by every other
         * measure here, like a strand with simply nothing to do. The bell is
         * the only mark it leaves — and the mark is that it hung one AT ALL,
         * not that one is still up: a writer takes the bell before it rings, so
         * a lost ring leaves the header looking clean. See BrookBellUnrung. */
        bool     bell_unrung = false;
        uint16_t bell_tag    = 0;
        uint64_t bk_head = 0, bk_tail = 0;
        if (e->state == (uint8_t)PROC_WAITING)
            bell_unrung = BrookBellUnrung(e->pid, &bell_tag, &bk_head, &bk_tail);

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
                kprintf("      ‼ BELL UNRUNG — this reader went to sleep with a bell "
                        "hung out, and brook tag %u holds head=%lu tail=%lu. "
                        "Frames were written for a strand that is asleep and "
                        "nothing told it\n",
                        (unsigned)bell_tag,
                        (unsigned long)bk_head, (unsigned long)bk_tail);
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
        if (bell_unrung)
            kprintf("      ‼ BELL UNRUNG — asleep with a bell hung out while brook "
                    "tag %u holds head=%lu tail=%lu\n",
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
        for (uint32_t s = 0; s < fresh_count; s++) g_suspects[s] = fresh[s];
        g_suspect_count = fresh_count;
        kfree(probe);
        return;
    }
    if (pass == 0)
        kprintf("[NIGHTWATCH] %s at uptime %lu ms — %u process(es):\n",
                "EVIDENCE",
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

    for (uint32_t s = 0; s < fresh_count; s++) g_suspects[s] = fresh[s];
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
    uint64_t now = clockboard_uptime_ms();

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

    /* One core does the walk. */
    last = __atomic_load_n(&g_last_look_ms, __ATOMIC_RELAXED);
    if (!__atomic_compare_exchange_n(&g_last_look_ms, &last, now, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;

    nightwatch_verdict();
}

void nightwatch_core_busy(uint8_t core)
{
    if (!__atomic_load_n(&g_armed, __ATOMIC_ACQUIRE)) return;
    if (core >= MAX_CORES) return;
    if (!g_core_idle[core]) return;

    g_core_idle[core] = 0;
    __atomic_sub_fetch(&g_idle_cores, 1, __ATOMIC_ACQ_REL);
}
