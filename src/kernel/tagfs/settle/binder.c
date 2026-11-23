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
#include "../dedup/dedup.h"
#include "loose_leaves.h"
#include "../../../kernel/drivers/timer/rtc.h"
#include "tagfs_volume.h"

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

bool BinderNoWrite(TagFSVolume* vol)
{
    return TagFSReadOnly(vol) || TagFSIsUnderGlass(vol) || StitchBookRefused(vol) || TagFSMediumLeft(vol) || SettleHalting(vol);
}

/* Why a Tally may not be begun as the holder's own state stands, or NULL — by
 * the Settle's holder, who alone moves what it reads (the cursor, the chain). */
static const char* BinderOwnMayNot(TagFSVolume* vol)
{
    const RibbonCursor* c = RibbonCursorNow(vol);
    if (!vol->binder.up || !c->up)
        return s_not_up;
    if (BinderNoTallies(vol))
        return BinderNoTallies(vol);
    /* This mount's own count: the book stands and is weighed (tagfs.c
     * MountReadBook) before the Settle's count is reset (SettleMountReset), so
     * SettledCycles there is the count of the mount before. */
    if (vol->binder.stitches == 0)
        return s_unsettled;
    if (!RibbonQuireSame(&c->quire, &c->ended))
        return s_on_edge;
    if (BinderChainIndex(vol, &c->quire) == UINT64_MAX)
        return s_off_chain;
    if (LooseLeaves(vol) > 0)
        return s_loose;      /* a Tally carries no Slip: its slice would lose the tail */
    return NULL;
}

/* Why a Tally may not be begun now, or NULL: by the Settle's holder. */
static const char* BinderMayNot(TagFSVolume* vol)
{
    return BinderNoWrite(vol) ? s_no_write : BinderOwnMayNot(vol);
}

/* Where the next Tally's nodes go on in the pair: past the ones laid, on an
 * atom. */
