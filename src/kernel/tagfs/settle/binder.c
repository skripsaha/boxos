#include "binder_internal.h"
#include "escheat.h"
#include "../mount/book_mount.h"
#include "../mount/under_glass.h"
#include "../tag_registry/tag_registry.h"
#include "../use_memory/use_memory.h"
#include "../block_map/block_map_derived.h"
#include "../scribe/scribe.h"
#include "../settle_cut/settle_cut.h"
#include "../helm/engross.h"
#include "loose_leaves.h"
#include "../../../kernel/drivers/timer/rtc.h"

/*
 * The Tally's steps (binder.h):
 *
 *   TALLY_TAKE        right after a stitch k settled, no stitch in flight, in
 *                     one turn: the slice — every live file's record as k
 *                     leaves it, a batch of files under the state lock at a
 *                     time (the Docket's `was` for a file changed since, the
 *                     map's record for the rest), the tags below the last
 *                     Coined that settled, the Use Context the book holds, the
 *                     ground DIGESTS speaks for; the ribbon as k leaves it.
 *                     Then, a turn each, the cycles going on between: a leaf of
 *                     the DIGESTS the reads stand on, to know the ones no copy
 *                     of reads; a node of the Tally, measured with nothing
 *                     written — and, the last one measured, the two runs its
 *                     copies go in taken from the map
 *   TALLY_NODES       a node a step, both copies, the cycles going on between
 *   TALLY_FLUSH       the flush that keeps every node
 *   TALLY_SLOT        the slot of the least seq written over: SpineHead naming
 *                     the Tally, k and k's Clasp; a root a tree; SpineRibbon
 *                     where the ribbon goes on past k; SpineCounts; SpineFlags
 *                     when the medium lied
 *   TALLY_SLOT_FLUSH  the flush that keeps the slot
 *   TALLY_DONE        the slot named so in memory; the Tally and the ribbon
 *                     the slot it was written over held, and no other slot
 *                     holds, rest in Fallow (FALLOW_BY_SPINE) for the lag's
 *                     stitches; the Gloss let go of what the Tally's DIGESTS
 *                     now names, and the reads of a digest turned to it
 *
 * ‼ WHILE A TALLY IS UNDER WAY NO GROUND GOES BACK. The slice names blocks by
 * the state after k, and their digests and births are read as the nodes are
 * laid, a step a turn: a block a later stitch gave up, handed back and written
 * again meanwhile would lose the digest and the birth the slice needs. Fallow
 * rests on until the Tally is done (sewing.c asks BinderHoldsGround).
 */

static BinderRun s_run;
static volatile uint8_t s_under_way;   /* s_run.active, for the cores that are not the holder */

#ifdef CONFIG_VOLUME_TESTS
static TallyStep s_stand_stop;
static uint32_t  s_stand_tear;
#endif

// ----------------------------------------------------------------------------
// TALLY_TAKE: the state after k, measured, and the ground its copies go in
// ----------------------------------------------------------------------------

/* Why a Tally may not be begun or go on. The first two pass on their own as
 * the stitches go on; the volume taking no write is said by its own line. */
static const char s_no_write[]  = "the volume takes no write";
static const char s_not_up[]    = "the volume's book is not up";
static const char s_unsettled[] = "no stitch of this mount has settled yet (§4.2)";
static const char s_on_edge[]   = "the last stitch ends on its Quire's last sector, where no slot can put the "
                                  "ribbon on — until the next stitch";
static const char s_off_chain[] = "the Quire the ribbon stands in is not one the slots' ribbons are known to run "
                                  "through — memory would not note it, and no Tally is written until the ribbon "
                                  "has left it";
static const char s_loose[]     = "the settled state carries Slips (§16.5 п.14) — each tail goes into its block "
                                  "first (Engross)";

bool BinderNoWrite(void)
{
    return TagFSReadOnly() || TagFSIsUnderGlass() || StitchBookRefused() || TagFSMediumLeft() || SettleHalting();
}

/* Why a Tally may not be begun as the holder's own state stands, or NULL — by
 * the Settle's holder, who alone moves what it reads (the cursor, the chain). */
static const char* BinderOwnMayNot(void)
{
    const RibbonCursor* c = RibbonCursorNow();
    if (!g_binder.up || !c->up)
        return s_not_up;
    if (BinderNoTallies())
        return BinderNoTallies();
    if (SettledCycles() == 0)
        return s_unsettled;
    if (!RibbonQuireSame(&c->quire, &c->ended))
        return s_on_edge;
    if (BinderChainIndex(&c->quire) == UINT64_MAX)
        return s_off_chain;
    if (LooseLeaves() > 0)
        return s_loose;      /* a Tally carries no Slip: its slice would lose the tail */
    return NULL;
}

