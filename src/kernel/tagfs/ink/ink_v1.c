#include "ink_internal.h"

/*
 * Format v1's second half of every Ink (ink_internal.h): the record packed for
 * the medium, its room planned in the pool's tail against the reserve
 * (meta_pool.h) and laid there, the mirror put through the pool's door
 * (PoolMirrorPut) and the file's row following it — under the volume's state
 * lock, with nothing left that can refuse once a change begins; and the Spare
 * an Ink makes for itself when the tail has none.
 */

// ----------------------------------------------------------------------------
// The record, built before the lock
// ----------------------------------------------------------------------------


void InkRecordLetGo(InkRecord* r)
{
    FileRecordRelease(r->rec);
    kfree(r->packed);
    r->rec    = NULL;
    r->packed = NULL;
}

/* `meta` packed into the medium's copy of the record — memory's is the
 * caller's to build beside it (FileRecordBuild, Relabel, Splice): OK,
 * ERR_TAGFS_RECORD_FULL for a record wider than format v1 holds, PackV1's
 * refusal of what one cannot carry, ERR_NO_MEMORY. */
error_t InkRecordMake(const TagFSMetadata* meta, InkRecord* r)
{
    memset(r, 0, sizeof(*r));
    if (!PlotFits(meta, &r->bytes))
        return ERR_TAGFS_RECORD_FULL;
    r->packed = kmalloc(r->bytes);
    if (!r->packed)
        return ERR_NO_MEMORY;
    error_t packed = PackV1(meta, TagFSLayout()->data_blocks, r->packed, &r->bytes);
    if (packed != OK)
        InkRecordLetGo(r);
    return packed;
}

// ----------------------------------------------------------------------------
// The tail, under the lock
// ----------------------------------------------------------------------------

/* What a change may take of the pool's room (meta_pool.h, the reserve). */
typedef enum {
    INK_FREES_NOTHING = 0,     /* a record: a turn past the whole reserve */
    INK_GIVES_UP_NOTHING,      /* a Quitclaim of a file with no ground: past Glean's share */
    INK_FREES_GROUND,          /* a change that lets ground go: past the close it needs */
} InkFrees;

typedef struct {
    bool     back;          /* the file's record in the tail is taken back first */
    uint32_t back_len;
    uint32_t keep;          /* the Spares a turn leaves the pool */
} InkPlan;

/*
 * Room for `bytes`, planned and nothing changed: ERR_WOULD_BLOCK when the tail
 * has too little and no Spare past the change's floor may be had, or room the
 * change may not take. While the volume has a free block, the floor is the
 * whole reserve — the caller makes a Spare of its own from it; with none, the
 * change's own (meta_pool.h). And below Glean's share no change takes the
 * tail's room unless what it leaves covers what comes after it: the Spare of
 * the close that lets what the tail holds go, at least, and for a change that
 * frees nothing the step Glean takes next (GleanStepNeedLocked), `was` being
 * the record it kills — on a volume with no free block, the room is Glean's to
 * make.
 */
static error_t InkPlanFor(uint64_t fid, const RecordWhere* was, uint32_t bytes, InkFrees frees,
                          InkPlan* plan)
{
    uint32_t floor  = frees == INK_FREES_NOTHING    ? (uint32_t)SPARE_RESERVE
                    : frees == INK_GIVES_UP_NOTHING ? (uint32_t)SPARE_FOR_GLEAN
                                                    : 1u;   /* the close its ground needs */
    plan->back      = was && PoolTailHolds(was, fid, &plan->back_len);
    plan->keep      = InkState()->ledger.free_blocks ? (uint32_t)SPARE_RESERVE : floor;
    uint32_t room   = PoolTailRoom() + (plan->back ? plan->back_len : 0);
    uint32_t spares = PoolSparesHeld(NULL, 0);
    if (room < bytes)
        return spares > plan->keep ? OK : ERR_WOULD_BLOCK;
    if (spares >= (uint32_t)SPARE_FOR_GLEAN)
        return OK;
    uint32_t need = frees == INK_FREES_GROUND ? 0u : GleanStepNeedLocked(room - bytes, was);
    return spares >= (need > 1u ? need : 1u) ? OK : ERR_WOULD_BLOCK;
}

