#include "settle_internal.h"
#include "../scribe/scribe.h"
#include "../ink/ink.h"
#include "../ink/fallow.h"
#include "../metadata_pool/meta_pool.h"
#include "../file_table/file_table.h"
#include "../tag_registry/tag_registry.h"
#include "../integrity/integrity.h"
#include "../settle_cut/settle_cut.h"
#include "../ledger/ledger.h"
#include "../use_memory/use_memory.h"
#include "../glean/glean.h"
#include "../mount/away.h"
#include "amp.h"
#include "baton.h"
#include "kheap.h"
#include "atomics.h"
#include "cpu_calibrate.h"
#ifdef CONFIG_VOLUME_TESTS
#include "boardroom.h"     /* BoardroomNoteSeatReady — the stand's ring inside a cycle */
#endif

/*
 * The Settle cycle (settle.h). Its holder is the one committer, and is never
 * switched away from while it holds the Scribe (scribe.h); everything below
 * that is not S0 or S6 runs with no TagFS lock held. Between two steps the
 * holder's core is back in its loop, and allocates there as any other work
 * does: the heap's reserve is the steps' alone.
 */
Scribe g_settle_scribe = SCRIBE_INIT(SCRIBE_RANK_SETTLE, "the volume's Settle");

typedef enum {
    SETTLE_FOR_WAITER = 1,   /* a kernel path asks and waits for what it settles */
    SETTLE_FOR_ROOM,         /* the volume is short while Fallow rests blocks */
    SETTLE_FOR_HALT,         /* the machine is stopping: the last cycle */
    SETTLE_FOR_CADENCE,      /* Lull permitted, or the window ran out, or a demand */
} SettleWhy;

typedef enum {
    SETTLE_STEP_GLEAN = 0,       /* before S0: Glean's pass (glean.h) */
    SETTLE_STEP_TAKE,            /* S0 */
    SETTLE_STEP_REGISTRY,        /* S1 */
    SETTLE_STEP_MAP,
    SETTLE_STEP_TABLE,
    SETTLE_STEP_HEAD,            /* the pool's head, named again past the chain's front */
    SETTLE_STEP_INTEGRITY,
    SETTLE_STEP_USE_CHAIN,       /* the Use Context's new chain, the last of S1 */
    SETTLE_STEP_MAP_LAY,         /* pages of an integrity map being laid (RT-15) */
    SETTLE_STEP_FIRST_FLUSH,     /* S2 */
    SETTLE_STEP_POOL,            /* S3, a block a turn */
    SETTLE_STEP_LEDGER,          /* S4 */
    SETTLE_STEP_SPARES,          /* the reserve made again, its stamps for S5 to keep */
    SETTLE_STEP_LAST_FLUSH,      /* S5 */
    SETTLE_STEP_DONE,            /* S6 */
} SettleStep;

/* Under the state lock. */
static uint64_t s_settled_gen;
static uint64_t s_cycles;        /* cycles settled this mount; the next is s_cycles + 1 */

/* Said once for each run of refused cycles, until one settles. */
static bool     s_failing;

/* Said once for each run of cycles that could not make the reserve whole,
 * until one could. The Settle's holder's alone. */
static bool     s_spares_short;

/* The cycles in a row whose S1 could not write the tag registry: the first is
 * said, and how many they were once a cycle writes it again. The Settle's
 * holder's alone. */
static uint64_t s_registry_refused;

/* Blocks the last settled cycle's S6 gave back: the map carries them to the
 * medium at the next cycle's S1. The Settle's holder's alone. And every block
 * the S6 of a cycle ever gave back, counted under the state lock: what an
 * asker for room judges a cycle by (SettleForRoom). */
static uint64_t s_given;
static uint64_t s_given_count;

/* The halt's last cycle, for tagfs_shutdown. */
static bool     s_halt_ran;
static error_t  s_halt_answer;

/* Where the last settled seal's pool span ended — the next seal's pool_from —
 * and whether this volume's seals say its medium once held less than a seal
 * did. The Settle's holder's, and the mount's with the door shut. */
static uint32_t s_sealed_block;
static uint32_t s_sealed_bytes;
static bool     s_medium_lied;