/* Why a Tally may not be begun now, or NULL: by the Settle's holder. */
static const char* BinderMayNot(void)
{
    return BinderNoWrite() ? s_no_write : BinderOwnMayNot();
}

/* Where the next Tally's nodes go on in the pair: past the ones laid, on an
 * atom. */
static uint64_t PairNext(uint64_t grid)
{
    return (g_binder.pair_used + grid - 1u) / grid * grid;
}

/*
 * ‼ THE TALLIES GO ON IN THE CURRENT PAIR OF TALLY-QUIRES (§2.6, §16.5 п.16).
 * Each copy of a node goes in a run of whole Quires of its own — each Quire on
 * the grid of the ribbon's, so a census after a mount holds the same Quire
 * whole (Р-d3) — the second past the first, so the two copies of a node are a Quire apart at
 * the least and never in one unit a medium maps as one (§2.5): a Tally's nodes
 * are laid one after another past the last Tally's in the pair, copy 2 as far
 * into B as copy 1 into A, and a new pair is taken — the least k Quires its
 * nodes fit in — only when they would not fit what is left. What of the pair
 * before no node was laid in goes back to the map then, whole Quires: no slot
 * ever named it (BinderPairLetGo). Taking a pair keeps free what the ring's other slots lack of a Tally
 * this size (BinderRingShort): the Tallies that fill the ring after it give
 * nothing back, and each must find its ground (§16.2 п.6). No Quire of ribbon
 * is kept past it: the next_quire takes that from the reserve itself, and a
 * Tally the ribbon asked for once it took it must still be written — it is
 * what gives the ribbon its ground back. OK, or the map's refusal with
 * nothing taken.
 */
static error_t TallyRunsTake(BinderRun* r, uint64_t blocks)
{
    uint64_t q    = RibbonCursorNow()->quire_blocks;
    uint64_t data = g_book.frame.data_block;
    uint64_t at   = PairNext(r->w.grid);
    if (g_binder.pair_room && at + blocks <= g_binder.pair_room) {
        r->w.a    = g_binder.pair_a + at;
        r->w.b    = g_binder.pair_b + at;
        r->w.room = g_binder.pair_room - at;
        r->taken  = true;
        BinderSlotOwnPublish(r->slot, 0);   /* the pair's: nothing off the free */
        return OK;
    }
    uint64_t run  = (blocks + q - 1u) / q * q, a = 0, b = 0;
    uint64_t keep = BinderRingShortBut(2u * run, q, r->slot);
    error_t  rc   = BlockMapTakeQuire(GROUND_TALLY, run, (uint32_t)q, keep + run, data, data, &a);
    if (rc != OK)
        return rc;
    rc = BlockMapTakeQuire(GROUND_TALLY, run, (uint32_t)q, keep, data, a + run, &b);
    if (rc != OK) {
        (void)BlockMapDerivedGiveAs(GROUND_TALLY, 0, a, run);
        return rc;
    }
    BinderPairLetGo(q);
    g_binder.pair_a    = r->w.a = a;
    g_binder.pair_b    = r->w.b = b;
    g_binder.pair_room = r->w.room = run;
    g_binder.pair_used = 0;
    r->taken = true;
    BinderSlotOwnPublish(r->slot, 2u * run);   /* the pair whole, as the free gave it */
    return OK;
}

void BinderPairUsed(const BinderRun* r)
{
    if (r->taken && r->w.a >= g_binder.pair_a && r->w.a - g_binder.pair_a + r->w.off > g_binder.pair_used)
        g_binder.pair_used = r->w.a - g_binder.pair_a + r->w.off;
}

/* The slot's numbers (§4.5 п.8): past every fid, tag and bookmark the medium
 * has shown — the mount's, the stitches' of this mount, the slice's. */
static void TallyCounts(BinderRun* r)
{
    uint64_t fid = g_binder.seen_fid > g_binder.laid_fid ? g_binder.seen_fid : g_binder.laid_fid;
    for (BinderFiles* c = r->w.files; c; c = c->next)
        if (c->n && c->f[c->n - 1u].fid + 1u > fid)
            fid = c->f[c->n - 1u].fid + 1u;
    r->counts = (SpineCounts){ .next_fid = fid,
                               .next_tag = g_binder.seen_tag > r->w.tag_hi ? g_binder.seen_tag : r->w.tag_hi,
                               .next_bookmark = g_binder.seen_bookmark };
}