/* The plan carried out and the record laid: where it sits. From here nothing
 * refuses. What the replaced record had freed is freed by this Ink now. */
static RecordWhere InkLayDown(const RecordWhere* was, const InkPlan* plan, const InkRecord* r,
                              uint64_t gen)
{
    if (plan->back) {
        PoolTakeBack(was, plan->back_len);
        FallowRetag(was->gen, gen);
    }
    if (PoolTailRoom() < r->bytes)
        (void)PoolTailTurn(plan->keep);
    return PoolAppend(r->packed, r->bytes, gen);
}

/* The file's row follows its record: where the table will name it the next
 * time a cycle writes its block. */
static void InkRowFollows(uint64_t fid, const RecordWhere* where)
{
    uint32_t block = 0;
    if (PoolLinkBlock(where->link, &block))
        (void)FileMapRowSet(InkState()->files, fid, block, where->offset);
}

/* The same laying and the same door as every Ink: a moved record is its
 * file's newest, and lands past every record there is. */
void InkGleanLocked(uint64_t fid, const RecordWhere* from, const uint8_t* packed)
{
    uint64_t    gen = InkGenNextLocked();
    RecordWhere to  = PoolAppend(packed, from->len, gen);
    (void)PoolMirrorMove(fid, from, &to);
    InkGenLaidLocked(gen);
}

/* The record the Plot commits — the file's fields as `cur` has them, the
 * Plot's size and runs, and the time it was written — in both copies: the
 * medium's packed from the Plot's runs, as format v1 always packed them, and
 * memory's `cur` with what the Plot did to its runs spliced in, every Parcel
 * the Plot did not touch shared with `cur` (PlotConveyance). The ground the
 * splices free is not asked of them: Fallow is fed the Plot's own, as format
 * v1 always fed it (TagFSInkPlot). A STRICT or VOLUME_TESTS kernel holds the
 * two copies to each other, and the ground the splices free to the Plot's. */
error_t InkPlotRecord(const Plot* p, const FileRecord* cur, uint64_t now, InkRecord* r)
{
    TagFSMetadata meta = {
        .file_id       = p->fid,
        .flags         = cur->flags,
        .size          = p->size,
        .created_time  = cur->created_time,
        .modified_time = now,
        .filename      = (char*)cur->name,
        .tag_ids       = (uint32_t*)cur->tags,
        .tag_count     = cur->tag_count,
        .extents       = p->runs,
        .extent_count  = p->run_count,
    };
    BookSplice* sp  = NULL;
    uint64_t    nsp = 0;
    error_t     rc  = InkRecordMake(&meta, r);
    if (rc == OK)
        rc = PlotConveyance(p, cur, &sp, &nsp);
    if (rc == OK)
        rc = FileRecordSplice(cur, sp, nsp, p->size, now, &r->rec, NULL, NULL);
#if CONFIG_STRICT || defined(CONFIG_VOLUME_TESTS)
    if (rc == OK)
        (void)PlotConveyanceAgrees(p, cur, sp, nsp, r->rec, r->packed, r->bytes);
#endif
    kfree(sp);
    if (rc != OK)
        InkRecordLetGo(r);
    return rc;
}

