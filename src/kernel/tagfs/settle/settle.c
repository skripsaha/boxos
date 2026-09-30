#include "settle.h"
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
#include "amp.h"
#include "baton.h"

/*
 * The Settle cycle (settle.h). Its holder is the one committer, and is never
 * switched away from while it holds the Scribe (scribe.h); everything below
 * that is not S0 or S6 runs with no TagFS lock held.
 */
Scribe g_settle_scribe = SCRIBE_INIT(SCRIBE_RANK_SETTLE, "the volume's Settle");

typedef enum {
    SETTLE_FOR_WAITER = 1,   /* an anchor or a demand waits for what it settles */
    SETTLE_FOR_ROOM,         /* the volume is short while Fallow rests blocks */
    SETTLE_FOR_HALT,         /* the machine is stopping: the last cycle */
} SettleWhy;

/* Under the state lock. */
static uint64_t s_settled_gen;
static uint64_t s_cycles;        /* cycles settled this mount; the next is s_cycles + 1 */

/* Said once for each run of refused cycles, until one settles. */
static bool     s_failing;

/* Blocks the last settled cycle's S6 gave back: the map carries them to the
 * medium at the next cycle's S1. The Settle's holder's alone. */
static uint64_t s_given;

/* The halt's last cycle, for tagfs_shutdown. */
static bool     s_halt_ran;
static error_t  s_halt_answer;

/* Where the last settled seal's pool span ended — the next seal's pool_from —
 * and whether this volume's seals say its medium once held less than a seal
 * did. The Settle's holder's, and the mount's with the door shut. */
static uint32_t s_sealed_block;
static uint32_t s_sealed_bytes;
static bool     s_medium_lied;

uint64_t SettledGen(void)
{
    return __atomic_load_n(&s_settled_gen, __ATOMIC_ACQUIRE);
}

uint64_t SettledCycles(void)
{
    return __atomic_load_n(&s_cycles, __ATOMIC_ACQUIRE);
}