/* The slice of the state after k — the files of the leaves a Tally laid in
 * lays, every file of a whole one — and the ground DIGESTS speaks for: memory
 * alone, no read of the medium — one turn of the holder, so no stitch settles
 * inside it, and as long as a walk of every live file's runs takes. The
 * Tally's seq is taken here: every pointer and birth_delta of it is spelled
 * from it, and a measure of other numbers is no bound. */
static error_t TallySliceTake(BinderRun* r)
{
    BinderWork*         w = &r->w;
    const RibbonCursor* c = RibbonCursorNow();
    uint32_t            lo = 0, hi = 0;
    r->k         = c->last;
    r->clasp     = c->clasp;
    r->ribbon    = (SpineRibbon){ .at = c->quire.block + c->off / TAGFS_BLOCK_SECTORS, .quire_block = c->quire.block,
                                  .quire_blocks = c->quire.blocks, .next_block = c->named.block,
                                  .next_blocks = c->named.blocks };
    r->chain_at  = BinderChainIndex(&c->quire);
    r->useful_at = g_binder.useful;
    r->made_ns   = rtc_get_unix64() * 1000000000ull;
    EscheatTake();
    DogEarTake();
    g_binder.cost_known = 0;
    TagRegistryCoinedRange(tagfs_get_state()->registry, &lo, &hi);
    w->tag_hi = lo;
    w->use    = tagfs_recall_use_context(NULL);
    w->grid   = g_book.atom > TAGFS_BLOCK_SIZE ? g_book.atom / TAGFS_BLOCK_SIZE : 1u;
    w->seq    = RibbonSeqNext();
    error_t rc = BinderTipInTake(r);
    if (rc == OK)
        rc = BinderSliceTake(w, r->fids, r->nfids);
    if (rc == OK)
        rc = BinderGroundTake(w);
    if (rc == OK) {
        TallyCounts(r);
        EndpaperTallyBegun(w->seq, r->counts.next_fid);
    }
    return rc;
}

// ----------------------------------------------------------------------------
// TALLY_SLOT: the slot of the least seq
// ----------------------------------------------------------------------------

/* The slot's stream: SpineHead, a root a tree laid, SpineRibbon, SpineCounts,
 * SpineFlags when the medium lied — into `out`, the starts of its records
 * into `starts`. Its length. */
static uint32_t SlotStream(const BinderRun* r, uint8_t* out, uint32_t* starts, uint32_t* n)
{
    const BinderWork* w = &r->w;
    SpineHead         h = { .tally_seq = w->seq, .stitch_seq = r->k, .stitch_clasp = r->clasp, .made_ns = r->made_ns };
    uint32_t          at = 0;
    *n = 0;
    starts[(*n)++] = at;
    at += SpineHeadPut(out + at, &h);
    for (uint32_t tree = TALLY_FILES; tree <= TALLY_SHARED; tree++) {
        const BinderRoot* root = &w->root[tree];
        uint64_t          blocks[TALLY_COPIES_HELD] = { root->ptr.block[0], root->ptr.block[1] };
        uint8_t           ptr[48];
        if (!root->laid)
            continue;
        uint32_t plen  = BookPtrPut(ptr, root->ptr.seq, root->ptr.sheets, blocks,
                                    root->ptr.copies < TALLY_COPIES_HELD ? root->ptr.copies : TALLY_COPIES_HELD);
        starts[(*n)++] = at;
        at += SpineRootPut(out + at, tree, 0, root->level, root->items, ptr, plen);
    }
    starts[(*n)++] = at;
    at += SpineRibbonPut(out + at, &r->ribbon);
    starts[(*n)++] = at;
    at += SpineCountsPut(out + at, &r->counts);
    if (RibbonCursorLied()) {
        starts[(*n)++] = at;
        at += SpineFlagsPut(out + at, SPINE_MEDIUM_LIED);
    }
    return at;
}

/* The slot's sectors from `first`, `n` of them, to the medium. */
static error_t SlotSectorsWrite(uint64_t vlba, uint32_t first, uint32_t n, const uint8_t* unit)
{
    int wrote = tagfs_volume_write(vlba + first, n, unit + (uint64_t)first * BOOK_SECTOR_BYTES);
    return wrote == 0 ? OK : wrote == (int)ERR_DEVICE_NOT_READY ? ERR_DEVICE_NOT_READY : ERR_WRITE_FAILED;
}

/*
 * The slot written over: its stream sealed at a seq past the Tally's, in two
 * halves of its eight sectors — the cut a SETTLECUT `slot` makes between them
 * leaves it torn (C9), as the stand's tear leaves it with only its first
 * `tear` sectors. From the first sector issued, the slot may hold either
 * Tally (slot_begun) — the new one as sealed here, kept in r->sealed until the
 * Tally ends, and by the slot itself when it is held unsure.
 */