static uint64_t PairNext(TagFSVolume* vol, uint64_t grid)
{
    return (vol->binder.pair_used + grid - 1u) / grid * grid;
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
static error_t TallyRunsTake(TagFSVolume* vol, BinderRun* r, uint64_t blocks)
{
    uint64_t q    = RibbonCursorNow(vol)->quire_blocks;
    uint64_t data = vol->book.frame.data_block;
    uint64_t at   = PairNext(vol, r->w.grid);
    if (vol->binder.pair_room && at + blocks <= vol->binder.pair_room) {
        r->w.a    = vol->binder.pair_a + at;
        r->w.b    = vol->binder.pair_b + at;
        r->w.room = vol->binder.pair_room - at;
        r->taken  = true;
        BinderSlotOwnPublish(vol, r->slot, 0);   /* the pair's: nothing off the free */
        return OK;
    }
    uint64_t run  = (blocks + q - 1u) / q * q, a = 0, b = 0;
    uint64_t keep = BinderRingShortBut(vol, 2u * run, q, r->slot);
    error_t  rc   = BlockMapTakeQuire(vol, GROUND_TALLY, run, (uint32_t)q, keep + run, data, data, &a);
    if (rc != OK)
        return rc;
    rc = BlockMapTakeQuire(vol, GROUND_TALLY, run, (uint32_t)q, keep, data, a + run, &b);
    if (rc != OK) {
        (void)BlockMapDerivedGiveAs(vol, GROUND_TALLY, 0, a, run);
        return rc;
    }
    BinderPairLetGo(vol, q);
    vol->binder.pair_a    = r->w.a = a;
    vol->binder.pair_b    = r->w.b = b;
    vol->binder.pair_room = r->w.room = run;
    vol->binder.pair_used = 0;
    r->taken = true;
    BinderSlotOwnPublish(vol, r->slot, 2u * run);   /* the pair whole, as the free gave it */
    return OK;
}

void BinderPairUsed(TagFSVolume* vol, const BinderRun* r)
{
    if (r->taken && r->w.a >= vol->binder.pair_a && r->w.a - vol->binder.pair_a + r->w.off > vol->binder.pair_used)
        vol->binder.pair_used = r->w.a - vol->binder.pair_a + r->w.off;
}

/* The slot's numbers (§4.5 п.8): past every fid, tag and bookmark the medium
 * has shown — the mount's, the stitches' of this mount, the slice's. */
static void TallyCounts(TagFSVolume* vol, BinderRun* r)
{
    uint64_t fid = vol->binder.seen_fid > vol->binder.laid_fid ? vol->binder.seen_fid : vol->binder.laid_fid;
    for (BinderFiles* c = r->w.files; c; c = c->next)
        if (c->n && c->f[c->n - 1u].fid + 1u > fid)
            fid = c->f[c->n - 1u].fid + 1u;
    r->counts = (SpineCounts){ .next_fid = fid,
                               .next_tag = vol->binder.seen_tag > r->w.tag_hi ? vol->binder.seen_tag : r->w.tag_hi,
                               .next_bookmark = vol->binder.seen_bookmark };
}

/* The slice of the state after k — the files of the leaves a Tally laid in
 * lays, every file of a whole one — and the ground DIGESTS speaks for: memory
 * alone, no read of the medium — one turn of the holder, so no stitch settles
 * inside it, and as long as a walk of every live file's runs takes. The
 * Tally's seq is taken here: every pointer and birth_delta of it is spelled
 * from it, and a measure of other numbers is no bound. */
static error_t TallySliceTake(TagFSVolume* vol, BinderRun* r)
{
    BinderWork*         w = &r->w;
    const RibbonCursor* c = RibbonCursorNow(vol);
    uint32_t            lo = 0, hi = 0;
    r->k         = c->last;
    r->clasp     = c->clasp;
    r->ribbon    = (SpineRibbon){ .at = c->quire.block + c->off / TAGFS_BLOCK_SECTORS, .quire_block = c->quire.block,
                                  .quire_blocks = c->quire.blocks, .next_block = c->named.block,
                                  .next_blocks = c->named.blocks };
    r->chain_at  = BinderChainIndex(vol, &c->quire);
    r->useful_at = vol->binder.useful;
    r->made_ns   = rtc_get_unix64() * 1000000000ull;
    EscheatTake(vol);
    DogEarTake(vol);
    vol->binder.cost_known = 0;
    TagRegistryCoinedRange(vol->state.registry, &lo, &hi);
    w->tag_hi  = lo;
    w->tag_gen = SettledGen(vol);
    w->use    = tagfs_recall_use_context(vol, NULL);
    w->name   = VolumeNameHeld(vol);
    w->grid   = vol->book.atom > TAGFS_BLOCK_SIZE ? vol->book.atom / TAGFS_BLOCK_SIZE : 1u;
    w->seq    = RibbonSeqNext(vol);
    error_t rc = BinderTipInTake(vol, r);
    if (rc == OK)
        rc = BinderSliceTake(vol, w, r->fids, r->nfids);
    if (rc == OK)
        rc = BinderGroundTake(vol, w);
    if (rc == OK) {
        TallyCounts(vol, r);
        EndpaperTallyBegun(vol, w->seq, r->counts.next_fid);
    }
    return rc;
}

// ----------------------------------------------------------------------------
// TALLY_SLOT: the slot of the least seq
// ----------------------------------------------------------------------------

/* The slot's stream: SpineHead, a root a tree laid, SpineRibbon, SpineCounts,
 * SpineFlags when the medium lied — into `out`, the starts of its records
 * into `starts`. Its length. */
static uint32_t SlotStream(TagFSVolume* vol, const BinderRun* r, uint8_t* out, uint32_t* starts, uint32_t* n)
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
    if (RibbonCursorLied(vol)) {
        starts[(*n)++] = at;
        at += SpineFlagsPut(out + at, SPINE_MEDIUM_LIED);
    }
    return at;
}

/* The slot's sectors from `first`, `n` of them, to the medium. */
static error_t SlotSectorsWrite(TagFSVolume* vol, uint64_t vlba, uint32_t first, uint32_t n, const uint8_t* unit)
{
    int wrote = tagfs_volume_write(vol, vlba + first, n, unit + (uint64_t)first * BOOK_SECTOR_BYTES);
    return wrote == 0 ? OK : wrote == (int)ERR_DEVICE_NOT_READY ? ERR_DEVICE_NOT_READY : ERR_WRITE_FAILED;
}