/* The second half, under the lock. */
error_t InkPlotLocked(Plot* p, const RecordWhere* was, InkRecord* r, FallowNode* fallow,
                             InkReceipt* out)
{
    error_t rc = InkVolumeTakes();
    if (rc != OK)
        return rc;
    RecordWhere now;
    FileRecord* still = FileMapRecordHold(InkState()->files, p->fid, &now);
    if (!still)
        return ERR_FILE_NOT_FOUND;
    FileRecordRelease(still);
    if (now.gen != was->gen)
        return INK_AGAIN;

    InkPlan plan;
    rc = InkPlanFor(p->fid, &now, r->bytes, fallow ? INK_FREES_GROUND : INK_FREES_NOTHING,
                    &plan);
    if (rc != OK)
        return rc;

    uint64_t    gen   = InkGenNextLocked();
    RecordWhere where = InkLayDown(&now, &plan, r, gen);
    where.shape = gen;
    FileMapStake none = { .count = 0 };
    (void)PoolMirrorPut(p->fid, r->rec, &where, 0, &none);
    r->rec = NULL;
    InkRowFollows(p->fid, &where);
    if (fallow)
        FallowAdd(fallow, gen);
    if (p->stake_count)
        BlockMapInkedLocked(p->fid, gen);    /* its Stakes reach the medium with this record */
    if (p->claimed)
        p->helm->claimed_runs = 0;    /* the record names them now */
    InkDataNeedRaiseLocked(p->data_need);
    InkGenLaidLocked(gen);
    out->gen = gen;
    return OK;
}

/* Under the state lock, so a name or a tag inked for the file either came
 * before — and the record as it is now is the one checked here — or comes
 * after, and finds the room claimed. */
error_t InkRoomClaim(Plot* p)
{
    uint64_t extents = PackV1Extents(p->runs, p->run_count);
    if (!p->helm || extents <= p->runs_before)
        return OK;

    TagFSState* fs = InkState();
    error_t     rc = OK;
    TagFSLockTake(&fs->lock);
    FileRecord* cur = FileMapRecordHold(fs->files, p->fid, NULL);
    if (!cur) {
        rc = ERR_FILE_NOT_FOUND;
    } else {
        TagFSMetadata record = {
            .filename     = (char*)cur->name,
            .tag_ids      = (uint32_t*)cur->tags,
            .tag_count    = cur->tag_count,
            .extents      = p->runs,
            .extent_count = p->run_count,
        };
        if (PlotFits(&record, NULL)) {
            /* A record that fits carries at most a u16 of extents. */
            p->helm->claimed_runs = (uint32_t)extents;
            p->claimed            = (uint32_t)extents;
        } else {
            rc = ERR_TAGFS_RECORD_FULL;
        }
        FileRecordRelease(cur);
    }
    TagFSLockDrop(&fs->lock);
    if (rc == ERR_TAGFS_RECORD_FULL)
        kprintf("[TagFS] file %llu: its name and tags as they are now leave its record no room "
                "for the %llu extent(s) this call would name — refused, and nothing of it "
                "written\n", p->fid, (unsigned long long)extents);
    return rc;
}

void InkRoomLetGo(Plot* p)
{
    if (!p->claimed)
        return;
    TagFSLockTake(&InkState()->lock);
    p->helm->claimed_runs = 0;
    TagFSLockDrop(&InkState()->lock);
    p->claimed = 0;
}

/* The record `meta` describes must also leave the room a write in flight
 * claimed (RT-19): the extents that write will ink, where they are more than
 * the record names now. */
static bool InkFieldsLeaveRoom(uint64_t fid, const TagFSMetadata* meta)
{
    uint32_t claimed = HelmClaimedRuns(fid);
    return claimed <= PackV1Extents(meta->extents, meta->extent_count) ||
           claimed <= PlotRunRoom(meta);
}

error_t InkFieldsLocked(uint64_t fid, const RecordWhere* was, const InkFieldsPlan* f,
                               const TagFSMetadata* meta, uint32_t add, InkRecord* r,
                               TagBitmapStake* stake, InkReceipt* out)
{
    TagFSState* fs = InkState();
    error_t     rc = InkVolumeTakes();
    if (rc != OK)
        return rc;
    RecordWhere now;
    FileRecord* still = FileMapRecordHold(fs->files, fid, &now);
    if (!still)
        return ERR_FILE_NOT_FOUND;
    FileRecordRelease(still);
    if (now.gen != was->gen)
        return INK_AGAIN;
    if (f->adds && !TagBitmapStakeCovers(fs->bitmap_index, &add, 1, fid, stake))
        return INK_AGAIN;
    if (!InkFieldsLeaveRoom(fid, meta))
        return ERR_TAGFS_RECORD_FULL;

    InkPlan plan;
    rc = InkPlanFor(fid, &now, r->bytes, INK_FREES_NOTHING, &plan);
    if (rc != OK)
        return rc;

    uint64_t    gen   = InkGenNextLocked();
    RecordWhere where = InkLayDown(&now, &plan, r, gen);
    where.shape = now.shape;
    FileMapStake none = { .count = 0 };
    (void)PoolMirrorPut(fid, r->rec, &where, 0, &none);
    r->rec = NULL;
    InkRowFollows(fid, &where);
    if (f->adds)
        TagBitmapAddCovered(fs->bitmap_index, &add, 1, fid);
    for (uint32_t i = 0; i < f->drops && i < 2; i++)
        TagBitmapLeave(fs->bitmap_index, f->dropped[i], fid);
    InkGenLaidLocked(gen);
    out->gen = gen;
    return OK;
}