static error_t TallySlotWrite(BinderRun* r)
{
    uint8_t* stream = kmalloc(TALLY_SHEET_PAYLOAD);
    uint32_t starts[TALLY_SHARED + 6u], n = 0, half = TAGFS_BLOCK_SECTORS / 2u;
    r->sealed       = kmalloc(sizeof(SlotSealed));
    error_t  rc     = stream && r->sealed ? OK : ERR_NO_MEMORY;
    if (rc == OK) {
        uint8_t* unit = r->sealed->unit;
        r->sealed->next = NULL;
        memset(stream, 0, TALLY_SHEET_PAYLOAD);
        uint32_t len   = SlotStream(r, stream, starts, &n);
        uint64_t block = g_book.frame.spine_block + (uint64_t)r->slot * g_book.frame.spine_stride;
        uint64_t vlba  = block * TAGFS_BLOCK_SECTORS;
        r->slot_seq    = RibbonSeqNext();
        TallyUnitSeal(unit, stream, len, starts, n, BOOK_UNIT_SLOT, r->slot_seq, vlba, TAGFS_BLOCK_SECTORS,
                      tagfs_get_state()->uuid);
#ifdef CONFIG_VOLUME_TESTS
        if (s_stand_tear)
            half = s_stand_tear;
#endif
        r->slot_begun = true;
        rc = SlotSectorsWrite(vlba, 0, half, unit);
#ifdef CONFIG_VOLUME_TESTS
        if (rc == OK && s_stand_tear)
            rc = ERR_IO_CANCELLED;          /* the stand tears it here */
#endif
        SETTLE_CUT_CYCLE(r->target);
        if (rc == OK)
            SETTLE_CUT_AT(SETTLE_CUT_POINT_SLOT);
        if (rc == OK)
            rc = SlotSectorsWrite(vlba, half, TAGFS_BLOCK_SECTORS - half, unit);
    }
    kfree(stream);
    return rc;
}

// ----------------------------------------------------------------------------
// TALLY_DONE, and a Tally refused
// ----------------------------------------------------------------------------

/* The Tally under way ended without its DONE: its runs given back when no
 * slot can name them, held when the slot's write began; everything let go. */
static void TallyEnded(BinderRun* r, error_t rc)
{
    if (rc != OK && r->taken && r->slot_begun)
        BinderSlotUnsure(r);
    else if (rc != OK && r->taken)
        BinderSlotOwnPublish(r->slot, 0);   /* its nodes nobody's: the pair goes on where they began */
    if (rc != OK) {
        kprintf("[TagFS] the Tally of the state after stitch %llu was refused at %s (error %u): the slots hold "
                "what they held%s\n", (unsigned long long)r->k, r->stage ? r->stage : "its slice", (unsigned)rc,
                r->slot_begun ? ", and the slot it was written over is held as naming either Tally" : "");
        DogEarTakenBack();
        FoliationDraftLetGo();
        EscheatTakenBack();
    }
#if BINDER_CHECKS
    BinderCheckLetGo();
#endif
    EndpaperTallyLaid(NULL);
    BinderNodesLetGo(&r->w);
    BinderSliceLetGo(&r->w);
    kfree(r->fresh.ground);
    kfree(r->sealed);
    kfree(r->fresh.quires);
    kfree(r->kept);
    kfree(r->fids);
    BinderDigestsLetGo(r);
    memset(r, 0, sizeof(*r));
    __atomic_store_n(&s_under_way, 0u, __ATOMIC_RELEASE);
    BinderWeigh();
    BinderRoomEnded();
}

// ----------------------------------------------------------------------------
// TALLY_TAKE, a turn at a time
// ----------------------------------------------------------------------------

/* Every node no copy of reads known — the FILES ones the mount found, the
 * DIGESTS ones weighed a leaf a turn — the ground the slot will hold made
 * room for, and the walk begun with nothing written. */
static error_t TallyMeasureBegin(BinderRun* r)
{
    error_t rc = BinderFreshGround(r);
    if (rc != OK)
        return rc;
    r->w.kept      = r->kept;
    r->w.nkept     = r->nkept;
    r->w.measuring = true;
    BinderNodesBegin(&r->w);
    return OK;
}

/* Every node measured: the blocks one copy takes, its pointers laid at their
 * widest — the runs its copies go in taken, in the slot of the least seq, and
 * the walk begun again to write them. */
