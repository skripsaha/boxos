#include "ink_internal.h"
#include "../mount/book_mount.h"
#include "../settle/sewing.h"

/*
 * Format v2's second half of an Ink (ink_internal.h): the writer's half of
 * BookPut (spec_v2 §3.7, §16.2 п.15). Before the lock, the record the file
 * becomes is built, a Jot made for it and the stakes taken; under the lock the
 * file is held to what the first half read, the record put in the mirror, the
 * Jot merged into the Docket for the next stitch (docket.h), the tag index and
 * the counters moved — nothing asked of the heap, nothing laid anywhere: the
 * stitch that takes the Docket lays the records (settle/sewing.c). What the
 * Docket spends, and the records it lets go, are let go after the lock.
 *
 * ‼ THE RIBBON'S ROOM IS CLAIMED UNDER THE SAME LOCK, BEFORE ANYTHING CHANGES
 * (P21, §3.6). The bytes the change's record takes at the most are counted
 * before the lock (DocketClaimOf) and claimed once the file is held to what
 * the first half read (RibbonClaim): a claim refused changes nothing —
 * ERR_WOULD_BLOCK while room can come, which the caller waits for the cycle's
 * end on (settle_ops.c, SettleOpForRoom), ERR_TAGFS_NO_SPACE when none can,
 * said once. The Coineds of the tags it brings were held before they were
 * given ids (ink.c, InkBookRefuses) and are the registry's to count since.
 *
 * ‼ A FILE ID IS GIVEN ONCE, AND NEVER PAST WHAT A PROGRAM HOLDS. The next one
 * is the mount's bound past every fid the book names (book_mount.c), counted
 * on in 64 bits here — the ids a format-v1 Ledger counts in 32 are not a book's
 * — and given no further than INT64_MAX: a program reads a file id back as a
 * signed 64-bit number (the storage deck's answers), so one past it is no id
 * any program could name.
 */

/* The last file id a book's file is given. */
#define INK_BOOK_FID_MOST ((uint64_t)INT64_MAX)

// ----------------------------------------------------------------------------
// A file's name and tags
// ----------------------------------------------------------------------------

static error_t InkBookFieldsLocked(uint64_t fid, const RecordWhere* was, const InkFieldsPlan* f, uint32_t add,
                                   FileRecord** rec, Jot* j, TagBitmapStake* stake, uint64_t claim, Jot** spent,
                                   InkReceipt* out)
{
    TagFSState* fs = InkState();
    error_t     rc = InkVolumeTakes();
    if (rc != OK)
        return rc;
    RecordWhere now;
    FileRecord* still = FileMapRecordHold(fs->files, fid, &now);
    if (!still)
        return ERR_FILE_NOT_FOUND;
    if (now.gen != was->gen || (f->adds && !TagBitmapStakeCovers(fs->bitmap_index, &add, 1, fid, stake)))
        rc = INK_AGAIN;
    else
        rc = RibbonClaim(claim, false);
    if (rc != OK) {
        FileRecordRelease(still);    /* the map holds it: nothing freed here */
        return rc;
    }
    uint64_t     gen   = InkGenNextLocked();
    RecordWhere  where = { .gen = gen, .shape = now.shape }, gone;
    FileMapStake none  = { .count = 0 };
    (void)FileMapRecordPut(fs->files, fid, *rec, &where, 0, &none, &gone);
    *rec   = NULL;
    j->was = still;
    j->now = FileMapRecordHold(fs->files, fid, NULL);
    DocketPutLocked(j, spent);
    if (f->adds)
        TagBitmapAddCovered(fs->bitmap_index, &add, 1, fid);
    for (uint32_t i = 0; i < f->drops && i < 2; i++)
        TagBitmapLeave(fs->bitmap_index, f->dropped[i], fid);
    InkGenLaidLocked(gen);
    out->gen = gen;
    return OK;
}

error_t InkBookFields(uint64_t fid, FileRecord* cur, const RecordWhere* was, const InkFieldsPlan* f,
                      const char* name, uint32_t add, InkReceipt* out)
{
    TagFSState*     fs    = InkState();
    TagBitmapStake* stake = NULL;
    Jot*            spent = NULL;
    FileRecord*     rec   = FileRecordRelabel(cur, name, f->tags, f->count, cur->flags);
    Jot*            j     = rec ? JotMake(JOT_LABELED, fid, NULL, NULL) : NULL;
    error_t         rc    = j ? OK : ERR_NO_MEMORY;
    if (rc == OK && f->adds)
        rc = TagBitmapStakeFor(fs->bitmap_index, &add, 1, fid, &stake);
    if (rc == OK) {
        uint64_t claim = DocketClaimOf(JOT_LABELED, cur, rec);
        TagFSLockTake(&fs->lock);
        rc = InkBookFieldsLocked(fid, was, f, add, &rec, j, stake, claim, &spent, out);
        TagFSLockDrop(&fs->lock);
        RibbonClaimSay(rc);
    }
    if (rc == OK) {
        SettleInked(out->gen, false);
        TagBitmapShrink(fs->bitmap_index, &fs->lock, f->dropped, f->drops < 2 ? f->drops : 2, fid);
    } else {
        JotsLetGo(j);
    }
    JotsLetGo(spent);
    FileRecordRelease(rec);
    TagBitmapStakeLetGo(stake);
    return rc;
}

// ----------------------------------------------------------------------------
// A new file
// ----------------------------------------------------------------------------

