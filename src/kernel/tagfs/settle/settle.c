#include "settle_internal.h"
#include "sewing.h"
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
 * The Settle cycle (settle.h). Its holder is the one committer, and is never
 * switched away from while it holds the Scribe (scribe.h); everything below
 * that is not S0 or S6 runs with no TagFS lock held. Between two steps the
 * holder's core is back in its loop, and allocates there as any other work
 * does: the heap's reserve is the steps' alone.
 */
Scribe g_settle_scribe = SCRIBE_INIT(SCRIBE_RANK_SETTLE, "the volume's Settle");

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

/* Whether the newest settled cycle sealed the volume clean: the halt's last
 * word on the medium, asked by its drain (SettleOwed). */
static volatile bool s_sealed_clean;

/* Steps any cycle took this boot — how the halt's drain sees a cycle on
 * another core going on (SettleStepsTaken). */
static volatile uint64_t s_steps;

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
    DeedFormat       format;     /* the book it writes, fixed as it begins: format v1's steps or v2's */
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

/* The refusal of a cycle on a volume that takes no write — under glass, or a
 * book no Quire took a write of — said once a mount. */
static volatile uint8_t s_refusal_said;

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
    SettleHaltMountReset();
    StitchMountReset();
    __atomic_store_n(&s_sealed_clean, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_refusal_said, 0u, __ATOMIC_RELEASE);
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

uint64_t SettleGivenLast(void)
{
    return s_given;
}