static error_t TallyMeasured(BinderRun* r)
{
    uint64_t blocks = r->w.off ? r->w.off : r->w.grid;
    r->slot = BinderSlotNext();
    error_t rc = TallyRunsTake(r, blocks);
    if (rc != OK)
        return rc;
    r->w.measuring = false;
    BinderNodesBegin(&r->w);
    return OK;
}

/* TALLY_TAKE a turn at a time: the slice; a DIGESTS leaf weighed — laid in,
 * one it lays or offers read, the ground it speaks for made once every one is
 * (binder_digests.c); a node measured. *taken once the runs the copies go in
 * are. */
static error_t TallyTakeStep(BinderRun* r, bool* taken)
{
    bool    done = false;
    error_t rc   = OK;
    *taken = false;
    if (!r->sliced) {
        r->sliced = true;
        return TallySliceTake(r);
    }
    if (!r->rots_read) {
        if (r->w.tip)
            rc = BinderDigestsReadNext(r, &done);
        else
            rc = TallyDigestsRottenNext(&r->rot_leaf, BinderRotOfDigests, r, &done);
        if (rc == OK && done && r->w.tip)
            rc = BinderDigestsSifted(r);
        if (rc == OK && done) {
            r->rots_read = true;
            rc           = TallyMeasureBegin(r);
        }
        return rc;
    }
    if ((rc = BinderNodeNext(&r->w, &done)) != OK || !done)
        return rc;
    rc     = TallyMeasured(r);
    *taken = rc == OK;
    return rc;
}

// ----------------------------------------------------------------------------
// The steps
// ----------------------------------------------------------------------------

static error_t TallyFlush(void)
{
    return VolumeFlushFor(VolumeFlushNeed());
}

static const char* const s_stages[] = {
    [TALLY_TAKE] = "its slice and measure", [TALLY_NODES] = "its nodes", [TALLY_FLUSH] = "the flush of its nodes",
    [TALLY_SLOT] = "its slot", [TALLY_SLOT_FLUSH] = "the flush of its slot", [TALLY_DONE] = "its DONE",
};

/* One step of the Tally under way: *done once its DONE ran. */
static error_t TallyStepRun(BinderRun* r, bool* done)
{
    bool    end = false;
    error_t rc  = OK;
    r->stage    = s_stages[r->step];
#ifdef CONFIG_VOLUME_TESTS
    if (s_stand_stop && r->step == s_stand_stop)
        return ERR_IO_CANCELLED;      /* the stand stops it here: the volume left as a cut leaves it */
#endif
    switch (r->step) {
    case TALLY_TAKE:
        if ((rc = TallyTakeStep(r, &end)) == OK && end)
            r->step = TALLY_NODES;
        break;
    case TALLY_NODES:
        if ((rc = BinderNodeNext(&r->w, &end)) == OK && end)
            r->step = TALLY_FLUSH;
        break;
    case TALLY_FLUSH:
        SETTLE_CUT_CYCLE(r->target);
        SETTLE_CUT_AT(SETTLE_CUT_POINT_TALLY);
        if ((rc = TallyFlush()) == OK)
            r->step = TALLY_SLOT;
        break;
    case TALLY_SLOT:
        if ((rc = TallySlotWrite(r)) == OK)
            r->step = TALLY_SLOT_FLUSH;
        break;
    case TALLY_SLOT_FLUSH:
        if ((rc = TallyFlush()) == OK)
            r->step = TALLY_DONE;
        break;
    case TALLY_DONE:
        if (!r->turned) {
            error_t turn = BinderDigestsTurn(&r->w);
            if (turn == ERR_BUSY)
                break;              /* a read is still in the table the turn takes: the next turn */
            r->turned   = true;
            r->captured = turn == OK;
        }
        rc    = BinderTallyDone(r);
        *done = true;
        break;
    }
    return rc;
}

/* The useful bytes the next Tally is owed at: TALLY_SLACK times what it is
 * reckoned to cost — laid in, the leaves the keys changed since meet, each at
 * what is left live in it, and their path (binder_cost.c), in both copies, and
 * its slot; never more than the last Tally's whole book, both copies and the
 * slot (§4.2, §16.5 п.16, §16.6 п.6). */
static uint64_t TallyWholeBytes(void)
{
    return (2u * g_binder.tally_blocks + 1u) * TAGFS_BLOCK_SIZE;
}

static uint64_t TallyCostBytes(void)
{
    uint64_t tip = BinderTipInBlocks();
    return tip < g_binder.tally_blocks ? (2u * tip + 1u) * TAGFS_BLOCK_SIZE : TallyWholeBytes();
}

