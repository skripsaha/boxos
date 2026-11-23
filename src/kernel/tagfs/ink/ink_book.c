#include "ink_internal.h"
#include "../mount/book_mount.h"
#include "../settle/sewing.h"
#include "../integrity/gloss.h"
#include "../file_map/tail_slip.h"
#include "../helm/engross.h"
#include "../mount/perambulation.h"
#include "../retire/retire.h"
#include "tagfs_volume.h"

/*
 * The second half of an Ink (ink_internal.h): the writer's half of BookPut.
 * Before the lock, the record the file becomes is built, a Jot made for it and
 * the stakes taken; under the lock the file is held to what the first half
 * read, the record put in the mirror, the Jot merged into the Docket for the
 * next stitch (docket.h), the tag index and the counters moved — nothing asked
 * of the heap, nothing laid anywhere: the stitch that takes the Docket lays
 * the records (settle/sewing.c). What the Docket spends, and the records it
 * lets go, are let go after the lock. A change to a file's data comes as a
 * Plot's window spliced into the record it was loaded from, the digests of the
 * blocks it wrote put in the Gloss under the same lock as the record (§8.1),
 * and the ground it let go resting in Fallow (BY_RECORD) — the writer's half
 * of BookPut.
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
 * on in 64 bits here, and given no further than INT64_MAX: a program reads a file id back as a
 * signed 64-bit number (the storage deck's answers), so one past it is no id
 * any program could name.
 */

/* The last file id a book's file is given. */
#define INK_BOOK_FID_MOST ((uint64_t)INT64_MAX)

// ----------------------------------------------------------------------------
// A file's name and tags
// ----------------------------------------------------------------------------

/* A tag no file may wear any more — retired — is put on no record: the
 * registry closes a tag before the files are cleared of it, and a retirement
 * is laid only under this lock with none wearing it (InkBookRetireLocked), so
 * no record laid under it wears a tag once retired. */
static error_t InkBookWearable(TagFSVolume* vol, const uint32_t* tags, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        if (TagRegistryRetiredId(vol->state.registry, tags[i]))
            return ERR_TAGFS_INVALID_TAG;
    return OK;
}

static error_t InkBookFieldsLocked(TagFSVolume* vol, uint64_t fid, const RecordWhere* was, const InkFieldsPlan* f, uint32_t add,
                                   FileRecord** rec, Jot* j, TagBitmapStake* stake, uint64_t claim, Jot** spent,
                                   InkReceipt* out)
{
    TagFSState* fs = &vol->state;
    error_t     rc = InkVolumeTakes(vol);
    if (rc != OK)
        return rc;
    RecordWhere now;
    FileRecord* still = FileMapRecordHold(fs->files, fid, &now);
    if (!still)
        return ERR_FILE_NOT_FOUND;
    if (f->adds && (rc = InkBookWearable(vol, &add, 1)) != OK) {
        FileRecordRelease(still);
        return rc;
    }
    if (now.gen != was->gen || (f->adds && !TagBitmapStakeCovers(fs->bitmap_index, &add, 1, fid, stake)))
        rc = INK_AGAIN;
    else
        rc = RibbonClaim(vol, claim, false);
    if (rc != OK) {
        FileRecordRelease(still);    /* the map holds it: nothing freed here */
        return rc;
    }
    uint64_t     gen   = InkGenNextLocked(vol);
    RecordWhere  where = { .gen = gen, .shape = now.shape }, gone;
    FileMapStake none  = { .count = 0 };
    (void)FileMapRecordPut(fs->files, fid, *rec, &where, &none, &gone);
    *rec   = NULL;
    j->was = still;
    j->now = FileMapRecordHold(fs->files, fid, NULL);
    DocketPutLocked(vol, j, spent);
    if (f->adds)
        TagBitmapAddCovered(fs->bitmap_index, &add, 1, fid);
    for (uint32_t i = 0; i < f->drops; i++)
        TagBitmapLeave(fs->bitmap_index, f->dropped[i], fid);
    InkGenLaidLocked(vol, gen);
    out->gen = gen;
    return OK;
}