void SettleMountReset(void)
{
    s_given = 0;
    __atomic_store_n(&s_settled_gen, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_cycles, 0, __ATOMIC_RELEASE);
    s_failing  = false;
    s_halt_ran = false;
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
// One cycle
// ----------------------------------------------------------------------------

typedef struct {
    PoolSnap      pool;
    SettleMark    mark;         /* what the cycle leaves on the medium, for S4 */
    uint64_t      target;
    uint64_t      cycle;
    uint64_t      data_need;
    MetaPoolPage* given;        /* the halt's Spares, given back at S0 */
    const char*   stage;        /* where a refusal came from, for the one line */
} SettleSnap;

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
 */
static void SettleTake(SettleSnap* snap, SettleWhy why)
{
    TagFSState* fs = tagfs_get_state();
    TagFSLockTake(&fs->lock);
    snap->target    = InkedGen();
    snap->cycle     = s_cycles + 1;
    snap->data_need = InkDataNeed();
    PoolSnapTake(&snap->pool, true);
    if (why == SETTLE_FOR_HALT)
        snap->given = PoolSparesGiveBack();
    PoolSealTake(&snap->mark);
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

/*
 * S1: what the pool's records stand on, and what the Ledger names. The
 * registry's refusal is said by it and ends nothing: no record wears a tag
 * the medium does not define — an Ink takes none that is not on it
 * (InkNameTagIntern, TagRegistryFlushFor) — so what it could not write is
 * only the tags nobody wears yet. The map's, the table's, the integrity
 * map's and the Use Context chain's refusals end the cycle.
 */
static error_t SettleStandings(SettleSnap* snap)
{
    TagFSState* fs = tagfs_get_state();

    SETTLE_CUT_AT(SETTLE_CUT_POINT_REGISTRY);
    (void)tag_registry_flush(fs->registry);

    SETTLE_CUT_AT(SETTLE_CUT_POINT_BITMAP);
    error_t rc = BlockMapFlush();
    if (rc != OK) {
        snap->stage = "the block map";
        return rc;
    }

    SETTLE_CUT_AT(SETTLE_CUT_POINT_TABLE);
    if (file_table_flush() != 0) {
        snap->stage = "the file table";
        return ERR_TAGFS_METADATA_ERROR;
    }

    SETTLE_CUT_AT(SETTLE_CUT_POINT_INTEGRITY);
    FallowZeroDigests(snap->cycle);
    rc = IntegrityFlush();
    IntegrityDrainReports();
    if (rc != OK) {
        snap->stage = "the integrity map";
        return rc;
    }

    SETTLE_CUT_AT(SETTLE_CUT_POINT_USECHAIN);
    rc = UseMemoryWriteLinks();
    if (rc != OK)
        snap->stage = "the Use Context's chain";
    return rc;
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
    if (reg > need)
        need = reg;
    if (use > need)
        need = use;
    if (snap->pool.link_need > need)
        need = snap->pool.link_need;
    if (need == 0)
        return OK;
    error_t rc = VolumeFlushFor(need);
    if (rc != OK)
        snap->stage = "the flush before the pool";
    return rc;
}

/* S6, under the state lock: the target settled, the written closed blocks out
 * of memory, the copy of the Ledger written naming the Use Context's chain it
 * was given, and Fallow's ripe ground back to the allocator. */
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
    s_given = FallowRipen(closed, snap->cycle, &nodes);
    TagFSLockDrop(&fs->lock);

    PoolPagesFree(spent);
    FallowNodesFree(nodes);
}

static error_t SettleCycle(SettleWhy why)
{
    SettleSnap snap;
    memset(&snap, 0, sizeof(snap));

    /* A Spare for the early close, made before S0 — which takes nothing from
     * the heap or the medium. A volume with none keeps its tail open, and the
     * cycle writes it as it stands. */
    if (why != SETTLE_FOR_HALT)
        (void)PoolSpareRefill(1);
    UseMemoryStake();

    SettleTake(&snap, why);

    error_t rc = SettleStandings(&snap);
    if (rc == OK)
        rc = SettleFirstFlush(&snap);
    if (rc == OK) {
        SETTLE_CUT_AT(SETTLE_CUT_POINT_POOL);
        rc = PoolSnapWrite(&snap.pool);
        if (rc != OK)
            snap.stage = "the metadata pool";
    }
    if (rc == OK) {
        SETTLE_CUT_AT(SETTLE_CUT_POINT_LEDGER);
        rc = LedgerImageWrite(&snap.mark);
        UseMemoryLedgerWritten();
        if (rc != OK)
            snap.stage = "the ledger";
    }
    if (rc == OK) {
        SETTLE_CUT_AT(SETTLE_CUT_POINT_FLUSH);
        rc = VolumeFlushFor(VolumeFlushNeed());
        if (rc != OK)
            snap.stage = "the flush after the ledger";
    }
    PoolPagesFree(snap.given);

    if (rc != OK) {
        if (!s_failing)
            kprintf("[Settle] the cycle that would have settled %llu change(s) was refused at "
                    "%s (error %u) — nothing of it is declared settled, and the next cycle "
                    "writes it all again\n",
                    (unsigned long long)(snap.target - SettledGen()), snap.stage,
                    (unsigned)rc);
        s_failing = true;
        UseMemoryCycleOver(rc);
        return rc;
    }
    SETTLE_CUT_AT(SETTLE_CUT_POINT_DONE);
    SettleDone(&snap);
    UseMemoryCycleOver(OK);
    s_failing = false;

    /* The reserve a delete stands on, made again first from what came back. */
    if (why != SETTLE_FOR_HALT)
        (void)PoolSpareRefill(SPARE_RESERVE);
    return OK;
}

/* A cycle that began after the ask: this call's own, or one another core was
 * already making. */
static error_t SettleAsked(SettleWhy why)
{
    uint64_t   cover  = 0;
    error_t    rc     = OK;
    uint64_t   ticket = ScribeAsk(&g_settle_scribe);
    ScribeTurn turn   = why == SETTLE_FOR_HALT
                            ? ScribeWaitTakingOver(&g_settle_scribe, ticket, &cover, &rc)
                            : ScribeWait(&g_settle_scribe, ticket, &cover, &rc);
    switch (turn) {
    case SCRIBE_MINE:
        rc = SettleCycle(why);
        ScribeGive(&g_settle_scribe, cover, rc);
        return rc;
    case SCRIBE_SELF:
        kprintf("[Settle] a cycle was asked for on a core that holds one beneath the ask — "
                "waiting for it here would never end\n");
        return ERR_BUSY;
    case SCRIBE_REFUSED:
        return rc;
    default:
        return OK;          /* SCRIBE_WRITTEN */
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

error_t SettleForRoom(void)
{
    TagFSState* fs = tagfs_get_state();

    /* A caller under a TagFS lock or a Scribe, or a baton continuation, may
     * not wait for a cycle: the room stays on its way. */
    if (LockRankAnyHeldHere() || ScribeAnyHeldHere() || BatonRunningHere())
        return ERR_WOULD_BLOCK;

    for (uint64_t round = 0; round <= FallowLag(); round++) {
        TagFSLockTake(&fs->lock);
        uint64_t resting = FallowBlocks();
        TagFSLockDrop(&fs->lock);
        if (resting == 0)
            return round ? OK : ERR_TAGFS_NO_SPACE;

        error_t rc = SettleAsked(SETTLE_FOR_ROOM);
        if (rc != OK)
            return rc;

        TagFSLockTake(&fs->lock);
        bool came_back = FallowBlocks() < resting;
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