error_t InkNewLocked(const uint32_t* tags, uint32_t count, InkRecord* r,
                            InkNewStakes* st, uint64_t* out_fid, InkReceipt* out)
{
    TagFSState* fs  = InkState();
    uint64_t    fid = fs->ledger.next_file_id;
    error_t     rc  = InkVolumeTakes();
    if (rc != OK)
        return rc;

    if (FileMapNodesToAdd(fs->files, fid) > st->map.count ||
        !TagBitmapStakeCovers(fs->bitmap_index, tags, count, fid, st->index))
        return INK_AGAIN;
    /* The id is issued as the last of the checks: a refused one finds nothing
     * changed, and an issued one is the id the stakes were taken for. */
    InkPlan plan;
    rc = InkPlanFor(fid, NULL, r->bytes, INK_FREES_NOTHING, &plan);
    if (rc == OK)
        rc = PackV1Refile(r->packed, fid);    /* the id the issue gives, as the record carries it */
    if (rc == OK)
        rc = LedgerIssueFileId(&fs->ledger, &fid);
    if (rc != OK)
        return rc;

    r->rec->file_id = fid;

    uint64_t    gen   = InkGenNextLocked();
    RecordWhere where = InkLayDown(NULL, &plan, r, gen);
    uint32_t    block = 0;
    where.shape = gen;
    (void)PoolLinkBlock(where.link, &block);
    (void)PoolMirrorPut(fid, r->rec, &where, block, &st->map);   /* the row with the record */
    r->rec = NULL;
    (void)file_table_update(fid, block, where.offset);
    TagBitmapAddCovered(fs->bitmap_index, tags, count, fid);
    fs->ledger.total_files++;
    InkGenLaidLocked(gen);
    *out_fid = fid;
    out->gen = gen;
    return OK;
}

error_t InkQuitclaimLocked(uint64_t fid, const RecordWhere* was, const FileRecord* cur,
                                  InkRecord* r, FallowNode* fallow, InkReceipt* out)
{
    TagFSState* fs = InkState();
    error_t     rc = InkVolumeTakes();
    if (rc != OK)
        return rc;
    RecordWhere now;
    FileRecord* still = FileMapRecordHold(fs->files, fid, &now);
    if (!still)
        return ERR_FILE_NOT_FOUND;
    FileRecordRelease(still);
    if (now.gen != was->gen)
        return INK_AGAIN;

    InkPlan plan;
    rc = InkPlanFor(fid, &now, r->bytes, fallow ? INK_FREES_GROUND : INK_GIVES_UP_NOTHING,
                    &plan);
    if (rc != OK)
        return rc;

    uint64_t    gen   = InkGenNextLocked();
    RecordWhere where = InkLayDown(&now, &plan, r, gen);
    FileMapStake none = { .count = 0 };
    (void)PoolMirrorPut(fid, NULL, &where, 0, &none);
    for (uint32_t i = 0; i < cur->tag_count; i++)
        TagBitmapLeave(fs->bitmap_index, cur->tags[i], fid);
    if (fallow)
        FallowAdd(fallow, gen);
    AllotmentRelease(fid);
    if (fs->ledger.total_files > 0)
        fs->ledger.total_files--;
    InkGenLaidLocked(gen);
    out->gen = gen;
    return OK;
}
