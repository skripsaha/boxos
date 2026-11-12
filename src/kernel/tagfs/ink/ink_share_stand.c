#include "ink_internal.h"
#include "commonhold.h"
#include "../settle/sewing.h"
#include "../settle/settle.h"
#include "../file_map/tail_slip.h"

#ifdef CONFIG_VOLUME_TESTS

/*
 * The stand's lever for a run SHARED (ink.h, InkBookShareStand): a duplicate
 * laid on the block it duplicates as Dedup lays one (spec_v2 §8.2) — the
 * file's own blocks let go, a run SHARED put in their place — by the Ink a
 * writer of it takes: before the lock the splice, the record it builds, the
 * ground it lets go in a Fallow node and the room Commonhold's change takes;
 * under the lock the file held to what was read, the record put in the mirror
 * and its Jot in the Docket, one owner more on every shared block, the file's
 * own let go through Commonhold (fallow.h, FallowNodeCommonheld).
 */

typedef struct {
    uint64_t    fid;
    uint64_t    start;
    uint32_t    count;
    RecordWhere was;
    FileRecord* rec;
    FallowNode* fallow;
    uint64_t    claim;
    uint64_t    splits;     /* the runs Commonhold's change may cut it into, the share's and the node's */
    uint64_t    gen;
} InkShare;

static error_t InkShareLocked(InkShare* s, Jot* j, Jot** spent)
{
    TagFSState* fs = InkState();
    error_t     rc = InkVolumeTakes();
    RecordWhere now, gone;
    if (rc != OK)
        return rc;
    FileRecord* still = FileMapRecordHold(fs->files, s->fid, &now);
    if (!still)
        return ERR_FILE_NOT_FOUND;
    if (now.gen != s->was.gen || !CommonholdRoomHolds(s->splits) ||
        (s->fallow && !FallowNodeCommonRoom(s->fallow)))
        rc = INK_AGAIN;
    else
        rc = RibbonClaim(s->claim, false);
    if (rc != OK) {
        FileRecordRelease(still);   /* the map holds it: nothing freed here */
        return rc;
    }
    uint64_t     gen   = InkGenNextLocked();
    RecordWhere  where = { .gen = gen, .shape = gen };
    FileMapStake none  = { .count = 0 };
    (void)FileMapRecordPut(fs->files, s->fid, s->rec, &where, 0, &none, &gone);
    s->rec = NULL;
    j->was = still;
    j->now = FileMapRecordHold(fs->files, s->fid, NULL);
    DocketPutLocked(j, spent);
    if (s->fallow && FallowNodeCommonheld(s->fallow, gen)) {
        FallowAdd(s->fallow, gen);
        s->fallow = NULL;
    }
    CommonholdShareLocked(s->start, s->count);
    InkGenLaidLocked(gen);
    s->gen = gen;
    return OK;
}

/* The share's work before the lock: the record, the node, the rooms. */
static error_t InkShareBuilt(InkShare* s, const FileRecord* cur, uint64_t at)
{
    Strip      put    = { .start = s->start, .count = s->count, .kind = STRIP_SHARED, .birth = 0 };
    BookSplice sp     = { .at = at, .remove = s->count, .ninsert = 1, .insert = &put };
    Strip*     freed  = NULL;
    uint64_t   nfreed = 0;
    error_t    rc     = FileRecordSplice(cur, &sp, 1, cur->size, cur->modified_time, &s->rec, &freed, &nfreed);
    if (rc == OK && nfreed > UINT32_MAX)
        rc = ERR_NO_MEMORY;
    if (rc == OK && nfreed && !(s->fallow = FallowNodeMake(freed, (uint32_t)nfreed, FALLOW_BY_RECORD, s->fid)))
        rc = ERR_NO_MEMORY;
    s->splits = (uint64_t)s->count + 2u + 2u * nfreed;
    if (rc == OK)
        rc = CommonholdRoomFor(s->splits);
    s->claim = DocketClaimOfShaped(1, 1, 0, rc == OK ? TailSlipClaim(cur, s->rec, false) : 0);
    kfree(freed);
    return rc;
}

error_t InkBookShareStand(uint64_t fid, uint64_t at, uint64_t start, uint32_t count)
{
    error_t rc = INK_AGAIN;
    while (rc == INK_AGAIN) {
        InkShare    s     = { .fid = fid, .start = start, .count = count };
        Jot*        spent = NULL;
        Jot*        j     = NULL;
        FileRecord* cur   = FileMapRecordHold(InkState()->files, fid, &s.was);
        rc = cur ? InkShareBuilt(&s, cur, at) : ERR_FILE_NOT_FOUND;
        if (rc == OK && !(j = JotMake(JOT_SHAPED, fid, NULL, NULL)))
            rc = ERR_NO_MEMORY;
        if (rc == OK) {
            TagFSLockTake(&InkState()->lock);
            rc = InkShareLocked(&s, j, &spent);
            TagFSLockDrop(&InkState()->lock);
            rc = RibbonClaimSay(rc);
        }
        if (rc == OK)
            SettleInked(s.gen, false);
        else
            JotsLetGo(j);
        JotsLetGo(spent);
        FallowNodeLetGo(s.fallow);
        FileRecordRelease(s.rec);
        FileRecordRelease(cur);
    }
    return rc;
}

#endif /* CONFIG_VOLUME_TESTS */