/*
 * ‼ THE COST IS RECKONED AGAIN ONLY WHEN IT COULD BE REACHED. Between a
 * Tally's slice and the next, DogEar only gains keys and the Foliation, the
 * reads' table and the last book stand still; a leaf costs what is left live
 * in it (§16.6 п.6, binder_live.c), so the cost grows with every key DogEar
 * gains and with every key born in a leaf left empty, and shrinks only where
 * a key of the state may have gone — a file given up, a Use Context said, or
 * ground let go that leaves a leaf of DIGESTS no block of the file's nor one
 * taken since the slice (dog_ear.c, LeafMayEmpty) — each of which DogEar
 * counts (DogEarGoneCount). While none went since the cost was last
 * reckoned, useful bytes short of TALLY_SLACK times it are short of the cost
 * now, and nothing is walked: reckoned again once the useful bytes reach it —
 * each time past a cost at least TALLY_SLACK times the bytes before, a few
 * times a Tally — or at the first weigh after a key went, so a Tally a large
 * delete made cheap is owed at once. A write over a block of a file, laid on
 * fresh ground, lets go a block of a leaf the file still holds one of: no key
 * went, and a stitch of such writes walks nothing. Only one that may have
 * emptied a leaf — a file given up, a file cut short of every block of a
 * leaf, the Use Context said — walks the cost at the weigh after it. A slice
 * taken, a Tally ended and a mount stood shrink what it is reckoned by: the
 * cost is reckoned again from nothing (cost_known 0).
 */
static bool TallyDue(void)
{
    uint64_t useful = g_binder.useful;
    uint64_t gone   = DogEarGoneCount();
    if (!g_binder.up)
        return false;               /* no book stands: nothing is owed, nothing weighed */
    if (useful >= (uint64_t)TALLY_SLACK * TallyWholeBytes())
        return true;
    if (gone == g_binder.cost_gone && useful < (uint64_t)TALLY_SLACK * g_binder.cost_known)
        return false;
    g_binder.cost_known = TallyCostBytes();
    g_binder.cost_gone  = gone;
    return useful >= (uint64_t)TALLY_SLACK * g_binder.cost_known;
}

/*
 * What BinderOwed reads, published by the Settle's holder each time what it
 * weighs moves — a stitch settled, a Quire given the ribbon, a Tally ended,
 * the mount stood: whether TALLY_SLACK is reached, and why none may be begun
 * as its own state stands. A reason that will not pass with the next stitch,
 * met while a Tally is owed, is said once a mount (§16 "nothing in silence").
 */
void BinderWeigh(void)
{
    bool        due = TallyDue();
    const char* why = BinderOwnMayNot();
    __atomic_store_n(&g_binder.due, due ? 1u : 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_binder.may_not, why, __ATOMIC_RELEASE);
    BinderTurnGivesPublish();
    BinderGroundHeldPublish();
    bool owed = due || __atomic_load_n(&g_binder.demanded, __ATOMIC_ACQUIRE);
    if (owed && why == s_off_chain && g_binder.said != why) {
        g_binder.said = why;
        kprintf("[TagFS] a Tally is owed and not begun: %s\n", why);
    }
    /* Owed and held back by nothing but the tails: no Slip is given from here
     * (plot_slip.c), and the Helm's service lays each tail in its block. */
    __atomic_store_n(&g_binder.engross, owed && (why == NULL || why == s_loose) ? 1u : 0u, __ATOMIC_RELEASE);
    if (owed && why == s_loose)
        EngrossAsked();
}

bool BinderEngrossOwed(void)
{
    return __atomic_load_n(&g_binder.engross, __ATOMIC_ACQUIRE) && !BinderNoWrite();
}

bool BinderOwed(void)
{
    if (__atomic_load_n(&s_under_way, __ATOMIC_ACQUIRE))
        return true;
    bool due = __atomic_load_n(&g_binder.demanded, __ATOMIC_ACQUIRE) ||
               __atomic_load_n(&g_binder.due, __ATOMIC_ACQUIRE);
    return due && SettledCycles() >= __atomic_load_n(&g_binder.retry_after, __ATOMIC_ACQUIRE) &&
           !BinderNoWrite() && __atomic_load_n(&g_binder.may_not, __ATOMIC_ACQUIRE) == NULL;
}

