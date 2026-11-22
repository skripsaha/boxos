#include "settle_internal.h"
#include "sewing.h"
#include "../scribe/scribe.h"
#include "../ink/ink.h"
#include "../ink/fallow.h"
#include "../keep/keep.h"
#include "../retire/retire.h"
#include "../show/show.h"
#include "../settle_cut/settle_cut.h"
#include "../helm/helm.h"          /* HelmOwed — a change in flight, which no clean seal outruns */
#include "binder.h"               /* BinderRoomAwaited — the Tally a book's take waits on */
#include "amp.h"
#include "baton.h"
#include "kheap.h"
#include "atomics.h"
#include "cpu_calibrate.h"
#ifdef CONFIG_VOLUME_TESTS
#include "boardroom.h"     /* BoardroomNoteSeatReady — the stand's ring inside a cycle */
#endif

/*
 * The Settle cycle (settle.h): a book's stitch, sewn step by step (sewing.c).
 * Its holder is the one committer, and is never switched away from while it
 * holds the Scribe (scribe.h). Between two steps the holder's core is back in
 * its loop, and allocates there as any other work does: the heap's reserve is
 * the steps' alone.
 */
Scribe g_settle_scribe = SCRIBE_INIT(SCRIBE_RANK_SETTLE, "the volume's Settle");

/* Under the state lock. */
static uint64_t s_settled_gen;
static uint64_t s_cycles;        /* cycles settled this mount; the next is s_cycles + 1 */

/* Said once for each run of refused cycles, until one settles. */
static bool     s_failing;

/* Every block the cycles ever gave back, counted under the state lock: what
 * an asker for room judges a cycle by (SettleForRoom). */
static uint64_t s_given_count;

/* Whether the newest settled cycle sealed the volume clean: the halt's last
 * word on the medium, asked by its drain (SettleOwed). */
static volatile bool s_sealed_clean;

/* Steps any cycle took this boot — how the halt's drain sees a cycle on
 * another core going on (SettleStepsTaken). */
static volatile uint64_t s_steps;

/* The cycle in flight: the Scribe holder's alone, but `active` and `core`,
 * which the cadence of every core reads. */
typedef struct {
    volatile uint8_t active;
    volatile uint8_t core;
    bool             in_step;
    bool             door;       /* the cycle holds the volume's door itself */
    SettleWhy        why;
    error_t          answer;     /* how the last cycle ended */
    uint64_t         cover;      /* the Scribe tickets it answers */
    uint64_t         s0_us;
    uint64_t         busy_us;    /* its own time on the medium, step by step */
} SettleRun;

static SettleRun s_run;

uint64_t SettledGen(void)
{
    return __atomic_load_n(&s_settled_gen, __ATOMIC_ACQUIRE);
}

uint64_t SettledCycles(void)
{
    return __atomic_load_n(&s_cycles, __ATOMIC_ACQUIRE);
}

uint64_t SettleNowUs(void)
{
    return cpu_tsc_to_us(rdtsc());
}

/* The refusal of a cycle on a volume that takes no write — under glass, or a
 * book no Quire took a write of — said once a mount. */
static volatile uint8_t s_refusal_said;

void SettleMountReset(void)
{
    SettleWaitersMountReset();
    CadenceMountReset();
    KeepMountReset();
    RetireMountReset();
    ShowMountReset();
    __atomic_store_n(&s_settled_gen, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_cycles, 0, __ATOMIC_RELEASE);
    s_failing = false;
    SettleHaltMountReset();
    StitchMountReset();
    __atomic_store_n(&s_sealed_clean, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_refusal_said, 0u, __ATOMIC_RELEASE);
}

void SettleOwed(uint64_t* unsettled, bool* committer_busy, bool* sealed_clean)
{
    if (!tagfs_get_state()->initialized) {
        *unsettled      = 0;
        *committer_busy = false;
        *sealed_clean   = true;
        return;
    }
    /* A volume under glass is on the medium as it was read: no cycle owes it
     * a seal, and the halt asks for none (TagFSSettleForHalt). */
    *sealed_clean = __atomic_load_n(&s_sealed_clean, __ATOMIC_ACQUIRE) || TagFSIsUnderGlass();
    uint64_t inked   = InkedGen();
    uint64_t settled = SettledGen();
    uint32_t holder  = __atomic_load_n(&g_settle_scribe.holder, __ATOMIC_ACQUIRE);
    *unsettled       = inked > settled ? inked - settled : 0;
    /* A holder whose core is no longer running is a cycle abandoned, not one
     * running: the halt's own cycle takes it over (ScribeWaitTakingOver). */
    *committer_busy  = holder != 0 && holder != (uint32_t)amp_get_core_index() + 1u &&
                       holder - 1u < g_amp.total_cores &&
                       amp_core_online(&g_amp.cores[holder - 1u]);
}

