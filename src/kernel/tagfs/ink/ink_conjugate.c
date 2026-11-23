#include "ink_internal.h"
#include "commonhold.h"
#include "../settle/sewing.h"
#include "../file_map/tail_slip.h"
#include "tagfs_volume.h"

/*
 * Conjugate's Ink (ink.h, InkConjugate): the act compared the blocks with no
 * lock held (dedup/conjugate.c); here they are laid, an Ink like a Plot's —
 * before the lock the record the splice builds, the Fallow node of the
 * duplicate's own ground and the room Commonhold's change takes; under the
 * lock the guards, then the change, nothing asked of the heap.
 *
 * ‼ THE GROUND LET GO IS THE GROUND THE RANGE NAMED, ONE OWNER OFF EACH NAME
 * (§8.2). The splice's own reckoning of what the record stops naming is the
 * ground the old record names and the new one does not — a set — and a block
 * of the duplicate that the target names too (the target laid on the
 * duplicate before, or a file laid on itself) would be in neither: put back by
 * the run SHARED, it would keep an owner its DATA name no longer is. So the
 * node is made of every block the range named, and Commonhold takes one owner
 * off each and puts one on each block the run SHARED names: what names a
 * block is counted as names, never as a set.
 *
 * ‼ COMMONHOLD'S ROOM IS COUNTED AS IT STANDS, THE TARGET SHARED IN THE ORDER
 * OF ITS BLOCKS. A run shared puts in a run of its own for each stretch of its
 * blocks no run of Commonhold holds yet — one more than the runs it meets —
 * and cuts at most the two it ends inside. Taken in order of their first
 * block, every run shared before a run R and reaching into it covers R from
 * its first block on whole, so R's stretches lie past them where nothing
 * changed: the room is the runs the target's Strips meet now, and three a
 * Strip, with two a run let go for the node's cut (CommonholdLetGoLocked) —
 * counted again under the lock, an Ink that finds it short going round.
 */

typedef struct {
    FileRecord* rec;
    FallowNode* fallow;
    Strip*      order;     /* the target's ground in order of its first block, as Commonhold takes it */
    uint64_t    claim;
} InkConjugateWork;

static int StripByStart(const void* a, const void* b, void* ctx)
{
    (void)ctx;
    uint64_t x = ((const Strip*)a)->start, y = ((const Strip*)b)->start;
    return x < y ? -1 : x > y;
}

/* The runs Commonhold's change may add, as it stands now. */
static uint64_t InkConjugateRoom(TagFSVolume* vol, const InkConjugation* c, const InkConjugateWork* w)
{
    uint64_t pieces = 0, splits = 0;
    CommonholdCuts(vol, w->order, c->nonto, &pieces, &splits);
    return pieces + 3u * (uint64_t)c->nonto + 2u * (uint64_t)c->ndup;
}

static void InkConjugateWorkLetGo(InkConjugateWork* w)
{
    FileRecordRelease(w->rec);
    FallowNodeLetGo(w->fallow);
    kfree(w->order);
}

/* Before the lock: the record, the node, the order and the rooms. */
static error_t InkConjugateBuilt(TagFSVolume* vol, const InkConjugation* c, InkConjugateWork* w)
{
    BookSplice sp = { .at = c->at, .remove = c->count, .ninsert = c->nonto, .insert = c->onto };
    error_t    rc = FileRecordSplice(vol, c->cur, &sp, 1, c->cur->size, c->cur->modified_time, &w->rec, NULL, NULL);
    if (rc == OK && !(w->fallow = FallowNodeMake(vol, c->dup, c->ndup, FALLOW_BY_RECORD, c->fid)))
        rc = ERR_NO_MEMORY;
    if (rc == OK && !(w->order = kmalloc(sizeof(Strip) * c->nonto)))
        rc = ERR_NO_MEMORY;
    if (rc == OK) {
        memcpy(w->order, c->onto, sizeof(Strip) * c->nonto);
        KSort(w->order, c->nonto, sizeof(Strip), StripByStart, NULL);
        rc = CommonholdRoomFor(vol, InkConjugateRoom(vol, c, w));
    }
    if (rc == OK)
        w->claim = DocketClaimOfShaped(c->nonto, 1, 0, TailSlipClaim(c->cur, w->rec, false));
    return rc;
}