/*
 * Where the slot's write is torn on purpose: nowhere in a machine's own life —
 * a slot is one block of the volume, written whole, which a medium addressed
 * in 4096-byte blocks takes as one and any other may tear wherever it tears.
 * The stand tears it after its first `binding.stand_tear` sectors, and a kernel
 * built to stop inside a slot (SETTLECUT `slot`) after its first half (C9):
 * the reader is held to the tears a medium could make, whichever way the
 * slot was written.
 */
static uint32_t SlotTornAt(void)
{
#ifdef CONFIG_VOLUME_TESTS
    if (s_stand_tear)
        return s_stand_tear;
#endif
#ifdef CONFIG_TAGFS_SETTLE_CUT_POINT
    if (CONFIG_TAGFS_SETTLE_CUT_POINT == SETTLE_CUT_POINT_SLOT)
        return TAGFS_BLOCK_SECTORS / 2u;
#endif
    return TAGFS_BLOCK_SECTORS;
}

/*
 * The slot written over: its stream sealed at a seq past the Tally's, whole
 * (SlotTornAt). From the first sector issued, the slot may hold either Tally
 * (slot_begun) — the new one as sealed here, kept in r->sealed until the
 * Tally ends, and by the slot itself when it is held unsure.
 */
static error_t TallySlotWrite(TagFSVolume* vol, BinderRun* r)
{
    uint8_t* stream = kmalloc(TALLY_SHEET_PAYLOAD);
    uint32_t starts[TALLY_SHARED + 6u], n = 0, half = SlotTornAt();
    r->sealed       = kmalloc(sizeof(SlotSealed));
    error_t  rc     = stream && r->sealed ? OK : ERR_NO_MEMORY;
    if (rc == OK) {
        uint8_t* unit = r->sealed->unit;
        r->sealed->next = NULL;
        memset(stream, 0, TALLY_SHEET_PAYLOAD);
        uint32_t len   = SlotStream(vol, r, stream, starts, &n);
        uint64_t block = vol->book.frame.spine_block + (uint64_t)r->slot * vol->book.frame.spine_stride;
        uint64_t vlba  = block * TAGFS_BLOCK_SECTORS;
        r->slot_seq    = RibbonSeqNext(vol);
        TallyUnitSeal(unit, stream, len, starts, n, BOOK_UNIT_SLOT, r->slot_seq, vlba, TAGFS_BLOCK_SECTORS,
                      vol->state.uuid);
        r->slot_begun = true;
        rc = SlotSectorsWrite(vol, vlba, 0, half, unit);
#ifdef CONFIG_VOLUME_TESTS
        if (rc == OK && s_stand_tear)
            rc = ERR_IO_CANCELLED;          /* the stand tears it here */
#endif
        SETTLE_CUT_CYCLE(r->target);
        if (rc == OK)
            SETTLE_CUT_AT(SETTLE_CUT_POINT_SLOT);
        if (rc == OK && half < TAGFS_BLOCK_SECTORS)
            rc = SlotSectorsWrite(vol, vlba, half, TAGFS_BLOCK_SECTORS - half, unit);
    }
    kfree(stream);
    return rc;
}

// ----------------------------------------------------------------------------
// TALLY_DONE, and a Tally refused
// ----------------------------------------------------------------------------

/* The Tally under way ended without its DONE: its runs given back when no
 * slot can name them, held when the slot's write began; everything let go. */
static void TallyEnded(TagFSVolume* vol, BinderRun* r, error_t rc)
{
    if (rc != OK && r->taken && r->slot_begun)
        BinderSlotUnsure(vol, r);
    else if (rc != OK && r->taken)
        BinderSlotOwnPublish(vol, r->slot, 0);   /* its nodes nobody's: the pair goes on where they began */
    if (rc != OK) {
        kprintf("[TagFS] the Tally of the state after stitch %llu was refused at %s (error %u): the slots hold "
                "what they held%s\n", (unsigned long long)r->k, r->stage ? r->stage : "its slice", (unsigned)rc,
                r->slot_begun ? ", and the slot it was written over is held as naming either Tally" : "");
        DogEarTakenBack(vol);
        FoliationDraftLetGo(vol);
        EscheatTakenBack(vol);
    }
#if BINDER_CHECKS
    BinderCheckLetGo(vol);
#endif
    EndpaperTallyLaid(vol, NULL);
    BinderNodesLetGo(&r->w);
    BinderSliceLetGo(&r->w);
    kfree(r->fresh.ground);
    kfree(r->sealed);
    kfree(r->fresh.quires);
    kfree(r->kept);
    kfree(r->fids);
    BinderDigestsLetGo(r);
    memset(r, 0, sizeof(*r));
    __atomic_store_n(&vol->binding.under_way, 0u, __ATOMIC_RELEASE);
    BinderWeigh(vol);
    BinderRoomEnded(vol);
}

