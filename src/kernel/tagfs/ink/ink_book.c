#include "ink_internal.h"
#include "../mount/book_mount.h"
#include "../settle/sewing.h"
#include "../integrity/gloss.h"
#include "../file_map/tail_slip.h"
#include "../helm/engross.h"
#include "../mount/perambulation.h"

/*
 * Format v2's second half of an Ink (ink_internal.h): the writer's half of
 * BookPut (spec_v2 §3.7, §16.2 п.15). Before the lock, the record the file
 * becomes is built, a Jot made for it and the stakes taken; under the lock the
 * file is held to what the first half read, the record put in the mirror, the
 * Jot merged into the Docket for the next stitch (docket.h), the tag index and
 * the counters moved — nothing asked of the heap, nothing laid anywhere: the
 * stitch that takes the Docket lays the records (settle/sewing.c). What the
 * Docket spends, and the records it lets go, are let go after the lock. A
 * change to a file's data comes as a Plot's window spliced into the record it
 * was loaded from, the digests of the blocks it wrote put in the Gloss under
 * the same lock as the record (§8.1), and the ground it let go resting in
 * Fallow (BY_RECORD) — the writer's half of BookPut.
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
                      const char* name, uint32_t add, uint32_t flags, InkReceipt* out)
{
    TagFSState*     fs    = InkState();
    TagBitmapStake* stake = NULL;
    Jot*            spent = NULL;
    FileRecord*     rec   = FileRecordRelabel(cur, name, f->tags, f->count, flags);
    Jot*            j     = rec ? JotMake(JOT_LABELED, fid, NULL, NULL) : NULL;
    error_t         rc    = j ? OK : ERR_NO_MEMORY;
    if (rc == OK && f->adds)
        rc = TagBitmapStakeFor(fs->bitmap_index, &add, 1, fid, &stake);
    if (rc == OK) {
        uint64_t claim = DocketClaimOf(JOT_LABELED, cur, rec);
        TagFSLockTake(&fs->lock);
        rc = InkBookFieldsLocked(fid, was, f, add, &rec, j, stake, claim, &spent, out);
        TagFSLockDrop(&fs->lock);
        rc = RibbonClaimSay(rc);
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
            rc = RibbonClaimSay(rc);
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
                                      FallowNode** fallow, Jot* j, Jot** spent, InkReceipt* out)
{
    TagFSState* fs = InkState();
    error_t     rc = InkVolumeTakes();
    if (rc != OK)
        return rc;
    RecordWhere now, gone;
    FileRecord* still = FileMapRecordHold(fs->files, fid, &now);
    if (!still)
        return ERR_FILE_NOT_FOUND;
    if (now.gen != was->gen || (*fallow && !FallowNodeCommonRoom(*fallow)))
        rc = INK_AGAIN;
    else
        rc = RibbonClaim(DocketClaimOf(JOT_QUITCLAIM, cur, NULL), true);
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
    if (*fallow && FallowNodeCommonheld(*fallow, gen)) {
        FallowAdd(*fallow, gen);
        *fallow = NULL;
    }
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
        rc = InkBookQuitclaimLocked(fid, was, cur, &fallow, j, &spent, out);
        TagFSLockDrop(&fs->lock);
        rc = RibbonClaimSay(rc);
    }
    if (rc == OK) {
        SettleInked(out->gen, false);
        TagBitmapShrink(fs->bitmap_index, &fs->lock, cur->tags, cur->tag_count, fid);
    } else {
        JotsLetGo(j);
    }
    FallowNodeLetGo(fallow);    /* added, or left with no ground of its own to rest, or refused */
    JotsLetGo(spent);
    return rc;
}

// ----------------------------------------------------------------------------
// A file's data (spec_v2 §2.8 Shaped, §3.1, §8.1)
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

