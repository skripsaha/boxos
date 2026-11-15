#include "settle_internal.h"
#include "binder.h"
#include "../scribe/scribe.h"
#include "../ink/ink.h"
#include "../helm/helm.h"
#include "amp.h"
#include "baton.h"
#include "kcore.h"
#include "process.h"
#include "scheduler.h"      /* g_global_tick, SCHEDULER_DEFAULT_TICK_HZ */
#include "touch_queue.h"

/*
 * The cadence (settle.h): when the committer begins a cycle. What it reads is
 * advisory — a race costs a cycle a moment early or late, never a change: the
 * cycle's own snapshot, taken under the state lock, decides what is settled.
 */

/* k — the one place it is derived from the medium's share. */
#define SETTLE_LULL_FACTOR  (100u / SETTLE_SHARE - 1u)

static volatile uint64_t s_d_us;           /* the last settled cycle's own time on the medium */
static volatile uint64_t s_last_end_us;    /* when the last cycle ended */
static volatile uint64_t s_owed_since_us;  /* when the oldest change no cycle settled was inked */
static volatile uint64_t s_eager_gen;      /* the newest Ink that starts a cycle by Lull */
static volatile uint32_t s_demand;         /* SettleDemandKind */
static volatile uint64_t s_lull_fire;      /* the tick the Lull's node was last armed for */
static volatile int32_t  s_held;           /* the test runner's holds */
static volatile uint64_t s_room_asked;     /* SETTLE_DEMAND_ROOM, counted */
static bool              s_binds_said;     /* the window binds the Lull: said once an episode */

static TouchQueueNode s_lull_node;

uint8_t SettleCommitterCore(void)
{
    if (g_amp.total_cores <= 1 || !g_amp.multicore_active || g_amp.k_count <= 1)
        return g_amp.bsp_index;
    for (uint8_t c = 0; c < g_amp.total_cores; c++) {
        const CoreDescriptor* core = &g_amp.cores[c];
        if (core->is_kcore && c != g_amp.bsp_index && amp_core_online(core))
            return c;
    }
    return g_amp.bsp_index;
}

/*
 * ‼ THE COMMITTER IS WOKEN WHERE THE WORK IS MADE, NEVER BY A BATON (RT-11).
 *
 * Every Ink, every demand, every look asked for and the Lull's own node wake
 * the committer's core here, with an IPI when it sleeps in its gate — one load
 * otherwise, and nothing at all for the committer itself, which goes round its
 * loop after whatever it is in. It used to be a knock: a baton on the drain
 * core whose continuation woke the committer. A drain core busy with work that
 * pumps no baton — a span of gigabytes run whole at its service point, a seat
 * seated — left the committer asleep in its gate past the window, the change
 * the window promised unsettled, and the drain core was also where Nightwatch
 * would have said so. On one core there is nobody to wake: idle is the debtor
 * of the quantum (TagFSIdleOwed).
 */
static void CadenceWake(void)
{
    kcore_wake(SettleCommitterCore());
}

uint64_t SettleLullUs(void)
{
    uint64_t tick = 1000000ull / SCHEDULER_DEFAULT_TICK_HZ;
    uint64_t lull = SETTLE_LULL_FACTOR * __atomic_load_n(&s_d_us, __ATOMIC_RELAXED);
    if (lull < tick)
        lull = tick;
    return lull < SETTLE_WINDOW_US ? lull : SETTLE_WINDOW_US;
}

uint64_t SettleLastDUs(void)
{
    return __atomic_load_n(&s_d_us, __ATOMIC_RELAXED);
}

/*
 * ‼ A PROGRAM'S DEBT IS ITS OWN INK, OF THIS MOUNT. What a program leaves
 * unsettled is the newest Ink one of its own calls made — never the volume's
 * newest, which a reader that leaves would otherwise owe for a writer still
 * running, and never an Ink of a mount before this one, whose gens begin again
 * at 1. So an Ink has a place in one order across every mount of the boot —
 * this mount's base and its gen — and a program keeps the newest place of its
 * own (process_t, volume_ink_place): a place below this mount's base is a
 * mount's before it, and owes nothing here. One number, so it is kept by one
 * compare-and-swap from any core that serves the program.
 *
 * The core that made an Ink remembers its place: the guide serves a program's
 * call without giving its core away, so an Ink the core made while serving it
 * is the call's own (TagFSInkedHere, read before and after the handler).
 */