// ----------------------------------------------------------------------------
// TALLY_TAKE, a turn at a time
// ----------------------------------------------------------------------------

/* Every node no copy of reads known — the FILES ones the mount found, the
 * DIGESTS ones weighed a leaf a turn — the ground the slot will hold made
 * room for, and the walk begun with nothing written. */
static error_t TallyMeasureBegin(TagFSVolume* vol, BinderRun* r)
{
    error_t rc = BinderFreshGround(vol, r);
    if (rc != OK)
        return rc;
    r->w.kept      = r->kept;
    r->w.nkept     = r->nkept;
    r->w.measuring = true;
    BinderNodesBegin(vol, &r->w);
    return OK;
}

/* Every node measured: the blocks one copy takes, its pointers laid at their
 * widest — the runs its copies go in taken, in the slot of the least seq, and
 * the walk begun again to write them. */
static error_t TallyMeasured(TagFSVolume* vol, BinderRun* r)
{
    uint64_t blocks = r->w.off ? r->w.off : r->w.grid;
    r->slot = BinderSlotNext(vol);
    error_t rc = TallyRunsTake(vol, r, blocks);
    if (rc != OK)
        return rc;
    r->w.measuring = false;
    BinderNodesBegin(vol, &r->w);
    return OK;
}

/* TALLY_TAKE a turn at a time: the slice; a DIGESTS leaf weighed — laid in,
 * one it lays or offers read, the ground it speaks for made once every one is
 * (binder_digests.c); a node measured. *taken once the runs the copies go in
 * are. */
static error_t TallyTakeStep(TagFSVolume* vol, BinderRun* r, bool* taken)
{
    bool    done = false;
    error_t rc   = OK;
    *taken = false;
    if (!r->sliced) {
        r->sliced = true;
        return TallySliceTake(vol, r);
    }
    if (!r->rots_read) {
        if (r->w.tip)
            rc = BinderDigestsReadNext(vol, r, &done);
        else
            rc = TallyDigestsRottenNext(vol, &r->rot_leaf, BinderRotOfDigests, r, &done);
        if (rc == OK && done && r->w.tip)
            rc = BinderDigestsSifted(vol, r);
        if (rc == OK && done) {
            r->rots_read = true;
            rc           = TallyMeasureBegin(vol, r);
        }
        return rc;
    }
    if ((rc = BinderNodeNext(vol, &r->w, &done)) != OK || !done)
        return rc;
    rc     = TallyMeasured(vol, r);
    *taken = rc == OK;
    return rc;
}

// ----------------------------------------------------------------------------
// The steps
// ----------------------------------------------------------------------------

static error_t TallyFlush(TagFSVolume* vol)
{
    return VolumeFlushFor(vol, VolumeFlushNeed(vol));
}

static const char* const s_stages[] = {
    [TALLY_TAKE] = "its slice and measure", [TALLY_NODES] = "its nodes", [TALLY_FLUSH] = "the flush of its nodes",
    [TALLY_SLOT] = "its slot", [TALLY_SLOT_FLUSH] = "the flush of its slot", [TALLY_DONE] = "its DONE",
};