static void InkBookWorkLetGo(InkBookWork* w)
{
    GlossRoomLetGo(&w->gloss);
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
static error_t InkBookPlotJoin(const Plot* p, const FileRecord* cur, InkBookWork* w)
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
    TagFSLockTake(&InkState()->lock);
    bool lacks = DocketBaseLacks(p->fid, begins, last.start);
    w->takes   = DocketTakes();
    TagFSLockDrop(&InkState()->lock);
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
static error_t InkBookPlotBuilt(const Plot* p, const FileRecord* cur, InkBookWork* w)
{
    Strip*   freed  = NULL;
    Strip*   written = NULL;
    uint64_t nfreed = 0;
    uint32_t nwritten = 0;
    error_t  rc     = PlotConveyance(p, cur, &w->sp, &w->nsp);
    if (rc == OK)
        rc = InkBookPlotJoin(p, cur, w);
    if (rc == OK)
        rc = FileRecordSplice(cur, w->sp, w->nsp, p->size, rtc_get_unix64() * 1000000000ull, &w->rec, &freed,
                              &nfreed);
    if (rc == OK)
        InkBookPlotFolded(p, w->rec);
#if CONFIG_STRICT || defined(CONFIG_VOLUME_TESTS)
    if (rc == OK)
        (void)InkBookFreedAgrees(p, freed, nfreed);
#endif
    if (rc == OK && nfreed > UINT32_MAX)
        rc = ERR_NO_MEMORY;
    if (rc == OK && nfreed && !(w->fallow = FallowNodeMake(freed, (uint32_t)nfreed, FALLOW_BY_RECORD, p->fid)))
        rc = ERR_NO_MEMORY;
    if (rc == OK && (rc = InkBookWritten(p, &written, &nwritten)) == OK)
        rc = GlossRoomFor(written, nwritten, &w->gloss);
    if (rc == OK)
        w->claim = InkBookPlotClaim(p, w->sp, w->nsp, cur, w->rec);
    kfree(freed);
    kfree(written);
    return rc;
}

/* The room the Plot holds on the ribbon for its record: none past a mounting
 * it was not held of. */
static uint64_t InkBookPlotHeld(const Plot* p)
{
    return p->mount_epoch == tagfs_mount_epoch() ? p->ribbon_held : 0;
}

static error_t InkBookPlotLocked(const Plot* p, const RecordWhere* was, InkBookWork* w, Jot* j, Jot** spent,
                                 uint64_t departed, InkReceipt* out)
{
    TagFSState* fs = InkState();
    error_t     rc = InkVolumeTakes();
    if (rc != OK)
        return rc;
    if (AwayDepartures() != departed)
        return INK_AGAIN;           /* the medium left and came back since its blocks were held: again */
    RecordWhere now, gone;
    FileRecord* still = FileMapRecordHold(fs->files, p->fid, &now);
    if (!still)
        return ERR_FILE_NOT_FOUND;
    if (now.gen != was->gen || (w->joined && DocketTakes() != w->takes) ||
        (w->fallow && !FallowNodeCommonRoom(w->fallow)))
        rc = INK_AGAIN;
    else
        rc = RibbonClaimHeld(InkBookPlotHeld(p), w->claim);
    if (rc != OK) {
        FileRecordRelease(still);   /* the map holds it: nothing freed here */
        return rc;
    }
    uint64_t     gen   = InkGenNextLocked();
    RecordWhere  where = { .gen = gen, .shape = gen };
    FileMapStake none  = { .count = 0 };
    (void)FileMapRecordPut(fs->files, p->fid, w->rec, &where, 0, &none, &gone);
    w->rec = NULL;
    j->was = still;
    j->now = FileMapRecordHold(fs->files, p->fid, NULL);
    DocketPutLocked(j, spent);
    EndpaperInked(w->sp, w->nsp);
    EndpaperStakesGoneLocked(&p->stakes_counted);   /* its Jot and windows count them now */
    for (uint32_t i = 0; i < p->digest_count; i++)
        GlossPut(&w->gloss, p->digests[i].block, p->digests[i].digest);
    if (w->fallow && FallowNodeCommonheld(w->fallow, gen)) {
        FallowAdd(w->fallow, gen);
        w->fallow = NULL;
    }
    InkDataNeedRaiseLocked(p->data_need);
#ifdef CONFIG_TAGFS_SETTLE_CUT_POINT
    if (p->engross)
        EngrossInked(gen);  /* before the lock goes: the cycle whose snapshot takes `gen` sees it */
#endif
    InkGenLaidLocked(gen);
    out->gen = gen;
    return OK;
}

error_t InkBookPlot(const Plot* p, const FileRecord* cur, const RecordWhere* was, uint64_t small_us,
                    InkReceipt* out)
{
    InkBookWork w        = { 0 };
    Jot*        spent    = NULL;
    Jot*        j        = NULL;
    uint64_t    departed = 0;
    error_t     rc       = AwayProofPlot(p, &departed);
    if (rc == OK)
        rc = InkBookPlotBuilt(p, cur, &w);
    if (rc == OK && !(j = JotMake(JOT_SHAPED, p->fid, NULL, NULL)))
        rc = ERR_NO_MEMORY;
    if (rc == OK) {
        w.rec->small_append_us = small_us;
        TagFSLockTake(&InkState()->lock);
        rc = InkBookPlotLocked(p, was, &w, j, &spent, departed, out);
        TagFSLockDrop(&InkState()->lock);
        rc = RibbonClaimSay(rc);
    }
    if (rc != OK)
        JotsLetGo(j);
    JotsLetGo(spent);
    InkBookWorkLetGo(&w);
    return rc;
}

void InkBookPlotNext(Plot* p)
{
    FileRecordRelease(p->rec);
    p->rec          = FileMapRecordHold(InkState()->files, p->fid, NULL);
    p->run_count    = 0;
    p->windowed     = false;
    p->window_was   = 0;
    p->window_at    = p->rec ? p->rec->blocks : p->blocks;
    p->blocks       = p->rec ? p->rec->blocks : p->blocks;
    p->blocks_before = p->blocks;
    p->runs_before  = p->rec ? p->rec->strip_count : 0;
    p->digest_count = 0;
    p->ribbon_held  = 0;    /* the Ink took the hold for its claim */
    PlotSlipLetGo(p);       /* the record holds the write's Slip now, and the block promised to it */
}