/* Under the lock: both files where the act compared them — the target's
 * record held and let go at once, the map holding it — and the rooms. */
static error_t InkConjugateHolds(TagFSVolume* vol, const InkConjugation* c, const InkConjugateWork* w, FileRecord** still)
{
    TagFSState* fs = &vol->state;
    RecordWhere now, there;
    FileRecord* target = FileMapRecordHold(fs->files, c->onto_fid, &there);
    FileRecordRelease(target);
    if (!target)
        return ERR_FILE_NOT_FOUND;
    if (there.gen != c->onto_gen)
        return ERR_BUSY;
    if (!(*still = FileMapRecordHold(fs->files, c->fid, &now)))
        return ERR_FILE_NOT_FOUND;
    if (now.gen != c->was.gen)
        return ERR_BUSY;
    if (!CommonholdRoomHolds(vol, InkConjugateRoom(vol, c, w)) || !FallowNodeCommonRoom(vol, w->fallow))
        return INK_AGAIN;
    return RibbonClaim(vol, w->claim, false);
}

static error_t InkConjugateLocked(TagFSVolume* vol, const InkConjugation* c, InkConjugateWork* w, Jot* j, Jot** spent,
                                  InkReceipt* out)
{
    TagFSState* fs    = &vol->state;
    FileRecord* still = NULL;
    error_t     rc    = InkVolumeTakes(vol);
    if (rc == OK)
        rc = InkConjugateHolds(vol, c, w, &still);
    if (rc != OK) {
        FileRecordRelease(still);   /* the map holds it: nothing freed here */
        return rc;
    }
    uint64_t     gen   = InkGenNextLocked(vol);
    RecordWhere  where = { .gen = gen, .shape = gen }, gone;
    FileMapStake none  = { .count = 0 };
    (void)FileMapRecordPut(fs->files, c->fid, w->rec, &where, &none, &gone);
    w->rec = NULL;
    j->was = still;
    j->now = FileMapRecordHold(fs->files, c->fid, NULL);
    DocketPutLocked(vol, j, spent);
    if (FallowNodeCommonheld(vol, w->fallow, gen)) {
        FallowAdd(vol, w->fallow, gen);
        w->fallow = NULL;
    }
    for (uint32_t i = 0; i < c->nonto; i++)
        CommonholdShareLocked(vol, w->order[i].start, w->order[i].count);
    InkGenLaidLocked(vol, gen);
    out->gen = gen;
    return OK;
}

error_t InkConjugate(TagFSVolume* vol, const InkConjugation* c, InkReceipt* out)
{
    error_t rc = InkVolumeBegins(vol);
    if (rc != OK)
        return rc;
    do {
        InkConjugateWork w     = { 0 };
        Jot*             spent = NULL;
        Jot*             j     = NULL;
        rc = InkConjugateBuilt(vol, c, &w);
        if (rc == OK && !(j = JotMake(JOT_SHAPED, c->fid, NULL, NULL)))
            rc = ERR_NO_MEMORY;
        if (rc == OK) {
            TagFSLockTake(&vol->state.lock);
            rc = InkConjugateLocked(vol, c, &w, j, &spent, out);
            TagFSLockDrop(&vol->state.lock);
            rc = RibbonClaimSay(vol, rc);
        }
        if (rc == OK)
            SettleInked(vol, out->gen, false);
        else
            JotsLetGo(j);
        JotsLetGo(spent);
        InkConjugateWorkLetGo(&w);
    } while (rc == INK_AGAIN);
    return rc;
}