bool BinderRoomWait(void)
{
    if (BinderNoTallies() || BinderNoWrite() || __atomic_load_n(&g_binder.room_turns, __ATOMIC_ACQUIRE) >= g_binder.ring)
        return false;
    if (__atomic_load_n(&s_under_way, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&g_binder.room_waits, 1u, __ATOMIC_RELEASE);   /* its end gives the slot's ground */
        return true;
    }
    if (!EndpaperGrown() && !__atomic_load_n(&g_binder.turn_gives, __ATOMIC_ACQUIRE))
        return false;
    const char* why = __atomic_load_n(&g_binder.may_not, __ATOMIC_ACQUIRE);
    if (why == s_on_edge || why == s_unsettled) {
        SettleDemand(SETTLE_DEMAND_ROOM);    /* the stitch that takes the reason away */
        return true;
    }
    if (why)
        return false;
    __atomic_store_n(&g_binder.room_waits, 1u, __ATOMIC_RELEASE);
    SettleDemand(SETTLE_DEMAND_TALLY);
    return true;
}

const char* BinderRoomMeasured(const char** why)
{
    *why = NULL;
    if (!EndpaperGrown())
        return "to the block by its last Tally";
    if (__atomic_load_n(&g_binder.room_turns, __ATOMIC_ACQUIRE) >= g_binder.ring)
        return "from above past its last Tally, a turn of the ring waited for";
    *why = BinderNoTallies() ? BinderNoTallies()
         : BinderNoWrite()   ? s_no_write
                             : __atomic_load_n(&g_binder.may_not, __ATOMIC_ACQUIRE);
    return "from above past its last Tally, no Tally waited for";
}

bool BinderRoomAwaited(void)
{
    return __atomic_load_n(&g_binder.room_waits, __ATOMIC_ACQUIRE) != 0;
}

void BinderRoomTaken(void)
{
    if (__atomic_load_n(&g_binder.room_turns, __ATOMIC_ACQUIRE))
        __atomic_store_n(&g_binder.room_turns, 0u, __ATOMIC_RELEASE);
}

/* A Tally a write short of room waited on ended: one turn of the ring spent,
 * and the write woken by a cycle to ask again. By the Settle's holder. */
void BinderRoomEnded(void)
{
    if (!__atomic_exchange_n(&g_binder.room_waits, 0u, __ATOMIC_ACQ_REL))
        return;
    __atomic_add_fetch(&g_binder.room_turns, 1u, __ATOMIC_ACQ_REL);
    SettleDemand(SETTLE_DEMAND_ROOM);
}

bool BinderHoldsGround(void)
{
    return __atomic_load_n(&s_under_way, __ATOMIC_ACQUIRE) != 0;
}

void BinderDemanded(void)
{
    __atomic_store_n(&g_binder.demanded, 1u, __ATOMIC_RELEASE);
}

void BinderStopped(error_t rc)
{
    if (s_run.active)
        TallyEnded(&s_run, rc);
}

/* One step, by a holder of the Settle's Scribe: a Tally begun first when
 * none is under way — or the one under way ended, the volume taking no write
 * from here (BinderStopped). OK and *done; or the refusal, the Tally ended
 * with it. */
static error_t BinderStep(bool* done)
{
    BinderRun* r = &s_run;
    *done = false;
    if (r->active && BinderNoWrite()) {
        TallyEnded(r, ERR_TAGFS_READ_ONLY);
        return ERR_TAGFS_READ_ONLY;
    }
    if (!r->active) {
        if (BinderMayNot()) {
            BinderWeigh();          /* what BinderOwed read was stale: the cursor moved since */
            return ERR_BUSY;
        }
        memset(r, 0, sizeof(*r));
        r->active = true;
        r->step   = TALLY_TAKE;
        r->target = SettledGen();
        __atomic_store_n(&s_under_way, 1u, __ATOMIC_RELEASE);
        __atomic_store_n(&g_binder.demanded, 0u, __ATOMIC_RELEASE);
    }
    error_t rc = TallyStepRun(r, done);
    if (rc != OK) {
        __atomic_store_n(&g_binder.retry_after, SettledCycles() + 1u, __ATOMIC_RELEASE);
        TallyEnded(r, rc);
    } else if (*done) {
        TallyEnded(r, OK);
    }
    return rc;
}

/* The Settle's Scribe taken as a pass that answers no asker: given back with
 * the cover it found, so every one waiting takes the next pass itself. */
static bool BinderScribeTake(uint64_t* written)
{
    uint64_t cover = 0;
    error_t  refused = OK;
    uint64_t ticket = ScribeAsk(&g_settle_scribe);
    if (ScribeTry(&g_settle_scribe, ticket, &cover, &refused) != SCRIBE_MINE)
        return false;
    *written = __atomic_load_n(&g_settle_scribe.written, __ATOMIC_ACQUIRE);
    return true;
}