error_t InkBookFields(TagFSVolume* vol, uint64_t fid, FileRecord* cur, const RecordWhere* was, const InkFieldsPlan* f,
                      const char* name, uint32_t add, uint32_t flags, uint64_t keep_until,
                      InkReceipt* out)
{
    TagFSState*     fs    = &vol->state;
    TagBitmapStake* stake = NULL;
    Jot*            spent = NULL;
    FileRecord*     rec   = FileRecordRelabel(cur, name, f->tags, f->count, f->shown, f->shown_count, flags,
                                                keep_until);
    if (rec && rec->passport != f->passport)
        FileRecordPassportSet(rec, f->passport);
    Jot*            j     = rec ? JotMake(JOT_LABELED, fid, NULL, NULL) : NULL;
    error_t         rc    = j ? OK : ERR_NO_MEMORY;
    if (rc == OK && f->adds)
        rc = TagBitmapStakeFor(fs->bitmap_index, &add, 1, fid, &stake);
    if (rc == OK) {
        uint64_t claim = DocketClaimOf(JOT_LABELED, cur, rec);
        TagFSLockTake(&fs->lock);
        rc = InkBookFieldsLocked(vol, fid, was, f, add, &rec, j, stake, claim, &spent, out);
        TagFSLockDrop(&fs->lock);
        rc = RibbonClaimSay(vol, rc);
    }
    if (rc == OK) {
        SettleInked(vol, out->gen, false);
        TagBitmapShrink(fs->bitmap_index, &fs->lock, f->dropped, f->drops, fid);
    } else {
        JotsLetGo(j);
    }
    JotsLetGo(spent);
    FileRecordRelease(rec);
    TagBitmapStakeLetGo(stake);
    return rc;
}

/* The tags retired at a gen of their own, which is no file's: the stitch that
 * takes it up to its target lays each one's Coined RETIRED. Only once no file
 * wears one — asked under the lock every record that puts a tag on is laid
 * under — so none ever wears a tag the book holds retired. */
static error_t InkBookRetireLocked(TagFSVolume* vol, const uint32_t* tags, uint32_t count, InkReceipt* out)
{
    TagFSState* fs = &vol->state;
    error_t     rc = InkVolumeTakes(vol);
    if (rc != OK)
        return rc;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t  worn;
        TagFSPage page;
        if (TagBitmapQueryPage(fs->bitmap_index, &tags[i], 1, NULL, 0, NULL, 0, 0, &worn, 1, false, &page) != 0)
            return ERR_TAGFS_NOT_INITIALIZED;
        if (page.delivered)
            return ERR_BUSY;
    }
    uint64_t gen = InkGenNextLocked(vol);
    for (uint32_t i = 0; i < count && rc == OK; i++)
        rc = TagRegistryRetireAt(fs->registry, tags[i], gen);
    InkGenLaidLocked(vol, gen);
    out->gen = gen;
    return rc;
}

error_t InkBookRetire(TagFSVolume* vol, const uint32_t* tags, uint32_t count, InkReceipt* out)
{
    TagFSState* fs = &vol->state;
    TagFSLockTake(&fs->lock);
    error_t rc = InkBookRetireLocked(vol, tags, count, out);
    TagFSLockDrop(&fs->lock);
    if (rc == OK)
        SettleInked(vol, out->gen, false);
    return rc;
}

// ----------------------------------------------------------------------------
// A new file
// ----------------------------------------------------------------------------

static error_t InkBookNewLocked(TagFSVolume* vol, const uint32_t* tags, uint32_t count, FileRecord** rec, Jot* j, InkNewStakes* st,
                                uint64_t claim, Jot** spent, uint64_t* out_fid, InkReceipt* out)
{
    TagFSState* fs  = &vol->state;
    uint64_t    fid = vol->book.next_fid;
    error_t     rc  = InkVolumeTakes(vol);
    if (rc != OK)
        return rc;
    if (fid > INK_BOOK_FID_MOST)
        return ERR_TAGFS_FID_EXHAUSTED;
    if ((rc = InkBookWearable(vol, tags, count)) != OK)
        return rc;
    if (FileMapNodesToAdd(fs->files, fid) > st->map.count ||
        !TagBitmapStakeCovers(fs->bitmap_index, tags, count, fid, st->index))
        return INK_AGAIN;
    if ((rc = RibbonClaim(vol, claim, false)) != OK)
        return rc;
    (*rec)->file_id = fid;
    uint64_t    gen   = InkGenNextLocked(vol);
    RecordWhere where = { .gen = gen, .shape = gen }, gone;
    (void)FileMapRecordPut(fs->files, fid, *rec, &where, &st->map, &gone);
    *rec   = NULL;
    j->fid = fid;
    j->now = FileMapRecordHold(fs->files, fid, NULL);
    DocketPutLocked(vol, j, spent);
    TagBitmapAddCovered(fs->bitmap_index, tags, count, fid);
    vol->book.next_fid = fid + 1u;
    fs->total_files++;
    InkGenLaidLocked(vol, gen);
    *out_fid = fid;
    out->gen = gen;
    return OK;
}