static error_t InkBookNewLocked(const uint32_t* tags, uint32_t count, FileRecord** rec, Jot* j, InkNewStakes* st,
                                uint64_t claim, Jot** spent, uint64_t* out_fid, InkReceipt* out)
{
    TagFSState* fs  = InkState();
    uint64_t    fid = g_book.next_fid;
    error_t     rc  = InkVolumeTakes();
    if (rc != OK)
        return rc;
    if (fid > INK_BOOK_FID_MOST)
        return ERR_TAGFS_FID_EXHAUSTED;
    if (FileMapNodesToAdd(fs->files, fid) > st->map.count ||
        !TagBitmapStakeCovers(fs->bitmap_index, tags, count, fid, st->index))
        return INK_AGAIN;
    if ((rc = RibbonClaim(claim, false)) != OK)
        return rc;
    (*rec)->file_id = fid;
    uint64_t    gen   = InkGenNextLocked();
    RecordWhere where = { .gen = gen, .shape = gen }, gone;
    (void)FileMapRecordPut(fs->files, fid, *rec, &where, 0, &st->map, &gone);
    *rec   = NULL;
    j->fid = fid;
    j->now = FileMapRecordHold(fs->files, fid, NULL);
    DocketPutLocked(j, spent);
    TagBitmapAddCovered(fs->bitmap_index, tags, count, fid);
    g_book.next_fid         = fid + 1u;
    fs->ledger.next_file_id = fid + 1u > UINT32_MAX ? UINT32_MAX : (uint32_t)(fid + 1u);
    fs->ledger.total_files++;
    InkGenLaidLocked(gen);
    *out_fid = fid;
    out->gen = gen;
    return OK;
}

/* The file is born empty, its times a book's own — nanoseconds (§2.8), as the
 * mount reads them back — and its id the lock's to give: the stakes are taken
 * for the id the lock showed, and taken again when another create took it. */
error_t InkBookNew(const char* name, const uint32_t* tags, uint32_t count, uint64_t* out_fid, InkReceipt* out)
{
    TagFSState*   fs   = InkState();
    uint64_t      now  = rtc_get_unix64() * 1000000000ull;
    TagFSMetadata meta = { .flags = TAGFS_FILE_ACTIVE, .created_time = now, .modified_time = now,
                           .filename = (char*)name, .tag_ids = (uint32_t*)tags, .tag_count = count };
    FileRecord*   rec  = FileRecordBuild(&meta);
    error_t       rc   = rec ? INK_AGAIN : ERR_NO_MEMORY;
    uint64_t      claim = rec ? DocketClaimOf(JOT_BORN, NULL, rec) : 0;
    while (rc == INK_AGAIN) {
        InkNewStakes st    = { .map = { .count = 0 }, .index = NULL };
        Jot*         spent = NULL;
        TagFSLockTake(&fs->lock);
        uint64_t fid = g_book.next_fid;
        TagFSLockDrop(&fs->lock);
        Jot* j = JotMake(JOT_BORN, fid, NULL, NULL);
        rc     = j ? FileMapStakeFor(fs->files, fid, &st.map) : ERR_NO_MEMORY;
        if (rc == OK)
            rc = TagBitmapStakeFor(fs->bitmap_index, tags, count, fid, &st.index);
        if (rc == OK) {
            TagFSLockTake(&fs->lock);
            rc = InkBookNewLocked(tags, count, &rec, j, &st, claim, &spent, out_fid, out);
            TagFSLockDrop(&fs->lock);
            RibbonClaimSay(rc);
        }
        if (rc == OK)
            SettleInked(out->gen, false);
        else
            JotsLetGo(j);
        JotsLetGo(spent);
        FileMapStakeLetGo(&st.map);
        TagBitmapStakeLetGo(st.index);
    }
    FileRecordRelease(rec);
    if (rc == ERR_TAGFS_FID_EXHAUSTED)
        kprintf("[TagFS] '%s' is not made: every file id a program can name has been given on this volume, "
                "the last of them %llu\n", name, (unsigned long long)INK_BOOK_FID_MOST);
    return rc;
}

// ----------------------------------------------------------------------------
// A file given up
// ----------------------------------------------------------------------------

static error_t InkBookQuitclaimLocked(uint64_t fid, const RecordWhere* was, const FileRecord* cur,
                                      FallowNode* fallow, Jot* j, Jot** spent, InkReceipt* out)
{
    TagFSState* fs = InkState();
    error_t     rc = InkVolumeTakes();
    if (rc != OK)
        return rc;
    RecordWhere now, gone;
    FileRecord* still = FileMapRecordHold(fs->files, fid, &now);
    if (!still)
        return ERR_FILE_NOT_FOUND;
    rc = now.gen != was->gen ? INK_AGAIN : RibbonClaim(DocketClaimOf(JOT_QUITCLAIM, cur, NULL), true);
    if (rc != OK) {
        FileRecordRelease(still);
        return rc;
    }
    uint64_t gen = InkGenNextLocked();
    FileMapDrop(fs->files, fid, &gone);
    j->was = still;
    DocketPutLocked(j, spent);
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

error_t InkBookQuitclaim(uint64_t fid, const RecordWhere* was, const FileRecord* cur, FallowNode* fallow,
                         InkReceipt* out)
{
    TagFSState* fs    = InkState();
    Jot*        spent = NULL;
    Jot*        j     = JotMake(JOT_QUITCLAIM, fid, NULL, NULL);
    error_t     rc    = j ? OK : ERR_NO_MEMORY;
    if (rc == OK) {
        TagFSLockTake(&fs->lock);
        rc = InkBookQuitclaimLocked(fid, was, cur, fallow, j, &spent, out);
        TagFSLockDrop(&fs->lock);
        RibbonClaimSay(rc);
    }
    if (rc == OK) {
        SettleInked(out->gen, false);
        TagBitmapShrink(fs->bitmap_index, &fs->lock, cur->tags, cur->tag_count, fid);
    } else {
        JotsLetGo(j);
        FallowNodeLetGo(fallow);
    }
    JotsLetGo(spent);
    return rc;
}