bool BinderTurn(void)
{
    uint64_t written = 0;
    bool     done    = false;
    if (!tagfs_enter())
        return false;
    if (!BinderScribeTake(&written)) {
        tagfs_leave();
        return false;
    }
    (void)BinderStep(&done);
    ScribeGive(&g_settle_scribe, written, OK);
    tagfs_leave();
    return true;
}

/* The Tally under way, or one begun, a step a turn of the Scribe until it
 * ends, as the cadence would. */
static error_t BinderRunSteps(void)
{
    uint64_t written = 0;
    bool     done    = false;
    error_t  rc      = OK;
    while (rc == OK && !done) {
        uint64_t   cover = 0, ticket = ScribeAsk(&g_settle_scribe);
        error_t    refused = OK;
        ScribeTurn turn = ScribeWait(&g_settle_scribe, ticket, &cover, &refused);
        if (turn == SCRIBE_SELF)
            return ERR_BUSY;            /* this core holds it beneath the asker */
        if (turn != SCRIBE_MINE)
            continue;                   /* a pass covered the ask: the Tally's step is still owed */
        written = __atomic_load_n(&g_settle_scribe.written, __ATOMIC_ACQUIRE);
        rc      = BinderStep(&done);
        ScribeGive(&g_settle_scribe, written, OK);
    }
    return rc;
}

error_t BinderRunWhole(void)
{
    const char* why = s_run.active ? NULL : BinderMayNot();
    if (why == s_loose) {
        /* The caller waits for its Tally: the tails go into their blocks in
         * its own call, and the cycle that carries them settles first. */
        error_t rc = EngrossWhole();
        if (rc == OK)
            rc = TagFSSettle();
        if (rc != OK)
            return rc;
        why = BinderMayNot();
    }
    if (why) {
        kprintf("[TagFS] a Tally is asked for and not begun: %s\n", why);
        return ERR_BUSY;
    }
    return BinderRunSteps();
}

/* Never the tails' Engross: a write's wait for room is reached from inside a
 * Helm span (engross.c EngrossEscheat -> HelmSpan -> HelmRoomCameBack ->
 * SettleForRoom), and an Engross there walks the files again under it — a
 * recursion with no bound but the files. The Helm's service lays them
 * (BinderWeigh asks it), and the cadence begins the Tally after. */
error_t BinderRunAwaited(void)
{
    if (!s_run.active && BinderMayNot())
        return ERR_WOULD_BLOCK;
    return BinderRunSteps();
}

void BinderStitchSettled(uint64_t seq, uint64_t useful, uint64_t fid_most, error_t births)
{
    if (births != OK && !BinderNoTallies()) {
        BinderNoTalliesFrom("memory would not keep the births of the runs a stitch put in");
        kprintf("[TagFS] memory would not keep the births of the runs stitch %llu put in — no Tally is written "
                "by this mount: the ribbon is read whole at the next mount, and runs out of Quires before it "
                "does (ERR_TAGFS_NO_SPACE said then)\n", (unsigned long long)seq);
    }
    g_binder.useful += useful;
    if (fid_most + 1u > g_binder.laid_fid)
        g_binder.laid_fid = fid_most + 1u;
    BinderWeigh();
}

#ifdef CONFIG_VOLUME_TESTS
void BinderStandStopAt(TallyStep at)
{
    s_stand_stop = at;
}

void BinderStandTearSlot(uint32_t sectors)
{
    s_stand_tear = sectors <= TAGFS_BLOCK_SECTORS ? sectors : 0;
}

void BinderStand(BinderStandSaid* out)
{
    memset(out, 0, sizeof(*out));
    out->next_slot     = BinderSlotNext();
    out->useful        = g_binder.useful;
    out->cost          = TallyCostBytes();
    out->tally_blocks  = g_binder.tally_blocks;
    out->tallies       = g_binder.tallies;
    out->owed          = BinderOwed();
    out->demanded      = __atomic_load_n(&g_binder.demanded, __ATOMIC_ACQUIRE) != 0;
    out->pair_left     = g_binder.pair_room > g_binder.pair_used ? 2u * (g_binder.pair_room - g_binder.pair_used) : 0;
    out->checked       = BinderCheckHeld();
    uint64_t from      = UINT64_MAX;
    for (uint32_t i = 0; i < g_binder.nslots; i++) {
        const SlotGround* s = &g_binder.slot[i];
        if (s->holds && s->chain_from < from)
            from = s->chain_from;
        out->ribbon_quires += s->holds ? s->nextra : 0u;
    }
    if (from != UINT64_MAX && from >= g_binder.chain_base)
        out->ribbon_quires += g_binder.chain_base + g_binder.nchain - from;
}
#endif