/* The file is born empty, its times a book's own — nanoseconds (§2.8), as the
 * mount reads them back — and its id the lock's to give: the stakes are taken
 * for the id the lock showed, and taken again when another create took it.
 * `made_by` is the file of the program that makes it, 0 for none: it is in the
 * record from its birth, and in the Born that carries it. */
error_t InkBookNew(TagFSVolume* vol, const char* name, const uint32_t* tags, uint32_t count, uint64_t made_by, uint64_t* out_fid,
                   InkReceipt* out)
{
    TagFSState*   fs   = &vol->state;
    uint64_t      now  = rtc_get_unix64() * 1000000000ull;
    TagFSMetadata meta = { .flags = TAGFS_FILE_ACTIVE, .created_time = now, .modified_time = now,
                           .filename = (char*)name, .tag_ids = (uint32_t*)tags, .tag_count = count,
                           .made_by = made_by };
    FileRecord*   rec  = FileRecordBuild(&meta);
    error_t       rc   = rec ? INK_AGAIN : ERR_NO_MEMORY;
    uint64_t      claim = rec ? DocketClaimOf(JOT_BORN, NULL, rec) : 0;
    while (rc == INK_AGAIN) {
        InkNewStakes st    = { .map = { .count = 0 }, .index = NULL };
        Jot*         spent = NULL;
        TagFSLockTake(&fs->lock);
        uint64_t fid = vol->book.next_fid;
        TagFSLockDrop(&fs->lock);
        Jot* j = JotMake(JOT_BORN, fid, NULL, NULL);
        rc     = j ? FileMapStakeFor(fs->files, fid, &st.map) : ERR_NO_MEMORY;
        if (rc == OK)
            rc = TagBitmapStakeFor(fs->bitmap_index, tags, count, fid, &st.index);
        if (rc == OK) {
            TagFSLockTake(&fs->lock);
            rc = InkBookNewLocked(vol, tags, count, &rec, j, &st, claim, &spent, out_fid, out);
            TagFSLockDrop(&fs->lock);
            rc = RibbonClaimSay(vol, rc);
        }
        if (rc == OK)
            SettleInked(vol, out->gen, false);
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

static error_t InkBookQuitclaimLocked(TagFSVolume* vol, uint64_t fid, const RecordWhere* was, const FileRecord* cur,
                                      FallowNode** fallow, Jot* j, Jot** spent, InkReceipt* out)
{
    TagFSState* fs = &vol->state;
    error_t     rc = InkVolumeTakes(vol);
    if (rc != OK)
        return rc;
    RecordWhere now, gone;
    FileRecord* still = FileMapRecordHold(fs->files, fid, &now);
    if (!still)
        return ERR_FILE_NOT_FOUND;
    if (now.gen != was->gen || (*fallow && !FallowNodeCommonRoom(vol, *fallow)))
        rc = INK_AGAIN;
    else
        rc = RibbonClaim(vol, DocketClaimOf(JOT_QUITCLAIM, cur, NULL), true);
    if (rc != OK) {
        FileRecordRelease(still);
        return rc;
    }
    uint64_t gen = InkGenNextLocked(vol);
    FileMapDrop(fs->files, fid, &gone);
    j->was = still;
    DocketPutLocked(vol, j, spent);
    for (uint32_t i = 0; i < cur->tag_count; i++)
        TagBitmapLeave(fs->bitmap_index, cur->tags[i], fid);
    if (cur->flags & TAGFS_FILE_DELETED)
        TagBitmapLeave(fs->bitmap_index, TAGFS_DELETED_MEMBERS, fid);
    if (*fallow && FallowNodeCommonheld(vol, *fallow, gen)) {
        FallowAdd(vol, *fallow, gen);
        *fallow = NULL;
    }
    AllotmentRelease(vol, fid);
    if (fs->total_files > 0)
        fs->total_files--;
    InkGenLaidLocked(vol, gen);
    out->gen = gen;
    return OK;
}

error_t InkBookQuitclaim(TagFSVolume* vol, uint64_t fid, const RecordWhere* was, const FileRecord* cur, FallowNode* fallow,
                         InkReceipt* out)
{
    TagFSState* fs    = &vol->state;
    Jot*        spent = NULL;
    Jot*        j     = JotMake(JOT_QUITCLAIM, fid, NULL, NULL);
    error_t     rc    = j ? OK : ERR_NO_MEMORY;
    if (rc == OK) {
        TagFSLockTake(&fs->lock);
        rc = InkBookQuitclaimLocked(vol, fid, was, cur, &fallow, j, &spent, out);
        TagFSLockDrop(&fs->lock);
        rc = RibbonClaimSay(vol, rc);
    }
    if (rc == OK) {
        SettleInked(vol, out->gen, false);
        RetireOwed(vol);
        TagBitmapShrink(fs->bitmap_index, &fs->lock, cur->tags, cur->tag_count, fid);
        if (cur->flags & TAGFS_FILE_DELETED) {
            const uint32_t members = TAGFS_DELETED_MEMBERS;
            TagBitmapShrink(fs->bitmap_index, &fs->lock, &members, 1, fid);
        }
    } else {
        JotsLetGo(j);
    }
    FallowNodeLetGo(fallow);    /* added, or left with no ground of its own to rest, or refused */
    JotsLetGo(spent);
    return rc;
}

// ----------------------------------------------------------------------------
// A file's data
// ----------------------------------------------------------------------------

/* The blocks the Plot wrote, as the runs they lie in, for the Gloss's room: a
 * new array of *count — NULL with 0 for none — or ERR_NO_MEMORY. */
static error_t InkBookWritten(const Plot* p, Strip** out, uint32_t* count)
{
    *out   = NULL;
    *count = 0;
    if (!p->digest_count)
        return OK;
    Strip* runs = kmalloc(sizeof(Strip) * p->digest_count);
    if (!runs)
        return ERR_NO_MEMORY;
    for (uint32_t i = 0; i < p->digest_count; i++) {
        uint64_t b = p->digests[i].block;
        if (*count && runs[*count - 1u].start + runs[*count - 1u].count == b)
            runs[*count - 1u].count++;
        else
            runs[(*count)++] = (Strip){ .start = b, .count = 1, .kind = STRIP_DATA };
    }
    *out = runs;
    return OK;
}

/* The bytes the change's Shaped claims on the ribbon: its splices, every Strip
 * they put in, a digest a block the Plot wrote, and the Slip it may lay — the
 * Slips of `rec` when the write rode one and what the change from `cur`
 * brings into a hole (TailSlipClaim) (DocketClaimOfShaped). */
static uint64_t InkBookPlotClaim(const Plot* p, const BookSplice* sp, uint64_t nsp, const FileRecord* cur,
                                 const FileRecord* rec)
{
    uint64_t runs = 0;
    for (uint64_t i = 0; i < nsp; i++)
        runs += sp[i].ninsert;
    return DocketClaimOfShaped(runs, nsp, p->digest_count, TailSlipClaim(cur, rec, p->slip != NULL));
}

/*
 * ‼ A WRITE THAT RODE A SLIP IS FOLDED INTO THE TAIL ITS RECORD KEEPS. The
 * record the splices built keeps the Slips of the record it was built from by
 * their one rule (FileRecordSplice, TailSlipKept); the write's own Slip holds
 * its bytes alone. So the write's Slip takes every other byte of the tail
 * from the kept ones and is the record's tail — the Plot's TailSlip, made
 * before the lock and given no other memory here (§16.5 п.15: the heap that
 * refused it refused it at the write). The Plot holds it until the Ink is
 * laid: an Ink that goes round folds it again onto the record it finds then.
 */
static void InkBookPlotFolded(const Plot* p, FileRecord* rec)
{
    if (!p->slip)
        return;
    TailSlipFold(p->slip, p->slip_off, p->slip_len, rec->slip);
    TailSlipLetGo(rec->slip);
    rec->slip = TailSlipHold(p->slip);
}

/*
 * What an Ink of a Plot makes before the lock: the splices of its window, the
 * record they build, the room for the digests it wrote, and the ground the
 * record stops naming resting in a Fallow node — with the Docket's TAKEs as
 * they stood when the first splice was made to go on the run before it
 * (`joined`, InkBookPlotJoin).
 */
typedef struct {
    BookSplice* sp;
    uint64_t    nsp;
    Strip*      joined;
    uint64_t    takes;
    FileRecord* rec;
    GlossRoom   gloss;
    FallowNode* fallow;
    uint64_t    claim;
} InkBookWork;

static void InkBookWorkLetGo(TagFSVolume* vol, InkBookWork* w)
{
    GlossRoomLetGo(vol, &w->gloss);
    FileRecordRelease(w->rec);
    FallowNodeLetGo(w->fallow);
    kfree(w->joined);
    kfree(w->sp);
}

/*
 * ‼ APPENDS A CYCLE TAKES ARE ONE RUN (§3.7: "продлить последний вставленный
 * прогон"). Each append's window splices its own run in after the file's last,
 * and the record would keep one Strip an append — the stitch one run an append
 * in its Shaped, the Tally one in FILES — though the Allotment laid each on the
 * block that goes on from the one before. So the first run a change puts in
 * grows the record's Strip that ends where it begins when that Strip goes on
 * on the medium into it, both are of this mounting's writing (birth 0), and
 * the record the next stitch lays the change against does not name its ground
 * (DocketBaseLacks): ground no stitch named yet, which the stitch puts in whole
 * — never a run a settled stitch named, which the stitch would have to say
 * was taken out and put in again. A TAKE between this look and the Ink makes
 * that ground the stitch's: the Ink, under its lock, finds the Docket's TAKEs
 * moved and goes round, and the next look grows nothing.
 */
static error_t InkBookPlotJoin(TagFSVolume* vol, const Plot* p, const FileRecord* cur, InkBookWork* w)
{
    StripWalk w_at;
    Strip     last;
    uint64_t  begins = 0;
    if (w->nsp == 0 || w->sp[0].ninsert == 0 || w->sp[0].at == 0)
        return OK;
    const Strip* in = &w->sp[0].insert[0];
    if (in->kind != STRIP_DATA || !StripWalkAt(cur, w->sp[0].at - 1u, &w_at, &begins) ||
        !StripWalkNext(&w_at, &last) || last.kind != STRIP_DATA || begins + last.count != w->sp[0].at ||
        last.start + last.count != in->start || last.birth != 0 || in->birth != 0 ||
        (uint64_t)last.count + in->count > UINT32_MAX)
        return OK;
    TagFSLockTake(&vol->state.lock);
    bool lacks = DocketBaseLacks(vol, p->fid, begins, last.start);
    w->takes   = DocketTakes(vol);
    TagFSLockDrop(&vol->state.lock);
    if (!lacks)
        return OK;
    if (!(w->joined = kmalloc(sizeof(Strip) * w->sp[0].ninsert)))
        return ERR_NO_MEMORY;
    memcpy(w->joined, w->sp[0].insert, sizeof(Strip) * w->sp[0].ninsert);
    w->joined[0].start  = last.start;
    w->joined[0].count += last.count;
    w->sp[0].at        -= last.count;
    w->sp[0].remove    += last.count;
    w->sp[0].insert     = w->joined;
    return OK;
}

#if CONFIG_STRICT || defined(CONFIG_VOLUME_TESTS)
/* Whether the ground the record stops naming is the ground the Plot let go: a
 * disagreement said as a DEFECT, and stopped at under STRICT. */
static bool InkBookFreedAgrees(const Plot* p, Strip* freed, uint64_t nfreed)
{
    Strip*   plots  = kmalloc(sizeof(Strip) * (p->freed_count ? p->freed_count : 1u));
    uint64_t nplots = 0;
    bool     same   = true;
    if (!plots)
        return true;
    if (p->freed_count)
        memcpy(plots, p->freed, sizeof(Strip) * p->freed_count);
    nplots = StripsAsGround(plots, p->freed_count);
    nfreed = StripsAsGround(freed, nfreed);
    same   = nplots == nfreed && (nplots == 0 || memcmp(plots, freed, sizeof(Strip) * nplots) == 0);
    kfree(plots);
    if (same)
        return true;
#if CONFIG_STRICT
    panic("[TagFS] DEFECT: file %llu: the ground its record stops naming is not the ground its Plot let go",
          (unsigned long long)p->fid);
#endif
    kprintf("[TagFS] DEFECT: file %llu: the ground its record stops naming is not the ground its Plot let go — "
            "the record's is what rests\n", (unsigned long long)p->fid);
    return false;
}
#endif

/*
 * ‼ THE GROUND THAT RESTS IS THE GROUND THE RECORD STOPPED NAMING (§3.7, the
 * one door BookPut: "посчитать освобождённые прогоны (old.runs − new.runs)").
 * The Plot keeps its own list of what it let go, and a slip in it — a run of
 * a hole taken for ground — would put another owner's blocks to rest and, once
 * they ripen, hand them out again. The record knows what it owned: the Fallow
 * node is made of the DATA ground the old record names and the new one does
 * not, as the mount's replay of the same Shaped frees it, and nothing else.
 */
static error_t InkBookPlotBuilt(TagFSVolume* vol, const Plot* p, const FileRecord* cur, InkBookWork* w)
{
    Strip*   freed  = NULL;
    Strip*   written = NULL;
    uint64_t nfreed = 0;
    uint32_t nwritten = 0;
    error_t  rc     = PlotConveyance(p, cur, &w->sp, &w->nsp);
    if (rc == OK)
        rc = InkBookPlotJoin(vol, p, cur, w);
    if (rc == OK)
        rc = FileRecordSplice(vol, cur, w->sp, w->nsp, p->size, rtc_get_unix64() * 1000000000ull, &w->rec, &freed,
                              &nfreed);
    if (rc == OK) {
        InkBookPlotFolded(p, w->rec);
        /* The file's first bytes as the Plot left them — a write's laid over
         * the loaded ones, a cut's cut, a growth's the zeros of a hole — as
         * many as its size holds. */
        uint8_t  first[TAGFS_HEAD_MOST] = { 0 };
        uint32_t len = p->size < TAGFS_HEAD_MOST ? (uint32_t)p->size : TAGFS_HEAD_MOST;
        memcpy(first, p->meta.head, len);
        FileRecordHeadSet(w->rec, first, len);
    }
#if CONFIG_STRICT || defined(CONFIG_VOLUME_TESTS)
    if (rc == OK)
        (void)InkBookFreedAgrees(p, freed, nfreed);
#endif
    if (rc == OK && nfreed > UINT32_MAX)
        rc = ERR_NO_MEMORY;
    if (rc == OK && nfreed && !(w->fallow = FallowNodeMake(vol, freed, (uint32_t)nfreed, FALLOW_BY_RECORD, p->fid)))
        rc = ERR_NO_MEMORY;
    if (rc == OK && (rc = InkBookWritten(p, &written, &nwritten)) == OK)
        rc = GlossRoomFor(vol, written, nwritten, &w->gloss);
    if (rc == OK)
        w->claim = InkBookPlotClaim(p, w->sp, w->nsp, cur, w->rec);
    kfree(freed);
    kfree(written);
    return rc;
}

/* The room the Plot holds on the ribbon for its record: none past a mounting
 * it was not held of. */
static uint64_t InkBookPlotHeld(TagFSVolume* vol, const Plot* p)
{
    return p->mount_epoch == tagfs_mount_epoch(vol) ? p->ribbon_held : 0;
}

static error_t InkBookPlotLocked(TagFSVolume* vol, const Plot* p, const RecordWhere* was, InkBookWork* w, Jot* j, Jot** spent,
                                 uint64_t departed, InkReceipt* out)
{
    TagFSState* fs = &vol->state;
    error_t     rc = InkVolumeTakes(vol);
    if (rc != OK)
        return rc;
    if (AwayDepartures(vol) != departed)
        return INK_AGAIN;           /* the medium left and came back since its blocks were held: again */
    RecordWhere now, gone;
    FileRecord* still = FileMapRecordHold(fs->files, p->fid, &now);
    if (!still)
        return ERR_FILE_NOT_FOUND;
    if (now.gen != was->gen || (w->joined && DocketTakes(vol) != w->takes) ||
        (w->fallow && !FallowNodeCommonRoom(vol, w->fallow)))
        rc = INK_AGAIN;
    else
        rc = RibbonClaimHeld(vol, InkBookPlotHeld(vol, p), w->claim);
    if (rc != OK) {
        FileRecordRelease(still);   /* the map holds it: nothing freed here */
        return rc;
    }
    uint64_t     gen   = InkGenNextLocked(vol);
    RecordWhere  where = { .gen = gen, .shape = gen };
    FileMapStake none  = { .count = 0 };
    (void)FileMapRecordPut(fs->files, p->fid, w->rec, &where, &none, &gone);
    w->rec = NULL;
    j->was = still;
    j->now = FileMapRecordHold(fs->files, p->fid, NULL);
    DocketPutLocked(vol, j, spent);
    EndpaperInked(vol, w->sp, w->nsp);
    EndpaperStakesGoneLocked(vol, &p->stakes_counted);   /* its Jot and windows count them now */
    for (uint32_t i = 0; i < p->digest_count; i++)
        GlossPut(vol, &w->gloss, p->digests[i].block, p->digests[i].digest);
    if (w->fallow && FallowNodeCommonheld(vol, w->fallow, gen)) {
        FallowAdd(vol, w->fallow, gen);
        w->fallow = NULL;
    }
    InkDataNeedRaiseLocked(vol, p->data_need);
#ifdef CONFIG_TAGFS_SETTLE_CUT_POINT
    if (p->engross)
        EngrossInked(gen);  /* before the lock goes: the cycle whose snapshot takes `gen` sees it */
#endif
    InkGenLaidLocked(vol, gen);
    out->gen = gen;
    return OK;
}

error_t InkBookPlot(TagFSVolume* vol, const Plot* p, const FileRecord* cur, const RecordWhere* was, uint64_t small_us,
                    InkReceipt* out)
{
    InkBookWork w        = { 0 };
    Jot*        spent    = NULL;
    Jot*        j        = NULL;
    uint64_t    departed = 0;
    error_t     rc       = AwayProofPlot(vol, p, &departed);
    if (rc == OK)
        rc = InkBookPlotBuilt(vol, p, cur, &w);
    if (rc == OK && !(j = JotMake(JOT_SHAPED, p->fid, NULL, NULL)))
        rc = ERR_NO_MEMORY;
    if (rc == OK) {
        w.rec->small_append_us = small_us;
        TagFSLockTake(&vol->state.lock);
        rc = InkBookPlotLocked(vol, p, was, &w, j, &spent, departed, out);
        TagFSLockDrop(&vol->state.lock);
        rc = RibbonClaimSay(vol, rc);
    }
    if (rc != OK)
        JotsLetGo(j);
    JotsLetGo(spent);
    InkBookWorkLetGo(vol, &w);
    return rc;
}

void InkBookPlotNext(TagFSVolume* vol, Plot* p)
{
    FileRecordRelease(p->rec);
    p->rec          = FileMapRecordHold(vol->state.files, p->fid, NULL);
    p->run_count    = 0;
    p->windowed     = false;
    p->window_was   = 0;
    p->window_at    = p->rec ? p->rec->blocks : p->blocks;
    p->blocks       = p->rec ? p->rec->blocks : p->blocks;
    p->blocks_before = p->blocks;
    p->digest_count = 0;
    p->ribbon_held  = 0;    /* the Ink took the hold for its claim */
    PlotSlipLetGo(vol, p);       /* the record holds the write's Slip now, and the block promised to it */
}