uint64_t SettleStepsTaken(void)
{
    return __atomic_load_n(&s_steps, __ATOMIC_ACQUIRE);
}

void SettleSettledLocked(uint64_t target, uint64_t cycle, bool clean)
{
    __atomic_store_n(&s_settled_gen, target, __ATOMIC_RELEASE);
    __atomic_store_n(&s_cycles, cycle, __ATOMIC_RELEASE);
    __atomic_store_n(&s_sealed_clean, clean, __ATOMIC_RELEASE);
}

void SettleGivenLocked(uint64_t given)
{
    s_given_count += given;
}

#ifdef CONFIG_VOLUME_TESTS
/* The stand's turns of the next cycle: its seat rings right before it takes its
 * count, or right after its TAKE (sewing.c); its stitch answers a refusal of
 * the stand's choosing. */
enum { SETTLE_STAND_RINGS_BEFORE = 1, SETTLE_STAND_RINGS_AFTER };
static volatile uint8_t s_stand_rings;
static volatile error_t s_stand_refusal;

void TagFSStandSettleRings(bool before_its_count)
{
    __atomic_store_n(&s_stand_rings,
                     (uint8_t)(before_its_count ? SETTLE_STAND_RINGS_BEFORE
                                                : SETTLE_STAND_RINGS_AFTER),
                     __ATOMIC_RELEASE);
}

void TagFSStandSettleRefuses(error_t code)
{
    __atomic_store_n(&s_stand_refusal, code, __ATOMIC_RELEASE);
}

/* seat:ready noted of the volume's seat, as a driver's recovery notes it while
 * a cycle runs — when the stand chose: just before the cycle takes the count
 * its refusal is judged by, or after, before anything it decides. Noted, not
 * said: the room's pass says it later. */