/* One step of the Tally under way: *done once its DONE ran. */
static error_t TallyStepRun(TagFSVolume* vol, BinderRun* r, bool* done)
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
        if ((rc = TallyTakeStep(vol, r, &end)) == OK && end)
            r->step = TALLY_NODES;
        break;
    case TALLY_NODES:
        if ((rc = BinderNodeNext(vol, &r->w, &end)) == OK && end)
            r->step = TALLY_FLUSH;
        break;
    case TALLY_FLUSH:
        SETTLE_CUT_CYCLE(r->target);
        SETTLE_CUT_AT(SETTLE_CUT_POINT_TALLY);
        if ((rc = TallyFlush(vol)) == OK)
            r->step = TALLY_SLOT;
        break;
    case TALLY_SLOT:
        if ((rc = TallySlotWrite(vol, r)) == OK)
            r->step = TALLY_SLOT_FLUSH;
        break;
    case TALLY_SLOT_FLUSH:
        if ((rc = TallyFlush(vol)) == OK)
            r->step = TALLY_DONE;
        break;
    case TALLY_DONE:
        if (!r->turned) {
            error_t turn = BinderDigestsTurn(vol, &r->w);
            if (turn == ERR_BUSY)
                break;              /* a read is still in the table the turn takes: the next turn */
            r->turned   = true;
            r->captured = turn == OK;
        }
        rc    = BinderTallyDone(vol, r);
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
static uint64_t TallyWholeBytes(TagFSVolume* vol)
{
    return (2u * vol->binder.tally_blocks + 1u) * TAGFS_BLOCK_SIZE;
}