void SettleGivenLocked(uint64_t given)
{
    s_given        = given;
    s_given_count += given;
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
 * the machine stops after them — each one no change is in flight under. A
 * change that holds or waits for a file's Helm may still lay a write's bytes
 * in place after this snapshot, their digest with them or not, and the next
 * mount takes a clean seal's word that every digest stands (RT-22). No change
 * begins once the halt has shut the volume (TagFSShutForHalt), so the cycle
 * after the last of them seals clean.
 *
 * A cycle somebody asked for and waits on closes the tail onto a Spare, and so
 * does one that must give Fallow's ground back; one the cadence began closes
 * it only as far as a waiter is owed, or as far as its Glean laid records
 * again or bought the close an emptied front waits for — a moved record lies
 * in a closed block under the seal that lets its source go (glean.h). A close
 * for a waiter leaves Glean its share of the reserve (meta_pool.h). Glean's
 * own close digs to the bottom: its step held that Spare. So do the halt's,
 * which gives every Spare back right after — and leaves out of the map the
 * ground only an open handle still holds — and one asked for room while
 * Fallow's ground rests on a record in the open tail: that ground comes back
 * only once the tail is closed, and it is what makes the reserve whole again.
 * Left to a tail that never fills, it would rest for ever and the asker would
 * wait through every cycle after.
 */
static void SettleTake(SettleSnap* snap, SettleWhy why, uint32_t demands)
{
    bool     room       = why == SETTLE_FOR_ROOM || (demands & SETTLE_DEMAND_ROOM);
    bool     closes     = why != SETTLE_FOR_CADENCE || room;
    uint64_t owed_close = closes ? 0 : SettleWaitersCloseUpto();

    TagFSState* fs = tagfs_get_state();
    TagFSLockTake(&fs->lock);
    snap->target    = InkedGen();
    snap->cycle     = s_cycles + 1;
    snap->data_need = InkDataNeed();
    uint64_t close_upto = closes ? snap->target
                                 : (owed_close < snap->target ? owed_close : snap->target);
    if (!closes && GleanMovedGen() > close_upto)
        close_upto = GleanMovedGen();
    bool dig = why == SETTLE_FOR_HALT || GleanMovedGen() > PoolClosedGenMax() ||
               (room && FallowRestsOnTail(PoolClosedGenMax()));
    PoolSnapTake(&snap->pool, close_upto, dig ? 0u : (uint32_t)SPARE_FOR_GLEAN);
    if (why == SETTLE_FOR_HALT) {
        snap->given = PoolSparesGiveBack();
        IntegrityLayDrop();
        FallowViewedUnnamed(s_cycles);
    } else {
        (void)IntegrityLayName();
    }
    PoolSealTake(&snap->mark);
    GleanHeadTake(&snap->head);
    TagRegistryLanded(fs->registry, &snap->mark);
    snap->mark.pool_from_block = s_sealed_block;
    snap->mark.pool_from_bytes = s_sealed_bytes;
    snap->mark.flags           = (why == SETTLE_FOR_HALT && HelmOwed() == 0 ? SETTLE_CLEAN : 0u) |
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
                "and Glean stand on (error %u; %llu block(s) free, %llu resting in Fallow) — "
                "a delete past the tail's room waits for one, and every cycle tries again\n",
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

/* The step's answer as the stand chose it, once — none of the medium's: format
 * v1's block map wrote what it owed, and the step answers the stand's code; a
 * book's stitch answers it before it is made (sewing.c, STITCH_STEP_BUILD). */
error_t SettleStandRefusalHere(error_t rc)
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
    SettleSettledLocked(snap->target, snap->cycle, (snap->mark.flags & SETTLE_CLEAN) != 0);
    s_sealed_block = snap->mark.pool_to_block;
    s_sealed_bytes = snap->mark.pool_to_bytes;
    UseMemorySettled();
    LedgerSettled();
    GleanHeadSettled(&snap->head, snap->cycle, snap->mark.chain_first);
    SettleGivenLocked(FallowRipen(closed, snap->cycle, &nodes));
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

/* A cycle the caller just took the Scribe for, begun in the format of the book
 * it writes, which it keeps to its end: format v1's with Glean's pass first —
 * a machine stopping gleans nothing, since ground it took off now would rest
 * past the last cycle — and then S0, each a step of its own; format v2's at
 * its TAKE (sewing.c).
 *
 * Once the halt has shut the volume, a cycle anybody else begins is one of its
 * cycles too. One the cadence had already decided on, or an asker for room on
 * another core, can take the Scribe right after the halt's ask, and the halt's
 * ask is then answered by that cycle: begun as itself, it would seal the
 * volume unclean and stamp a Spare after the halt gave the last one back. */
static void SettleRunBegin(SettleWhy why, uint64_t cover, bool door)
{
    if (SettleHalting())
        why = SETTLE_FOR_HALT;
    memset(&s_run.snap, 0, sizeof(s_run.snap));
    s_run.format  = tagfs_get_state()->format;
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
    if (s_run.format == DEED_FORMAT_V2)
        StitchRunBegin(why);
    else if (why != SETTLE_FOR_HALT)
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
    bool        book     = s_run.format == DEED_FORMAT_V2;
    bool        door     = s_run.door;
    uint64_t    target   = book ? StitchRunTarget() : snap->target;
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
                    (unsigned long long)(target > SettledGen() ? target - SettledGen() : 0u),
                    book ? StitchRunStage() : snap->stage,
                    (unsigned)rc);
        s_failing = true;
        if (book)
            StitchRunEnd(rc);
        else
            UseMemoryCycleOver(rc);
        TagFSWriteRefused();
    } else {
        SETTLE_CUT_AT(SETTLE_CUT_POINT_DONE);
        if (book) {
            StitchRunEnd(OK);
        } else {
            SettleDone(snap);
            UseMemoryCycleOver(OK);
        }
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

/* One step of the cycle in flight in the format it began in, or of the other:
 * a volume's format is fixed by its mount, and a cycle is held to it from its
 * first step to its last — the book it writes is the one it began on. */
static error_t SettleStepOf(bool* done)
{
#if CONFIG_STRICT
    if (tagfs_get_state()->format != s_run.format)
        panic("[Settle] DEFECT: a cycle begun on a volume of format %u stepped on one of format %u",
              (unsigned)s_run.format, (unsigned)tagfs_get_state()->format);
#endif
    if (s_run.format == DEED_FORMAT_V2)
        return StitchRunStep(done);
    error_t rc = SettleStepRun(&s_run.snap, &s_run.step);
    *done = s_run.step == SETTLE_STEP_DONE;
    return rc;
}

/* One step of the cycle in flight on this core, with the heap's reserve open
 * for it alone: true, and *answer, when that was its last. */
static bool SettleRunStep(error_t* answer)
{
    uint64_t t0   = SettleNowUs();
    bool     done = false;
    KHeapReserveOpen();
    s_run.in_step = true;
    error_t rc = SettleStepOf(&done);
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
 * where the kernel said nothing would be. Every other volume's cycle is its
 * format's: format v1's Ledger, pool, table and bitmap, or a book's stitch
 * (sewing.c) — chosen as the cycle begins.
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

/* A cycle the cadence begins holds the volume's door from S0 to its end: the
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
 * A book's room (§3.4, §3.6, В1): ground a stitch gave up comes back once the
 * ribbon left the Quire the stitch lies in and the lag's stitches settled past
 * it. A cycle asked for room closes that Quire early when Fallow waits for it
 * (sewing.c, StitchTake) — its stitch goes on at the start of the next_quire
 * the last stitch names — and the lag's cycles after it give the ground back:
 * as many rounds as that takes, judged as format v1's by what came back. Room
 * on the ribbon is a cycle's to make even with nothing resting — the claims in
 * flight settled, a next_quire asked for (settle/ribbon.c, RibbonClaim) — so
 * one cycle is always run before the take is asked again, which says the last
 * word itself: a book has no pool slack to glean and no map sector a give
 * waits for. A take that waits on a Tally (binder.h, BinderRoomWait) has it
 * written here first, by its own writer, a step a turn of the Scribe — not
 * left to the committer while its cycles turn empty, each a stitch on a
 * ribbon a full volume has one Quire to spare for — and the ground the slot
 * let go rests in Fallow for the rounds after. One the tails hold back is
 * left to the Helm's service (BinderRunAwaited): this wait is reached from
 * inside a Helm span, and an Engross here would walk the files under it.
 */
static error_t SettleForRoomBook(void)
{
    TagFSState* fs = tagfs_get_state();
    if (BinderRoomAwaited())
        (void)BinderRunAwaited();
    for (uint64_t round = 0; round <= FallowLag() + 1u; round++) {
        TagFSLockTake(&fs->lock);
        uint64_t resting = FallowBlocks();
        uint64_t given   = s_given_count;
        uint64_t had     = fs->ledger.free_blocks;
        TagFSLockDrop(&fs->lock);
        error_t rc = SettleAskedFor(SETTLE_FOR_ROOM);
        if (rc != OK)
            return rc;
        TagFSLockTake(&fs->lock);
        bool came_back = s_given_count != given || fs->ledger.free_blocks > had;
        TagFSLockDrop(&fs->lock);
        if (came_back || resting == 0)
            return OK;
    }
    return ERR_TAGFS_NO_SPACE;
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
 * come already, and the take asked again says the last word itself. A sweep
 * of Glean's a delete waits on (glean.h) is answered after each cycle: the
 * delete asked again keeps it going, and hears the sweep's own judgment.
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
    if (fs->format == DEED_FORMAT_V2)
        return SettleForRoomBook();

    /* With nothing resting, one cycle more lets Glean take the pool's slack off
     * its chain, at the lowered slack; what it takes rests like the rest. */
    for (uint64_t round = 0; round <= FallowLag() + 1u; round++) {
        TagFSLockTake(&fs->lock);
        uint64_t resting = FallowBlocks();
        bool     unseen  = BlockMapGivesUnseenLocked();
        bool     glean   = resting == 0 && !unseen && GleanMayFree();
        bool     sweep   = GleanSweepingLocked();
        uint64_t given   = s_given_count;
        uint64_t had     = fs->ledger.free_blocks;
        TagFSLockDrop(&fs->lock);
        if (resting == 0 && !unseen && !glean && !sweep)
            return OK;
        if (glean)
            GleanShort();

        error_t rc = SettleAskedFor(SETTLE_FOR_ROOM);
        if (rc != OK)
            return rc;

        TagFSLockTake(&fs->lock);
        bool came_back = s_given_count != given || fs->ledger.free_blocks > had ||
                         (unseen && !BlockMapGivesUnseenLocked());
        TagFSLockDrop(&fs->lock);
        if (came_back || sweep)
            return OK;
    }
    return ERR_TAGFS_NO_SPACE;
}