void SettleStandRingsHere(bool before_its_count)
{
    uint8_t chosen = before_its_count ? SETTLE_STAND_RINGS_BEFORE : SETTLE_STAND_RINGS_AFTER;
    if (!__atomic_compare_exchange_n(&s_stand_rings, &chosen, 0u, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    uint8_t seat = tagfs_get_seat();
    BoardroomNoteSeatReady(BoardroomSeatKind(seat), BoardroomSeatIndex(seat));
}

/* The step's answer as the stand chose it, once — none of the medium's: the
 * stitch answers it before it is made (sewing.c, STITCH_STEP_BUILD). */
error_t SettleStandRefusalHere(error_t rc)
{
    error_t chosen = __atomic_exchange_n(&s_stand_refusal, OK, __ATOMIC_ACQ_REL);
    return rc == OK ? chosen : rc;
}
#endif

// ----------------------------------------------------------------------------
// The cycle in flight
// ----------------------------------------------------------------------------

/* A cycle the caller just took the Scribe for, begun at its stitch's TAKE
 * (sewing.c).
 *
 * Once the halt has shut the volume, a cycle anybody else begins is one of its
 * cycles too. One the cadence had already decided on, or an asker for room on
 * another core, can take the Scribe right after the halt's ask, and the halt's
 * ask is then answered by that cycle: begun as itself, it would seal the
 * volume unclean after the halt's last word. */
static void SettleRunBegin(SettleWhy why, uint64_t cover, bool door)
{
    if (SettleHalting())
        why = SETTLE_FOR_HALT;
    s_run.why     = why;
    s_run.door    = door;
    s_run.cover   = cover;
    s_run.answer  = OK;
    s_run.busy_us = 0;
    s_run.s0_us   = SettleNowUs();
    s_run.in_step = false;
    __atomic_store_n(&s_run.core, amp_get_core_index(), __ATOMIC_RELEASE);
    __atomic_store_n(&s_run.active, 1u, __ATOMIC_RELEASE);
    StitchRunBegin(why);

    /* The Scribe's MINE opened the heap's reserve for the pass; between the
     * steps it is closed, each step opens it again, and the last step's is
     * the one the Give closes. */
    KHeapReserveClose();
}

/*
 * The end of the cycle in flight, settled or refused: the Scribe answers its
 * askers, the cadence learns what it cost, and its waiters are answered — or,
 * refused, left to wait for the event its refusal names (SettleWaitersPark).
 * What the end needs is read out before the Give: from the Give on, the next
 * holder may be beginning its own cycle in s_run.
 */
static void SettleRunEnd(error_t rc)
{
    bool     door    = s_run.door;
    uint64_t target  = StitchRunTarget();
    uint64_t busy_us = s_run.busy_us;
    uint64_t s0_us   = s_run.s0_us;

    if (rc != OK) {
        if (!s_failing)
            kprintf("[Settle] the cycle that would have settled %llu change(s) was refused at "
                    "%s (error %u) — nothing of it is declared settled, and the next cycle "
                    "writes it all again\n",
                    (unsigned long long)(target > SettledGen() ? target - SettledGen() : 0u),
                    StitchRunStage(), (unsigned)rc);
        s_failing = true;
        StitchRunEnd(rc);
        TagFSWriteRefused();
    } else {
        SETTLE_CUT_AT(SETTLE_CUT_POINT_DONE);
        StitchRunEnd(OK);
        s_failing = false;
    }

    /* Its park before its Give: from the Give on the next cycle may end, and
     * a park this one left after it would stand on that cycle's answer. */
    SettleWaitersPark(rc);
    __atomic_store_n(&s_run.active, 0u, __ATOMIC_RELEASE);
    ScribeGive(&g_settle_scribe, s_run.cover, rc);

    CadenceRunEnded(rc, busy_us, s0_us, target);
    SettleWaitersRunEnded(rc, target);
    if (door)
        tagfs_leave();
}

/* One step of the cycle in flight on this core, with the heap's reserve open
 * for it alone: true, and *answer, when that was its last. */
static bool SettleRunStep(error_t* answer)
{
    uint64_t t0   = SettleNowUs();
    bool     done = false;
    KHeapReserveOpen();
    s_run.in_step = true;
    error_t rc = StitchRunStep(&done);
    s_run.in_step = false;
    s_run.busy_us += SettleNowUs() - t0;
    __atomic_add_fetch(&s_steps, 1u, __ATOMIC_RELEASE);

    if (rc != OK || done) {
        *answer = rc;
        SettleRunEnd(rc);           /* its Give closes the reserve this step opened */
        return true;
    }
    KHeapReserveClose();
    return false;
}

/* The cycle in flight run to its end here, by its holder; its answer. */
static error_t SettleRunDrive(void)
{
    error_t answer = OK;
    while (!SettleRunStep(&answer))
        ;
    return answer;
}

bool SettleRunInFlight(void)
{
    return __atomic_load_n(&s_run.active, __ATOMIC_ACQUIRE) != 0;
}

bool SettleRunBetweenStepsHere(void)
{
    return SettleRunInFlight() && !s_run.in_step &&
           __atomic_load_n(&s_run.core, __ATOMIC_ACQUIRE) == amp_get_core_index() &&
           ScribeHeldHere(&g_settle_scribe);
}

void SettleRunStepHere(void)
{
    error_t answer;
    if (SettleRunBetweenStepsHere())
        (void)SettleRunStep(&answer);
}

/*
 * ‼ NO CYCLE RUNS ON A VOLUME UNDER GLASS, NOR ON A BOOK THAT TAKES NO WRITE.
 * A volume under glass takes no write at all (tagfs.h), and a book whose
 * refused write no reader would take up again is said to be only read from
 * there (sewing.c, StitchRefused): each road that would begin a cycle — the
 * cadence's, an asker's, the halt's — is refused here, before the Scribe is
 * asked, so nothing waits for a turn that would write, and nothing is written
 * where the kernel said nothing would be. Every other volume's cycle is a
 * stitch (sewing.c).
 */
static error_t SettleMayRun(void)
{
    bool book = StitchBookRefused();
    if (!book && !TagFSIsUnderGlass())
        return OK;
    if (!__atomic_exchange_n(&s_refusal_said, 1u, __ATOMIC_ACQ_REL))
        kprintf("[Settle] no cycle runs: %s — every cycle asked of it is refused before the Scribe, "
                "ERR_TAGFS_READ_ONLY\n", book ? "the book takes no write (said when its last refusal could not be "
                "taken again)" : "the volume may only be read (said at its mount)");
    return ERR_TAGFS_READ_ONLY;
}

/* A cycle the cadence begins holds the volume's door from its TAKE to its end: the
 * volume it writes is the one mounted until it is done, whoever else leaves. */
bool SettleRunBeginForCadence(void)
{
    uint64_t cover   = 0;
    error_t  refused = OK;
    if (!tagfs_enter())
        return false;
    if (SettleMayRun() != OK) {
        tagfs_leave();
        return false;
    }
    uint64_t ticket = ScribeAsk(&g_settle_scribe);
    if (ScribeTry(&g_settle_scribe, ticket, &cover, &refused) != SCRIBE_MINE) {
        tagfs_leave();
        return false;
    }
    SettleRunBegin(SETTLE_FOR_CADENCE, cover, true);
    return true;
}

#ifdef CONFIG_VOLUME_TESTS
error_t TagFSSettleAsCadence(void)
{
    return SettleRunBeginForCadence() ? SettleRunDrive() : ERR_BUSY;
}

bool TagFSStandSettleBegin(void)
{
    return SettleRunBeginForCadence();
}
#endif

// ----------------------------------------------------------------------------
// Askers who wait
// ----------------------------------------------------------------------------

/* A cycle that began after the ask: this call's own, or one another core was
 * already making. A cycle this very core holds between two of its steps is
 * run to its end here first — waiting for it would be waiting for ourselves —
 * and then the ask is put again: that cycle took its snapshot before it. */
error_t SettleAskedFor(SettleWhy why)
{
    error_t may = SettleMayRun();
    if (may != OK)
        return may;
    uint64_t ticket = ScribeAsk(&g_settle_scribe);
    for (;;) {
        uint64_t   cover = 0;
        error_t    rc    = OK;
        ScribeTurn turn  = why == SETTLE_FOR_HALT
                               ? ScribeWaitTakingOver(&g_settle_scribe, ticket, &cover, &rc)
                               : ScribeWait(&g_settle_scribe, ticket, &cover, &rc);
        switch (turn) {
        case SCRIBE_MINE:
            SettleRunBegin(why, cover, false);
            return SettleRunDrive();
        case SCRIBE_SELF:
            if (SettleRunBetweenStepsHere()) {
                (void)SettleRunDrive();
                continue;
            }
            kprintf("[Settle] a cycle was asked for on a core that holds one beneath the ask — "
                    "waiting for it here would never end\n");
            return ERR_BUSY;
        case SCRIBE_REFUSED:
            return rc;
        default:
            return OK;          /* SCRIBE_WRITTEN */
        }
    }
}

error_t TagFSSettle(void)
{
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    error_t rc = SettleAskedFor(SETTLE_FOR_WAITER);
    tagfs_leave();
    return rc;
}

/*
 * ‼ ROOM IS JUDGED BY WHAT CAME BACK (F01-3), never by what still rests:
 * deletes beside the asker put ground in Fallow as fast as a cycle gives some
 * back, and a cycle on another core may give all of it back between the
 * allocator's refusal and the first look here. A cycle answers by the ground
 * the cycles gave back (s_given_count), or the free count risen.
 *
 * A book's room (§3.4, §3.6, В1): ground a stitch gave up comes back once the
 * ribbon left the Quire the stitch lies in and the lag's stitches settled past
 * it. A cycle asked for room closes that Quire early when Fallow waits for it
 * (sewing.c, StitchTake) — its stitch goes on at the start of the next_quire
 * the last stitch names — and the lag's cycles after it give the ground back:
 * as many rounds as that takes. Room on the ribbon is a cycle's to make even
 * with nothing resting — the claims in flight settled, a next_quire asked for
 * (settle/ribbon.c, RibbonClaim) — so one cycle is always run before the take
 * is asked again, which says the last word itself. A take that waits on a
 * Tally (binder.h, BinderRoomWait) has it written here first, by its own
 * writer, a step a turn of the Scribe — not left to the committer while its
 * cycles turn empty, each a stitch on a ribbon a full volume has one Quire to
 * spare for — and the ground the slot let go rests in Fallow for the rounds
 * after. One the tails hold back is left to the Helm's service
 * (BinderRunAwaited): this wait is reached from inside a Helm span, and an
 * Engross here would walk the files under it.
 */
error_t SettleForRoom(void)
{
    TagFSState* fs  = tagfs_get_state();
    bool        own = SettleRunBetweenStepsHere();
    error_t     may = SettleMayRun();
    if (may != OK)
        return may;

    /* A caller under a TagFS lock, a Scribe or a baton continuation may not
     * wait for a cycle: the room stays on its way, and the next cycle is asked
     * for. But the Settle's own holder, between two steps of its cycle, runs
     * that cycle to its end first (SettleAskedFor) — waiting for it would be
     * waiting for itself. */
    if (LockRankAnyHeldHere() || BatonRunningHere() || (ScribeAnyHeldHere() && !own)) {
        SettleDemand(SETTLE_DEMAND_ROOM);
        return ERR_WOULD_BLOCK;
    }
    if (BinderRoomAwaited())
        (void)BinderRunAwaited();
    for (uint64_t round = 0; round <= FallowLag() + 1u; round++) {
        TagFSLockTake(&fs->lock);
        uint64_t resting = FallowBlocks();
        uint64_t given   = s_given_count;
        uint64_t had     = fs->free_blocks;
        TagFSLockDrop(&fs->lock);
        error_t rc = SettleAskedFor(SETTLE_FOR_ROOM);
        if (rc != OK)
            return rc;
        TagFSLockTake(&fs->lock);
        bool came_back = s_given_count != given || fs->free_blocks > had;
        TagFSLockDrop(&fs->lock);
        if (came_back || resting == 0)
            return OK;
    }
    return ERR_TAGFS_NO_SPACE;
}