static volatile uint64_t s_ink_base;               /* the place of this mount's Ink 0 */
static volatile uint64_t s_ink_top;                /* the newest gen of this mount given a place */
static volatile uint64_t s_inked_here[MAX_CORES];  /* the place of each core's newest Ink */

uint64_t TagFSInkPlace(uint64_t gen)
{
    uint64_t top = __atomic_load_n(&s_ink_top, __ATOMIC_RELAXED);
    while (top < gen &&
           !__atomic_compare_exchange_n(&s_ink_top, &top, gen, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        ;
    return __atomic_load_n(&s_ink_base, __ATOMIC_ACQUIRE) + gen;
}

uint64_t TagFSInkedHere(void)
{
    return __atomic_load_n(&s_inked_here[amp_get_core_index()], __ATOMIC_RELAXED);
}

/*
 * ‼ A TALLY GOES A STEP A TURN, BETWEEN THE CYCLES (binder.h, §4.3). Its step
 * is the committer's work too: taken when no cycle is due, and — so a stream
 * of cycles never starves it — the next turn after each cycle ends while one
 * is under way. A waiter's cycle waits for one step at most: a node laid or
 * measured, a DIGESTS leaf read, a flush, the slot — or the slice, which is
 * memory alone, as long as a walk of every live file's runs takes
 * (binder_slice.c). But while a write short of room waits on the Tally under
 * way (binder.h, BinderRoomAwaited) every turn is the Tally's until it ends:
 * the write asks a cycle each round it is told to wait, and a cycle between
 * every two steps was a stitch each on a ribbon a full volume keeps one Quire
 * for — a Tally of two hundred steps took two hundred stitches, the ribbon ran
 * out under it and the delete that would have given the ground back was
 * refused (fullvolume, roomback). Its end asks the cycle that wakes the write
 * (BinderRoomEnded).
 */
static volatile uint8_t s_binder_turn;

/* A mount's first word, with the door shut: no Ink of the volume before it is
 * owed here, and what its medium cost — the Lull, the binding window, the
 * Lull's armed tick — is measured again by this one's first cycle: another
 * medium may sit in the seat. */
void CadenceMountReset(void)
{
    __atomic_store_n(&s_binder_turn, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_owed_since_us, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&s_eager_gen, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_demand, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_last_end_us, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_d_us, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_lull_fire, 0, __ATOMIC_RELAXED);
    s_binds_said = false;
    __atomic_store_n(&s_ink_base,
                     __atomic_load_n(&s_ink_base, __ATOMIC_RELAXED) +
                         __atomic_load_n(&s_ink_top, __ATOMIC_RELAXED) + 1u,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_ink_top, 0, __ATOMIC_RELEASE);
}

void SettleInked(uint64_t gen, bool lazy)
{
    __atomic_store_n(&s_inked_here[amp_get_core_index()], TagFSInkPlace(gen), __ATOMIC_RELAXED);
    uint64_t none = 0;
    (void)__atomic_compare_exchange_n(&s_owed_since_us, &none, SettleNowUs(), false,
                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    if (!lazy) {
        uint64_t seen = __atomic_load_n(&s_eager_gen, __ATOMIC_RELAXED);
        while (seen < gen &&
               !__atomic_compare_exchange_n(&s_eager_gen, &seen, gen, false,
                                            __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            ;
    }
    CadenceWake();
}

void SettleDemand(uint32_t kinds)
{
    if (kinds & SETTLE_DEMAND_TALLY)
        BinderDemanded();
    kinds &= ~(uint32_t)SETTLE_DEMAND_TALLY;
    if (!kinds) {
        CadenceWake();
        return;
    }
    if (kinds & SETTLE_DEMAND_ROOM)
        __atomic_add_fetch(&s_room_asked, 1u, __ATOMIC_ACQ_REL);
    __atomic_fetch_or(&s_demand, kinds, __ATOMIC_SEQ_CST);
    CadenceWake();
}

uint64_t CadenceRoomAsked(void)
{
    return __atomic_load_n(&s_room_asked, __ATOMIC_ACQUIRE);
}

void SettleKnock(void)
{
    CadenceWake();
}

void SettleCadenceHold(bool hold)
{
    __atomic_add_fetch(&s_held, hold ? 1 : -1, __ATOMIC_ACQ_REL);
    if (!hold)
        CadenceWake();
}

uint32_t CadenceRunBegins(void)
{
    return __atomic_exchange_n(&s_demand, 0u, __ATOMIC_SEQ_CST);
}

/* The window binds the Lull once k·D passes it: from there the medium pays more
 * than its share for the promise. Said once an episode, with what it costs. */
static void CadenceSayBinds(uint64_t d_us)
{
    bool binds = (uint64_t)SETTLE_LULL_FACTOR * d_us > SETTLE_WINDOW_US;
    if (binds && !s_binds_said)
        kprintf("[Settle] the medium pays %llu%% of its time for the %llu ms window — one cycle "
                "takes it %llu ms, more than the %u%% share allows\n",
                (unsigned long long)(d_us * 100u / (SETTLE_WINDOW_US + d_us)),
                (unsigned long long)(SETTLE_WINDOW_US / 1000u),
                (unsigned long long)(d_us / 1000u), SETTLE_SHARE);
    s_binds_said = binds;
}

/*
 * The window's clock after a cycle: nothing owed when every Ink is settled,
 * else the changes after S0 are at most as old as S0. The Ink side sets the
 * clock only when it reads nothing owed, after its gen was published; so this
 * clears first and then looks at the gen, and an Ink between the two either is
 * seen here or finds the clock cleared and starts it itself.
 */
void CadenceRunEnded(error_t rc, uint64_t busy_us, uint64_t s0_us, uint64_t target)
{
    uint64_t now = SettleNowUs();
    if (BinderOwed())
        __atomic_store_n(&s_binder_turn, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_last_end_us, now, __ATOMIC_RELEASE);
    if (rc == OK) {
        __atomic_store_n(&s_d_us, busy_us, __ATOMIC_RELAXED);
        CadenceSayBinds(busy_us);
        __atomic_store_n(&s_owed_since_us, 0, __ATOMIC_SEQ_CST);
        if (InkedGen() > target)
            __atomic_store_n(&s_owed_since_us, s0_us, __ATOMIC_SEQ_CST);
    } else if (rc == ERR_TAGFS_NO_SPACE) {
        /* The ribbon refused what the cycle carried, and no park waits on an
         * event for it (settle_wait.c: nothing asks for room): the window
         * starts again from here, so the cadence tries it a window on — a
         * waiter or a demand at once — and never in a loop. */
        __atomic_store_n(&s_owed_since_us, now, __ATOMIC_SEQ_CST);
    }
    CadenceWake();
}

typedef enum {
    CADENCE_REST = 0,       /* nothing owed, or a refusal waits for its event */
    CADENCE_NOW,
    CADENCE_LATER,          /* at *at_us: the Lull's permit, or the window's end */
} CadenceVerdict;

/* Whether a cycle is due. While pockets wait (`boundary`) only the window run
 * out and a demand start one; on an empty queue Lull permits one for an eager
 * change as well. Lock-free. */
static CadenceVerdict CadenceDue(bool boundary, uint64_t now, uint64_t* at_us)
{
    uint64_t settled = SettledGen();
    bool     waiters = SettleWaitersOwed();
    uint32_t demand  = __atomic_load_n(&s_demand, __ATOMIC_ACQUIRE);
    bool     owed    = InkedGen() > settled;

    if (!owed && !waiters && !demand)
        return CADENCE_REST;
    if (SettleParked() || __atomic_load_n(&s_held, __ATOMIC_ACQUIRE) > 0)
        return CADENCE_REST;
    if (waiters || demand)
        return CADENCE_NOW;

    uint64_t since     = __atomic_load_n(&s_owed_since_us, __ATOMIC_SEQ_CST);
    uint64_t window_at = (since ? since : now) + SETTLE_WINDOW_US;
    if (now >= window_at)
        return CADENCE_NOW;
    if (boundary)
        return CADENCE_REST;

    bool     eager  = __atomic_load_n(&s_eager_gen, __ATOMIC_ACQUIRE) > settled;
    uint64_t permit = __atomic_load_n(&s_last_end_us, __ATOMIC_ACQUIRE) + SettleLullUs();
    if (eager && now >= permit)
        return CADENCE_NOW;
    *at_us = (eager && permit < window_at) ? permit : window_at;
    return CADENCE_LATER;
}

/* A step of a Tally owed the committer: not while the cadence is held — the
 * stand's tests meet only the work they drive, and a stopping machine's halt
 * runs the last cycles itself — nor while a refused cycle's park stands. */
static bool CadenceBinderOwed(void)
{
    return __atomic_load_n(&s_held, __ATOMIC_ACQUIRE) == 0 && !SettleParked() && BinderOwed();
}

/* The first tick at or after `at_us`. */
static uint64_t CadenceLullTick(uint64_t at_us, uint64_t now_us)
{
    uint64_t ticks = (at_us - now_us) * SCHEDULER_DEFAULT_TICK_HZ / 1000000ull + 1u;
    return __atomic_load_n(&g_global_tick, __ATOMIC_RELAXED) + ticks;
}

/* The Lull's node, armed for the tick at or after `at_us`: the tick that fires
 * it wakes the committer (CadenceWake), and the committer looks again. */
static void CadenceArmLull(uint64_t at_us, uint64_t now_us)
{
    uint64_t fire = CadenceLullTick(at_us, now_us);
    if (__atomic_load_n(&s_lull_fire, __ATOMIC_RELAXED) == fire)
        return;
    __atomic_store_n(&s_lull_fire, fire, __ATOMIC_RELAXED);
    TouchQueueArmCall(&s_lull_node, CadenceWake, fire);
}

/* Whether the Lull's node is still to fire, and fires by `at_us`: armed to wake
 * the committer for the cycle owed then. One that fires earlier is harmless —
 * the committer looks again and arms it anew. */
static bool CadenceLullArmedFor(uint64_t at_us, uint64_t now_us)
{
    uint64_t fire = __atomic_load_n(&s_lull_fire, __ATOMIC_RELAXED);
    return fire > __atomic_load_n(&g_global_tick, __ATOMIC_RELAXED) &&
           fire <= CadenceLullTick(at_us, now_us);
}

/* A volume a cycle can write: mounted, its medium here, and taking writes. */
static bool CadenceMounted(void)
{
    return tagfs_get_state()->initialized && !TagFSMediumLeft() && !TagFSReadOnly();
}

void SettleServiceIfPending(void)
{
    /* A medium that takes no write answers every waiter for good — on
     * whichever core comes by first. One that left holds them with its open
     * book (mount/away.h), which answers them itself when it is given up. */
    if (SettleWaitersOwed() && TagFSReadOnly() && !TagFSMediumLeft())
        SettleWaitersAnswerAll(ERR_TAGFS_READ_ONLY);

    /* The cycle in flight is stepped by the core that began it, even one that
     * stopped being the committer's meanwhile (the other cores came up). */
    if (SettleRunInFlight()) {
        SettleRunStepHere();        /* nothing unless the cycle is this core's */
        return;
    }
    if (amp_get_core_index() != SettleCommitterCore())
        return;
    if (!CadenceMounted()) {
        /* A Tally under way on a volume that stopped taking writes — a data
         * write refused as protected, say — is ended by its next turn, which
         * writes nothing (binder.h, BinderStopped): its ground goes back. */
        if (BinderHoldsGround())
            (void)BinderTurn();
        return;
    }
    SettleWaitersSweep();

    /* Pockets waiting on this core's queue are work already handed in, and a
     * cycle Lull merely permits stands back for them (settle.h). */
    bool           boundary = kcore_queue_depth(amp_get_core_index()) != 0;
    uint64_t       now      = SettleNowUs();
    uint64_t       at       = 0;
    CadenceVerdict due      = CadenceDue(boundary, now, &at);
    if (CadenceBinderOwed() &&
        (due != CADENCE_NOW || __atomic_load_n(&s_binder_turn, __ATOMIC_ACQUIRE) || BinderRoomAwaited())) {
        if (BinderTurn())
            __atomic_store_n(&s_binder_turn, 0u, __ATOMIC_RELEASE);
        CadenceWake();
        return;
    }
    switch (due) {
    case CADENCE_NOW:
        (void)SettleRunBeginForCadence();
        return;
    case CADENCE_LATER:
        CadenceArmLull(at, now);
        return;
    default:
        return;
    }
}

bool SettleStepOwedHere(void)
{
    if (SettleRunInFlight())
        return SettleRunBetweenStepsHere();
    if (amp_get_core_index() != SettleCommitterCore())
        return false;
    /* A cycle another asker runs wakes the committer when it ends. */
    if (__atomic_load_n(&g_settle_scribe.holder, __ATOMIC_ACQUIRE) != 0 || !CadenceMounted())
        return false;
    if (CadenceBinderOwed())
        return true;
    uint64_t now = SettleNowUs();
    uint64_t at  = 0;
    switch (CadenceDue(false, now, &at)) {
    case CADENCE_NOW:
        return true;
    case CADENCE_LATER:
        /* Owed later, and the node that would wake this core for it is not
         * armed: the service point arms it. An Ink made after this core's
         * service point and before its gate — its wake finding the core not
         * yet asleep — would otherwise be slept over with no Lull to bring
         * the committer back, on a core whose own timer never runs. */
        return !CadenceLullArmedFor(at, now);
    default:
        return false;
    }
}

bool SettleIdleOwed(void)
{
    if (g_amp.total_cores != 1)
        return false;
    if (SettleRunInFlight())
        return true;
    if (!CadenceMounted())
        return false;
    uint64_t at = 0;
    return CadenceBinderOwed() || CadenceDue(true, SettleNowUs(), &at) == CADENCE_NOW;
}

/* Helm work listed for a service point is any core's to run: one that went to
 * sleep over it would leave a write between two windows until something else
 * woke a core — and the knock that should have may have been run by a pump
 * nested inside the very service point that looked at the list (Z07-8). */
bool TagFSStepOwedHere(void)
{
    return HelmReadyListed() || SettleStepOwedHere();
}

/* ‼ ON ONE CORE, HELM WORK MAKES IDLE THE DEBTOR OF THE QUANTUM, AS A CYCLE DOES.
 * A write gives its core back between two windows, and its next one is run by
 * a service point — on one core only the idle context's. Asked about the
 * Settle alone, the scheduler gave idle the core beside a program that never
 * parks only while a cycle was owed, and a write moved in bursts, a window's
 * five seconds apart (X1-2). Listed Helm work owes idle the next quantum too,
 * taken from whoever ran this one: the program has the core back after it, and
 * the two share the core turn about. */
bool TagFSIdleOwed(void)
{
    if (g_amp.total_cores != 1)
        return false;
    return HelmReadyListed() || SettleIdleOwed();
}

void TagFSWatchLook(void)
{
    SettleWatchLook(SettleNowUs());
}

void TagFSNoteInker(process_t* proc, uint64_t place)
{
    uint64_t seen = __atomic_load_n(&proc->volume_ink_place, __ATOMIC_RELAXED);
    while (seen < place &&
           !__atomic_compare_exchange_n(&proc->volume_ink_place, &seen, place, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        ;
}

void TagFSNoteInkerSince(process_t* proc, uint64_t inked_before)
{
    uint64_t inked = TagFSInkedHere();
    if (inked != inked_before)
        TagFSNoteInker(proc, inked);
}

#ifdef CONFIG_VOLUME_TESTS
uint32_t TagFSStandSettleDemands(void)
{
    return __atomic_load_n(&s_demand, __ATOMIC_ACQUIRE);
}
#endif

/* A place below this mount's base is an Ink of a volume before it: its
 * changes went with that volume, and nothing of this one is owed for it. */
void TagFSInkerLeaves(process_t* proc)
{
    uint64_t place = __atomic_load_n(&proc->volume_ink_place, __ATOMIC_ACQUIRE);
    uint64_t base  = __atomic_load_n(&s_ink_base, __ATOMIC_ACQUIRE);
    if (!CadenceMounted() || place <= base || place - base <= SettledGen())
        return;
    SettleDemand(SETTLE_DEMAND_LEAVE);
}

// ----------------------------------------------------------------------------
// Nightwatch's look (RT-11)
// ----------------------------------------------------------------------------

static uint64_t s_watch_since;      /* the debt the last look saw unclaimed */
static uint64_t s_watch_settled;
static bool     s_watch_seen;
static bool     s_watch_said;

static void SettleWatchSay(void* ctx);
static Baton    s_watch_baton = { .next = NULL, .run = SettleWatchSay, .ctx = NULL, .one_way = 0 };
static volatile uint32_t s_watch_passed;

static void SettleWatchSay(void* ctx)
{
    (void)ctx;
    uint64_t since = __atomic_load_n(&s_owed_since_us, __ATOMIC_SEQ_CST);
    uint64_t now   = SettleNowUs();
    kprintf("[Nightwatch] SETTLE UNCLAIMED: changes of the volume owed to the medium for %llu ms, "
            "past the %llu ms window, and no committer took them up across two looks — the "
            "cadence's launch rule did not start the cycle it owed (settled up to Ink %llu, "
            "inked %llu)\n",
            (unsigned long long)(since && now > since ? (now - since) / 1000u : 0),
            (unsigned long long)(SETTLE_WINDOW_US / 1000u),
            (unsigned long long)SettledGen(), (unsigned long long)InkedGen());
    __atomic_store_n(&s_watch_passed, 0u, __ATOMIC_RELEASE);
}

/* The park's own voice: a refused cycle that waiters wait on, parked longer
 * than the window, is waiting for an event that has not come — said once for
 * that park, by a baton, since the look runs in the tick. */
static void SettleWatchParkSay(void* ctx);
static Baton    s_park_baton = { .next = NULL, .run = SettleWatchParkSay, .ctx = NULL,
                                 .one_way = 0 };
static volatile uint32_t s_park_passed;
static uint64_t s_park_said;        /* when the park last said was decided */

static void SettleWatchParkSay(void* ctx)
{
    (void)ctx;
    SettleParkSay();
    __atomic_store_n(&s_park_passed, 0u, __ATOMIC_RELEASE);
}

static void SettleWatchParked(uint64_t now_us)
{
    uint64_t since = SettleParkedSinceUs();
    if (!since || since == s_park_said || !SettleWaitersOwed() || now_us <= since ||
        now_us - since <= SETTLE_WINDOW_US)
        return;
    s_park_said = since;
    if (__atomic_exchange_n(&s_park_passed, 1u, __ATOMIC_ACQ_REL) == 0)
        (void)BatonPass(&s_park_baton);
}

/* From the PIT tick, a look apart: a debt older than the window, no committer
 * holding the Scribe, no refusal parked on its event — and the same debt, not
 * a byte of it settled, at the next look. The first half alone is a cycle about
 * to begin; the second is a launch rule that did not. A park is its own
 * question (SettleWatchParked). */
void SettleWatchLook(uint64_t now_us)
{
    SettleWatchParked(now_us);
    uint64_t since   = __atomic_load_n(&s_owed_since_us, __ATOMIC_SEQ_CST);
    uint64_t settled = SettledGen();
    bool     late    = CadenceMounted() && since && now_us > since &&
                       now_us - since > SETTLE_WINDOW_US && !SettleParked() &&
                       __atomic_load_n(&s_held, __ATOMIC_ACQUIRE) == 0 &&
                       __atomic_load_n(&g_settle_scribe.holder, __ATOMIC_ACQUIRE) == 0;
    if (!late) {
        s_watch_seen = false;
        s_watch_said = false;
        return;
    }
    if (!s_watch_seen || s_watch_since != since || s_watch_settled != settled) {
        s_watch_seen    = true;
        s_watch_since   = since;
        s_watch_settled = settled;
        return;
    }
    if (s_watch_said)
        return;
    s_watch_said = true;
    if (__atomic_exchange_n(&s_watch_passed, 1u, __ATOMIC_ACQ_REL) == 0)
        (void)BatonPass(&s_watch_baton);
}