static uint64_t TallyCostBytes(TagFSVolume* vol)
{
    uint64_t tip = BinderTipInBlocks(vol);
    return tip < vol->binder.tally_blocks ? (2u * tip + 1u) * TAGFS_BLOCK_SIZE : TallyWholeBytes(vol);
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
static bool TallyDue(TagFSVolume* vol)
{
    uint64_t useful = vol->binder.useful;
    uint64_t gone   = DogEarGoneCount(vol);
    if (!vol->binder.up)
        return false;               /* no book stands: nothing is owed, nothing weighed */
    if (useful >= (uint64_t)TALLY_SLACK * TallyWholeBytes(vol))
        return true;
    if (gone == vol->binder.cost_gone && useful < (uint64_t)TALLY_SLACK * vol->binder.cost_known)
        return false;
    vol->binder.cost_known = TallyCostBytes(vol);
    vol->binder.cost_gone  = gone;
    return useful >= (uint64_t)TALLY_SLACK * vol->binder.cost_known;
}

/*
 * What BinderOwed reads, published by the Settle's holder each time what it
 * weighs moves — a stitch settled, a Quire given the ribbon, a Tally ended,
 * the mount stood: whether TALLY_SLACK is reached, and why none may be begun
 * as its own state stands. A reason that will not pass with the next stitch,
 * met while a Tally is owed, is said once a mount (§16 "nothing in silence").
 */
void BinderWeigh(TagFSVolume* vol)
{
    bool        due = TallyDue(vol);
    const char* why = BinderOwnMayNot(vol);
    __atomic_store_n(&vol->binder.due, due ? 1u : 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&vol->binder.may_not, why, __ATOMIC_RELEASE);
    BinderTurnGivesPublish(vol);
    BinderGroundHeldPublish(vol);
    DedupWeigh(vol);      /* a pass of Dedup's that waits for the ribbon's room goes on */
    bool owed = due || __atomic_load_n(&vol->binder.demanded, __ATOMIC_ACQUIRE);
    if (owed && why == s_off_chain && vol->binder.said != why) {
        vol->binder.said = why;
        kprintf("[TagFS] a Tally is owed and not begun: %s\n", why);
    }
    /* Owed and held back by nothing but the tails: no Slip is given from here
     * (plot_slip.c), and the Helm's service lays each tail in its block. */
    __atomic_store_n(&vol->binder.engross, owed && (why == NULL || why == s_loose) ? 1u : 0u, __ATOMIC_RELEASE);
    if (owed && why == s_loose)
        EngrossAsked(vol);
    /* An act asked of a Tally has no waiter to ask again, as a write short of
     * room has (BinderRoomAsk): held back by what the next stitch takes away,
     * its holder asks that stitch — a stitch the ask's cycle laid may end on
     * its Quire's last sector, and nobody writing, no cycle comes. */
    if ((why == s_unsettled || why == s_on_edge) && __atomic_load_n(&vol->binder.demanded, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&vol->binding.under_way, __ATOMIC_ACQUIRE) && EscheatOwed(vol))
        SettleDemand(vol, SETTLE_DEMAND_ROOM);
}

bool BinderEngrossOwed(TagFSVolume* vol)
{
    return __atomic_load_n(&vol->binder.engross, __ATOMIC_ACQUIRE) && !BinderNoWrite(vol);
}

bool BinderOwed(TagFSVolume* vol)
{
    if (__atomic_load_n(&vol->binding.under_way, __ATOMIC_ACQUIRE))
        return true;
    bool due = __atomic_load_n(&vol->binder.demanded, __ATOMIC_ACQUIRE) ||
               __atomic_load_n(&vol->binder.due, __ATOMIC_ACQUIRE);
    return due && SettledCycles(vol) >= __atomic_load_n(&vol->binder.retry_after, __ATOMIC_ACQUIRE) &&
           !BinderNoWrite(vol) && __atomic_load_n(&vol->binder.may_not, __ATOMIC_ACQUIRE) == NULL;
}

#ifdef CONFIG_VOLUME_TESTS
static volatile bool s_stand_no_room_wait;
static const char    s_stand_held_down[] = "the stand holds the wall's wait on a Tally down";

bool BinderStandNoRoomWait(bool armed)
{
    return __atomic_exchange_n(&s_stand_no_room_wait, armed, __ATOMIC_ACQ_REL);
}
#endif

/* A wait on a Tally that `gives` what the waiter lacks, if it is begun: the
 * one under way, or one asked — the cycle that takes the reason away asked
 * where none may be begun until the next stitch — within the turn of the ring
 * since a waiter was last given whole what it waited for. */
static bool BinderRoomAsk(TagFSVolume* vol, bool gives)
{
#ifdef CONFIG_VOLUME_TESTS
    if (__atomic_load_n(&s_stand_no_room_wait, __ATOMIC_ACQUIRE))
        return false;
#endif
    if (BinderNoTallies(vol) || BinderNoWrite(vol) || __atomic_load_n(&vol->binder.room_turns, __ATOMIC_ACQUIRE) >= vol->binder.ring)
        return false;
    if (__atomic_load_n(&vol->binding.under_way, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&vol->binder.room_waits, 1u, __ATOMIC_RELEASE);   /* its end gives the slot's ground */
        return true;
    }
    if (!gives)
        return false;
    const char* why = __atomic_load_n(&vol->binder.may_not, __ATOMIC_ACQUIRE);
    if (why == s_on_edge || why == s_unsettled) {
        SettleDemand(vol, SETTLE_DEMAND_ROOM);    /* the stitch that takes the reason away */
        return true;
    }
    if (why)
        return false;
    __atomic_store_n(&vol->binder.room_waits, 1u, __ATOMIC_RELEASE);
    SettleDemand(vol, SETTLE_DEMAND_TALLY);
    return true;
}

bool BinderRoomWait(TagFSVolume* vol)
{
    return BinderRoomAsk(vol, EndpaperGrown(vol) || __atomic_load_n(&vol->binder.turn_gives, __ATOMIC_ACQUIRE));
}

bool BinderRibbonWait(TagFSVolume* vol, bool barred)
{
    return BinderRoomAsk(vol, barred || EndpaperGrown(vol) || __atomic_load_n(&vol->binder.turn_gives, __ATOMIC_ACQUIRE));
}

const char* BinderRoomMeasured(TagFSVolume* vol, const char** why)
{
    *why = NULL;
#ifdef CONFIG_VOLUME_TESTS
    if (__atomic_load_n(&s_stand_no_room_wait, __ATOMIC_ACQUIRE))
        *why = s_stand_held_down;    /* BinderRoomWait waited for no Tally and no turn of the ring */
#endif
    if (!EndpaperGrown(vol))
        return "to the block by its last Tally";
    if (__atomic_load_n(&vol->binder.room_turns, __ATOMIC_ACQUIRE) >= vol->binder.ring)
        return "from above past its last Tally, a turn of the ring waited for";
    const char* not_now = BinderNoTallies(vol) ? BinderNoTallies(vol)
                        : BinderNoWrite(vol)   ? s_no_write
                                            : __atomic_load_n(&vol->binder.may_not, __ATOMIC_ACQUIRE);
    if (not_now)
        *why = not_now;
    return "from above past its last Tally, no Tally waited for";
}

bool BinderRoomAwaited(TagFSVolume* vol)
{
    return __atomic_load_n(&vol->binder.room_waits, __ATOMIC_ACQUIRE) != 0;
}

void BinderRoomTaken(TagFSVolume* vol)
{
    if (__atomic_load_n(&vol->binder.room_turns, __ATOMIC_ACQUIRE))
        __atomic_store_n(&vol->binder.room_turns, 0u, __ATOMIC_RELEASE);
}

/* A Tally a write short of room waited on ended: one turn of the ring spent,
 * and the write woken by a cycle to ask again. By the Settle's holder. */
void BinderRoomEnded(TagFSVolume* vol)
{
    if (!__atomic_exchange_n(&vol->binder.room_waits, 0u, __ATOMIC_ACQ_REL))
        return;
    __atomic_add_fetch(&vol->binder.room_turns, 1u, __ATOMIC_ACQ_REL);
    SettleDemand(vol, SETTLE_DEMAND_ROOM);
}

bool BinderHoldsGround(TagFSVolume* vol)
{
    return __atomic_load_n(&vol->binding.under_way, __ATOMIC_ACQUIRE) != 0;
}

void BinderDemanded(TagFSVolume* vol)
{
    __atomic_store_n(&vol->binder.demanded, 1u, __ATOMIC_RELEASE);
}

void BinderTallyAsked(TagFSVolume* vol)
{
    BinderDemanded(vol);
    if (BinderOwed(vol))
        SettleKnock();
    else
        SettleDemand(vol, SETTLE_DEMAND_ROOM);
}

void BinderStopped(TagFSVolume* vol, error_t rc)
{
    if (vol->binding.run.active)
        TallyEnded(vol, &vol->binding.run, rc);
}

/* One step, by a holder of the Settle's Scribe: a Tally begun first when
 * none is under way — or the one under way ended, the volume taking no write
 * from here (BinderStopped). OK and *done; or the refusal, the Tally ended
 * with it. */
static error_t BinderStep(TagFSVolume* vol, bool* done)
{
    BinderRun* r = &vol->binding.run;
    *done = false;
    if (r->active && BinderNoWrite(vol)) {
        TallyEnded(vol, r, ERR_TAGFS_READ_ONLY);
        return ERR_TAGFS_READ_ONLY;
    }
    if (!r->active) {
        if (BinderMayNot(vol)) {
            BinderWeigh(vol);          /* what BinderOwed read was stale: the cursor moved since */
            return ERR_BUSY;
        }
        memset(r, 0, sizeof(*r));
        r->active = true;
        r->step   = TALLY_TAKE;
        r->target = SettledGen(vol);
        __atomic_store_n(&vol->binding.under_way, 1u, __ATOMIC_RELEASE);
        __atomic_store_n(&vol->binder.demanded, 0u, __ATOMIC_RELEASE);
    }
    error_t rc = TallyStepRun(vol, r, done);
    if (rc != OK) {
        __atomic_store_n(&vol->binder.retry_after, SettledCycles(vol) + 1u, __ATOMIC_RELEASE);
        TallyEnded(vol, r, rc);
    } else if (*done) {
        TallyEnded(vol, r, OK);
    }
    return rc;
}

/* The Settle's Scribe taken as a pass that answers no asker: given back with
 * the cover it found, so every one waiting takes the next pass itself. */
static bool BinderScribeTake(TagFSVolume* vol, uint64_t* written)
{
    uint64_t cover = 0;
    error_t  refused = OK;
    uint64_t ticket = ScribeAsk(&vol->settle.scribe);
    if (ScribeTry(&vol->settle.scribe, ticket, &cover, &refused) != SCRIBE_MINE)
        return false;
    *written = __atomic_load_n(&vol->settle.scribe.written, __ATOMIC_ACQUIRE);
    return true;
}

bool BinderTurn(TagFSVolume* vol)
{
    uint64_t written = 0;
    bool     done    = false;
    if (!tagfs_enter(vol))
        return false;
    if (!BinderScribeTake(vol, &written)) {
        tagfs_leave(vol);
        return false;
    }
    (void)BinderStep(vol, &done);
    ScribeGive(&vol->settle.scribe, written, OK);
    tagfs_leave(vol);
    return true;
}

/* The Tally under way, or one begun, a step a turn of the Scribe until it
 * ends, as the cadence would. */
static error_t BinderRunSteps(TagFSVolume* vol)
{
    uint64_t written = 0;
    bool     done    = false;
    error_t  rc      = OK;
    while (rc == OK && !done) {
        uint64_t   cover = 0, ticket = ScribeAsk(&vol->settle.scribe);
        error_t    refused = OK;
        ScribeTurn turn = ScribeWait(&vol->settle.scribe, ticket, &cover, &refused);
        if (turn == SCRIBE_SELF)
            return ERR_BUSY;            /* this core holds it beneath the asker */
        if (turn != SCRIBE_MINE)
            continue;                   /* a pass covered the ask: the Tally's step is still owed */
        written = __atomic_load_n(&vol->settle.scribe.written, __ATOMIC_ACQUIRE);
        rc      = BinderStep(vol, &done);
        ScribeGive(&vol->settle.scribe, written, OK);
    }
    return rc;
}

error_t BinderRunWhole(TagFSVolume* vol)
{
    const char* why = vol->binding.run.active ? NULL : BinderMayNot(vol);
    if (why == s_loose) {
        /* The caller waits for its Tally: the tails go into their blocks in
         * its own call, and the cycle that carries them settles first. */
        error_t rc = EngrossWhole(vol);
        if (rc == OK)
            rc = TagFSSettle(vol);
        if (rc != OK)
            return rc;
        why = BinderMayNot(vol);
    }
    if (why) {
        kprintf("[TagFS] a Tally is asked for and not begun: %s\n", why);
        return ERR_BUSY;
    }
    return BinderRunSteps(vol);
}

/* Never the tails' Engross: a write's wait for room is reached from inside a
 * Helm span (engross.c EngrossEscheat -> HelmSpan -> HelmRoomCameBack ->
 * SettleForRoom), and an Engross there walks the files again under it — a
 * recursion with no bound but the files. The Helm's service lays them
 * (BinderWeigh asks it), and the cadence begins the Tally after. */
error_t BinderRunAwaited(TagFSVolume* vol)
{
    if (!vol->binding.run.active && BinderMayNot(vol))
        return ERR_WOULD_BLOCK;
    return BinderRunSteps(vol);
}

void BinderStitchSettled(TagFSVolume* vol, uint64_t seq, uint64_t useful, uint64_t fid_most, error_t births)
{
    if (births != OK && !BinderNoTallies(vol)) {
        BinderNoTalliesFrom(vol, "memory would not keep the births of the runs a stitch put in");
        kprintf("[TagFS] memory would not keep the births of the runs stitch %llu put in — no Tally is written "
                "by this mount: the ribbon is read whole at the next mount, and runs out of Quires before it "
                "does (ERR_TAGFS_NO_SPACE said then)\n", (unsigned long long)seq);
    }
    vol->binder.useful += useful;
    vol->binder.stitches++;
    if (fid_most + 1u > vol->binder.laid_fid)
        vol->binder.laid_fid = fid_most + 1u;
    BinderWeigh(vol);
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

void BinderStand(TagFSVolume* vol, BinderStandSaid* out)
{
    memset(out, 0, sizeof(*out));
    out->next_slot     = BinderSlotNext(vol);
    out->useful        = vol->binder.useful;
    out->cost          = TallyCostBytes(vol);
    out->tally_blocks  = vol->binder.tally_blocks;
    out->tallies       = vol->binder.tallies;
    out->owed          = BinderOwed(vol);
    out->demanded      = __atomic_load_n(&vol->binder.demanded, __ATOMIC_ACQUIRE) != 0;
    out->pair_left     = vol->binder.pair_room > vol->binder.pair_used ? 2u * (vol->binder.pair_room - vol->binder.pair_used) : 0;
    out->checked       = BinderCheckHeld(vol);
    uint64_t from      = UINT64_MAX;
    for (uint32_t i = 0; i < vol->binder.nslots; i++) {
        const SlotGround* s = &vol->binder.slot[i];
        if (s->holds && s->chain_from < from)
            from = s->chain_from;
        out->ribbon_quires += s->holds ? s->nextra : 0u;
    }
    if (from != UINT64_MAX && from >= vol->binder.chain_base)
        out->ribbon_quires += vol->binder.chain_base + vol->binder.nchain - from;
}
#endif