typedef struct {
    PoolSnap      pool;
    GleanHead     head;
    SettleMark    mark;         /* what the cycle leaves on the medium, for S4 */
    uint64_t      target;
    uint64_t      cycle;
    uint64_t      data_need;
    MetaPoolPage* given;        /* the halt's Spares, given back at S0 */
    const char*   stage;        /* where a refusal came from, for the one line */
    bool          pool_began;
} SettleSnap;

/* The cycle in flight: the Scribe holder's alone, but `active` and `core`,
 * which the cadence of every core reads. */
typedef struct {
    volatile uint8_t active;
    volatile uint8_t core;
    bool             in_step;
    bool             door;       /* the cycle holds the volume's door itself */
    SettleWhy        why;
    SettleStep       step;
    error_t          answer;     /* how the last cycle ended */
    uint64_t         cover;      /* the Scribe tickets it answers */
    uint64_t         s0_us;
    uint64_t         busy_us;    /* its own time on the medium, step by step */
    SettleSnap       snap;
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

void SettleMountReset(void)
{
    SettleWaitersMountReset();
    CadenceMountReset();
    s_given = 0;
    __atomic_store_n(&s_settled_gen, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_cycles, 0, __ATOMIC_RELEASE);
    s_failing          = false;
    s_spares_short     = false;
    s_registry_refused = 0;
    s_halt_ran         = false;
}

void SettleMountStand(uint32_t from_block, uint32_t from_bytes, bool medium_lied)
{
    s_sealed_block = from_block;
    s_sealed_bytes = from_bytes;
    __atomic_store_n(&s_medium_lied, medium_lied, __ATOMIC_RELEASE);
}

bool SettleMediumLied(void)
{
    return __atomic_load_n(&s_medium_lied, __ATOMIC_ACQUIRE);
}

void SettleOwed(uint64_t* unsettled, bool* committer_busy)
{
    if (!tagfs_get_state()->initialized) {
        *unsettled      = 0;
        *committer_busy = false;
        return;
    }
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

error_t SettleHaltAnswer(uint64_t* unsettled)
{
    uint64_t inked   = InkedGen();
    uint64_t settled = SettledGen();
    *unsettled       = inked > settled ? inked - settled : 0;
    return s_halt_ran ? s_halt_answer : ERR_TAGFS_METADATA_ERROR;
}

// ----------------------------------------------------------------------------
// The steps
// ----------------------------------------------------------------------------

/*
 * S0: memory only, under the state lock, nothing asked of the heap.
 *
 * The seal is taken here, of the same instant as the pool it describes: the
 * pool's part as it stands once S3 has written what was just taken, and the
 * registry's as far as its last write landed — every tag a record in this
 * snapshot wears was on the medium before the record was inked
 * (TagRegistryFlushFor), so a registry the cycle's own S1 lengthens is past
 * the seal and names nothing the seal holds. The halt's cycles seal clean:
 * the machine stops after them.
 *
 * A cycle somebody asked for and waits on closes the tail onto a Spare, and so
 * does one that must give Fallow's ground back; one the cadence began closes
 * it only as far as a waiter is owed. Each leaves the deletes their share of
 * the reserve, but the halt's, which gives every Spare back right after.
 */
static void SettleTake(SettleSnap* snap, SettleWhy why, uint32_t demands)
{
    bool     closes     = why != SETTLE_FOR_CADENCE || (demands & SETTLE_DEMAND_ROOM);
    uint64_t owed_close = closes ? 0 : SettleWaitersCloseUpto();

    TagFSState* fs = tagfs_get_state();
    TagFSLockTake(&fs->lock);
    snap->target    = InkedGen();
    snap->cycle     = s_cycles + 1;
    snap->data_need = InkDataNeed();
    PoolSnapTake(&snap->pool,
                 closes ? snap->target : (owed_close < snap->target ? owed_close : snap->target),
                 why == SETTLE_FOR_HALT);
    if (why == SETTLE_FOR_HALT) {
        snap->given = PoolSparesGiveBack();
        IntegrityLayDrop();
    } else {
        (void)IntegrityLayName();
    }
    PoolSealTake(&snap->mark);
    GleanHeadTake(&snap->head);
    TagRegistryLanded(fs->registry, &snap->mark);
    snap->mark.pool_from_block = s_sealed_block;
    snap->mark.pool_from_bytes = s_sealed_bytes;
    snap->mark.flags           = (why == SETTLE_FOR_HALT ? SETTLE_CLEAN : 0u) |
                                 (s_medium_lied ? SETTLE_MEDIUM_LIED : 0u);
    LedgerImageTake();
    UseMemoryTake();
    FallowLedgerTaken(snap->cycle);
    TagFSLockDrop(&fs->lock);
}

/* S2: the first flush, when the cycle names data, tags or a Use Context chain
 * a kept flush does not yet cover — and a closed block's link names a Spare
 * whose stamp it must cover. A need already kept costs nothing
 * (VolumeFlushFor); a chain the cycle wrote is new blocks, and always asks. */
static error_t SettleFirstFlush(SettleSnap* snap)
{
    uint64_t need = snap->data_need;
    uint64_t reg  = tag_registry_written_need(tagfs_get_state()->registry);
    uint64_t use  = UseMemoryLinkNeed();
    uint64_t lay  = IntegrityLayNeed();
    if (reg > need)
        need = reg;
    if (use > need)
        need = use;
    if (lay > need)
        need = lay;
    if (snap->pool.link_need > need)
        need = snap->pool.link_need;
    if (need == 0)
        return OK;
    return VolumeFlushFor(need);
}

/*
 * ‼ THE RESERVE IS MADE INSIDE THE CYCLE, BEFORE ITS LAST FLUSH. A Spare is a
 * write — its stamp — and a write the last flush did not keep leaves the
 * volume's book in doubt (scribe.h, VolumeWritesKept): a medium taken out and
 * put back exactly as it was left would be given up, every waiter answered
 * that the medium left. So the reserve is made whole twice in a cycle, as its
 * own steps under the heap's reserve: before S0, from the ground the last
 * cycle gave back, so the early close has a Spare to close onto; and after
 * S4, in place of what the close took, for S5 to keep. The halt makes none:
 * its S0 gives every Spare back. A refusal ends nothing — the deletes stand on
 * what the pool holds, and the next cycle tries again — and is said once
 * until a cycle makes the reserve whole.
 */
static void SettleSparesMake(void)
{
    error_t rc = PoolSpareRefill(SPARE_RESERVE);
    if (rc == OK) {
        s_spares_short = false;
        return;
    }
    if (!s_spares_short) {
        TagFSState* fs = tagfs_get_state();
        TagFSLockTake(&fs->lock);
        uint32_t held    = PoolSparesHeld(NULL, 0);
        uint64_t room    = fs->ledger.free_blocks;
        uint64_t resting = FallowBlocks();
        uint64_t cycle   = s_cycles + 1;
        TagFSLockDrop(&fs->lock);
        kprintf("[Settle] cycle %llu could make the pool only %u of the %u Spare(s) a delete "
                "stands on (error %u; %llu block(s) free, %llu resting in Fallow) — a delete "
                "past the tail's room waits for one, and every cycle tries again\n",
                (unsigned long long)cycle, held, (unsigned)SPARE_RESERVE, (unsigned)rc,
                (unsigned long long)room, (unsigned long long)resting);
    }
    s_spares_short = true;
}

#ifdef CONFIG_VOLUME_TESTS
/* The stand's turns of the next cycle: its seat rings right before it takes its
 * count, or right after its S0; its block map answers a refusal of the stand's
 * choosing. */
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
 * a cycle runs — `when` the stand chose: just before the cycle takes the count
 * its refusal is judged by, or after, before anything it decides. Noted, not
 * said: the room's pass says it later. */
static void SettleStandRingsHere(uint8_t when)
{
    uint8_t chosen = when;
    if (!__atomic_compare_exchange_n(&s_stand_rings, &chosen, 0u, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    uint8_t seat = tagfs_get_seat();
    BoardroomNoteSeatReady(BoardroomSeatKind(seat), BoardroomSeatIndex(seat));
}

/* The block map's answer as the stand chose it, once — none of the medium's:
 * the map wrote what it owed, and the step answers the stand's code. */
static error_t SettleStandRefusalHere(error_t rc)
{
    error_t chosen = __atomic_exchange_n(&s_stand_refusal, OK, __ATOMIC_ACQ_REL);
    return rc == OK ? chosen : rc;
}
#endif

/* S0's step: the reserve made whole for the early close — S0 itself takes
 * nothing from the heap or the medium; a volume with no Spare it may close
 * onto keeps its tail open, and the cycle writes it as it stands — the Use
 * Context's new chain staked, and S0 itself, whose time the window is
 * measured from. */
static void SettleTakeStep(SettleSnap* snap)
{
    if (s_run.why != SETTLE_FOR_HALT)
        SettleSparesMake();
    UseMemoryStake();
#ifdef CONFIG_VOLUME_TESTS
    SettleStandRingsHere(SETTLE_STAND_RINGS_BEFORE);
#endif
    SettleWaitersRunBegins();
    SettleTake(snap, s_run.why, CadenceRunBegins());
    s_run.s0_us = SettleNowUs();
#ifdef CONFIG_VOLUME_TESTS
    SettleStandRingsHere(SETTLE_STAND_RINGS_AFTER);
#endif
}

/*
 * S1's registry. Its refusal ends nothing: no record wears a tag the medium
 * does not define — an Ink takes none that is not on it (InkNameTagIntern,
 * TagRegistryFlushFor) — so what it could not write is only the tags nobody
 * wears yet, and the next cycle writes them. A refusal by the medium is the
 * seat's like any other — the seat rings seat:ready on the first command it
 * answers after (boardroom.h) — and the cycle goes on. Said for the first
 * cycle of a run of them, and counted until one writes it.
 */
static void SettleRegistryWrite(uint64_t cycle)
{
    error_t rc = tag_registry_flush(tagfs_get_state()->registry);
    if (rc == OK) {
        if (s_registry_refused)
            kprintf("[Settle] cycle %llu wrote the tag registry, after %llu cycle(s) that went "
                    "on without its newest tags\n",
                    (unsigned long long)cycle, (unsigned long long)s_registry_refused);
        s_registry_refused = 0;
        return;
    }
    if (s_registry_refused++ == 0)
        kprintf("[Settle] cycle %llu goes on without the tag registry's newest tags — their "
                "write was refused (error %u); no record wears a tag the medium does not "
                "define, and every cycle writes them again\n",
                (unsigned long long)cycle, (unsigned)rc);
}

/* One step, and the step after it. A refusal ends the cycle, and names its
 * stage; the steps whose refusal ends nothing answer OK and say their own. */
static error_t SettleStepRun(SettleSnap* snap, SettleStep* step)
{
    error_t rc = OK;
    bool    done;

    switch (*step) {
    case SETTLE_STEP_GLEAN:
        /* What it moves and takes off, this very cycle's S0 seals. */
        GleanStep(&done);
        if (done)
            *step = SETTLE_STEP_TAKE;
        return OK;
    case SETTLE_STEP_TAKE:
        SettleTakeStep(snap);
        SETTLE_CUT_CYCLE(snap->target);
        *step = SETTLE_STEP_REGISTRY;
        return OK;
    case SETTLE_STEP_REGISTRY:
        SETTLE_CUT_AT(SETTLE_CUT_POINT_REGISTRY);
        SettleRegistryWrite(snap->cycle);
        *step = SETTLE_STEP_MAP;
        return OK;
    case SETTLE_STEP_MAP:
        SETTLE_CUT_AT(SETTLE_CUT_POINT_BITMAP);
        rc          = BlockMapFlush(snap->target);
#ifdef CONFIG_VOLUME_TESTS
        rc          = SettleStandRefusalHere(rc);
#endif
        snap->stage = "the block map";
        *step       = SETTLE_STEP_TABLE;
        break;
    case SETTLE_STEP_TABLE:
        /* Rows past the chain that found no ground or memory to grow it by
         * wait in memory for a later cycle (file_table.h): under its seal the
         * pool names them, and the mount derives them from it. */
        SETTLE_CUT_AT(SETTLE_CUT_POINT_TABLE);
        rc          = file_table_flush();
        snap->stage = "the file table";
        *step       = SETTLE_STEP_HEAD;
        break;
    case SETTLE_STEP_HEAD:
        GleanHeadStep(&snap->head, &done);
        if (done)
            *step = SETTLE_STEP_INTEGRITY;
        return OK;
    case SETTLE_STEP_INTEGRITY:
        SETTLE_CUT_AT(SETTLE_CUT_POINT_INTEGRITY);
        FallowZeroDigests(snap->cycle);
        rc = IntegrityFlush();
        snap->stage = "the integrity map";
        *step       = SETTLE_STEP_USE_CHAIN;
        break;
    case SETTLE_STEP_USE_CHAIN:
        SETTLE_CUT_AT(SETTLE_CUT_POINT_USECHAIN);
        rc          = UseMemoryWriteLinks();
        snap->stage = "the Use Context's chain";
        *step       = SETTLE_STEP_MAP_LAY;
        break;
    case SETTLE_STEP_MAP_LAY:
        /* For no longer than the last cycle took: the lay's share of the medium
         * is the cycle's own. Its refusal is its own, and ends nothing here. */
        IntegrityLayStep(SettleLastDUs());
        *step = SETTLE_STEP_FIRST_FLUSH;
        return OK;
    case SETTLE_STEP_FIRST_FLUSH:
        rc          = SettleFirstFlush(snap);
        snap->stage = "the flush before the pool";
        *step       = SETTLE_STEP_POOL;
        break;
    case SETTLE_STEP_POOL:
        if (!snap->pool_began) {
            snap->pool_began = true;
            SETTLE_CUT_AT(SETTLE_CUT_POINT_POOL);
        }
        rc          = PoolSnapWriteNext(&snap->pool, &done);
        snap->stage = "the metadata pool";
        if (rc == OK && done)
            *step = SETTLE_STEP_LEDGER;
        break;
    case SETTLE_STEP_LEDGER:
        SETTLE_CUT_AT(SETTLE_CUT_POINT_LEDGER);
        rc          = LedgerImageWrite(&snap->mark);
        UseMemoryLedgerWritten();
        snap->stage = "the ledger";
        *step       = SETTLE_STEP_SPARES;
        break;
    case SETTLE_STEP_SPARES:
        if (s_run.why != SETTLE_FOR_HALT)
            SettleSparesMake();
        *step = SETTLE_STEP_LAST_FLUSH;
        return OK;
    case SETTLE_STEP_LAST_FLUSH:
        SETTLE_CUT_AT(SETTLE_CUT_POINT_FLUSH);
        rc = VolumeFlushFor(VolumeFlushNeed());
        snap->stage = "the flush after the ledger";
        *step       = SETTLE_STEP_DONE;
        break;
    case SETTLE_STEP_DONE:
        break;
    }
    return rc;
}

/* S6, under the state lock: the target settled, the written closed blocks out
 * of memory, the copy of the Ledger written naming the Use Context's chain it
 * was given — and the one a medium that leaves is held to (LedgerSettled) —
 * and Fallow's ripe ground back to the allocator. */
static void SettleDone(SettleSnap* snap)
{
    TagFSState*   fs    = tagfs_get_state();
    MetaPoolPage* spent = NULL;
    FallowNode*   nodes = NULL;

    TagFSLockTake(&fs->lock);
    uint64_t closed = PoolSnapSettled(&snap->pool, &spent);
    __atomic_store_n(&s_settled_gen, snap->target, __ATOMIC_RELEASE);
    __atomic_store_n(&s_cycles, snap->cycle, __ATOMIC_RELEASE);
    s_sealed_block = snap->mark.pool_to_block;
    s_sealed_bytes = snap->mark.pool_to_bytes;
    UseMemorySettled();
    LedgerSettled();
    GleanHeadSettled(&snap->head, snap->cycle, snap->mark.chain_first);
    s_given = FallowRipen(closed, snap->cycle, &nodes);
    s_given_count += s_given;
    bool map_stood = IntegrityLayStands();
    TagFSLockDrop(&fs->lock);

    PoolPagesFree(spent);
    FallowNodesFree(nodes);
    if (map_stood)
        IntegrityLaySay();
}

// ----------------------------------------------------------------------------
// The cycle in flight
// ----------------------------------------------------------------------------

/* A cycle the caller just took the Scribe for, begun: Glean's pass first — a
 * machine stopping gleans nothing, since ground it took off now would rest
 * past the last cycle — and then S0, each a step of its own. */
static void SettleRunBegin(SettleWhy why, uint64_t cover, bool door)
{
    memset(&s_run.snap, 0, sizeof(s_run.snap));
    s_run.why     = why;
    s_run.door    = door;
    s_run.cover   = cover;
    s_run.step    = why == SETTLE_FOR_HALT ? SETTLE_STEP_TAKE : SETTLE_STEP_GLEAN;
    s_run.answer  = OK;
    s_run.busy_us = 0;
    s_run.s0_us   = SettleNowUs();
    s_run.in_step = false;
    __atomic_store_n(&s_run.core, amp_get_core_index(), __ATOMIC_RELEASE);
    __atomic_store_n(&s_run.active, 1u, __ATOMIC_RELEASE);
    if (why != SETTLE_FOR_HALT)
        GleanPassBegin();

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
    SettleSnap* snap     = &s_run.snap;
    bool        door     = s_run.door;
    uint64_t    target   = snap->target;
    bool        unclosed = snap->pool.unclosed;
    uint64_t    busy_us  = s_run.busy_us;
    uint64_t    s0_us    = s_run.s0_us;

    PoolPagesFree(snap->given);
    snap->given = NULL;
    if (rc != OK) {
        if (!s_failing)
            kprintf("[Settle] the cycle that would have settled %llu change(s) was refused at "
                    "%s (error %u) — nothing of it is declared settled, and the next cycle "
                    "writes it all again\n",
                    (unsigned long long)(target - SettledGen()), snap->stage, (unsigned)rc);
        s_failing = true;
        UseMemoryCycleOver(rc);
        TagFSWriteRefused();
    } else {
        SETTLE_CUT_AT(SETTLE_CUT_POINT_DONE);
        SettleDone(snap);
        UseMemoryCycleOver(OK);
        s_failing = false;
    }

    /* Its park before its Give: from the Give on the next cycle may end, and
     * a park this one left after it would stand on that cycle's answer. */
    SettleWaitersPark(rc);
    __atomic_store_n(&s_run.active, 0u, __ATOMIC_RELEASE);
    ScribeGive(&g_settle_scribe, s_run.cover, rc);

    CadenceRunEnded(rc, busy_us, s0_us, target);
    SettleWaitersRunEnded(rc, target, unclosed);
    if (door)
        tagfs_leave();
}

/* One step of the cycle in flight on this core, with the heap's reserve open
 * for it alone: true, and *answer, when that was its last. */
static bool SettleRunStep(error_t* answer)
{
    uint64_t t0 = SettleNowUs();
    KHeapReserveOpen();
    s_run.in_step = true;
    error_t rc = SettleStepRun(&s_run.snap, &s_run.step);
    s_run.in_step = false;
    s_run.busy_us += SettleNowUs() - t0;

    if (rc != OK || s_run.step == SETTLE_STEP_DONE) {
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

/* A cycle the cadence begins holds the volume's door from S0 to its end: the
 * volume it writes is the one mounted until it is done, whoever else leaves. */
bool SettleRunBeginForCadence(void)
{
    uint64_t cover   = 0;
    error_t  refused = OK;
    if (!tagfs_enter())
        return false;
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
static error_t SettleAsked(SettleWhy why)
{
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
    error_t rc = SettleAsked(SETTLE_FOR_WAITER);
    tagfs_leave();
    return rc;
}

/*
 * ‼ ROOM IS JUDGED BY WHAT CAME BACK (F01-3), never by what still rests:
 * deletes beside the asker put ground in Fallow as fast as a cycle gives some
 * back, and a cycle on another core may give all of it back between the
 * allocator's refusal and the first look here. A cycle answers by the ground
 * the S6 of the cycles gave back (s_given_count), or the allocator's free
 * count risen. Gives that wait as deltas for their sectors are room on its way
 * like Fallow's: the take that met them was told so (MapShort), and only the
 * cycle whose S1 lays them on lets the next take see them — answering at once
 * would send that take round again for ever. And with nothing resting, no
 * give unseen and no slack to glean there is no refusal: the room may have
 * come already, and the take asked again says the last word itself.
 */
error_t SettleForRoom(void)
{
    TagFSState* fs  = tagfs_get_state();
    bool        own = SettleRunBetweenStepsHere();

    /* A caller under a TagFS lock, a Scribe or a baton continuation may not
     * wait for a cycle: the room stays on its way, and the next cycle is asked
     * for. But the Settle's own holder, between two steps of its cycle, runs
     * that cycle to its end first (SettleAsked) — waiting for it would be
     * waiting for itself. */
    if (LockRankAnyHeldHere() || BatonRunningHere() || (ScribeAnyHeldHere() && !own)) {
        SettleDemand(SETTLE_DEMAND_ROOM);
        return ERR_WOULD_BLOCK;
    }

    /* With nothing resting, one cycle more lets Glean take the pool's slack off
     * its chain, at the lowered slack; what it takes rests like the rest. */
    for (uint64_t round = 0; round <= FallowLag() + 1u; round++) {
        TagFSLockTake(&fs->lock);
        uint64_t resting = FallowBlocks();
        bool     unseen  = BlockMapGivesUnseenLocked();
        bool     glean   = resting == 0 && !unseen && GleanMayFree();
        uint64_t given   = s_given_count;
        uint64_t had     = fs->ledger.free_blocks;
        TagFSLockDrop(&fs->lock);
        if (resting == 0 && !unseen && !glean)
            return OK;
        if (glean)
            GleanShort();

        error_t rc = SettleAsked(SETTLE_FOR_ROOM);
        if (rc != OK)
            return rc;

        TagFSLockTake(&fs->lock);
        bool came_back = s_given_count != given || fs->ledger.free_blocks > had ||
                         (unseen && !BlockMapGivesUnseenLocked());
        TagFSLockDrop(&fs->lock);
        if (came_back)
            return OK;
    }
    return ERR_TAGFS_NO_SPACE;
}

/* Whether the halt still owes the volume ground at rest: blocks Fallow holds,
 * or blocks the last cycle gave back that no cycle has written the map of. */
static bool SettleHaltOwesGround(void)
{
    TagFSState* fs = tagfs_get_state();
    TagFSLockTake(&fs->lock);
    bool resting = FallowBlocks() > 0;
    TagFSLockDrop(&fs->lock);
    return resting || s_given > 0;
}

error_t TagFSSettleForHalt(void)
{
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;

    /* A medium that is away takes no cycle, and the stopping machine is the
     * event its open book waited for. */
    if (TagFSMediumLeft()) {
        AwayGiveUp(AWAY_HALT, tagfs_get_seat());
        s_halt_answer = ERR_DEVICE_NOT_READY;
        s_halt_ran    = true;
        tagfs_leave();
        return ERR_DEVICE_NOT_READY;
    }

    /*
     * ‼ NO CLEAN SHUTDOWN LEAVES GROUND AT REST. A block in Fallow is taken on
     * the medium and named by nothing, and the next mount's roll call keeps
     * such a block taken for good. So the last cycle is followed by the ones
     * Fallow's rule asks for — each settles, so both copies of the Ledger move
     * past the records that freed the ground — and by one more that writes the
     * map giving it back. A Ledger freer the last cycle itself made — the Use
     * Context chain it replaced — is named by the next cycle's image and rests
     * FallowLag() cycles from there: at most 1 + 1 + FallowLag() + 1, fewer
     * when nothing rests.
     */
    error_t rc = SettleAsked(SETTLE_FOR_HALT);
    for (uint64_t more = 0; rc == OK && more <= FallowLag() + 1u && SettleHaltOwesGround(); more++)
        rc = SettleAsked(SETTLE_FOR_HALT);
    s_halt_answer = rc;
    s_halt_ran    = true;
    tagfs_leave();
    return rc;
}
