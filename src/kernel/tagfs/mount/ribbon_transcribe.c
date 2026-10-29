#include "ribbon.h"
#include "errata.h"
#include "book_mount.h"
#include "../file_map/file_map.h"
#include "../integrity/gloss.h"
#include "../tag_registry/tag_registry.h"
#include "../settle/dog_ear.h"
#include "../settle/loose_leaves.h"

/*
 * RibbonTranscribe — pass 2 of the ribbon (spec_v2 §4.5 п.5, §3.7): every
 * stitch pass 1 sewed on, in order, each record through the rule of its kind
 * into the memory every format shares — the map of live files, the registry,
 * the tag index, the Use Context.
 *
 * ‼ ONE DOOR OF THE MIRROR. A record is replayed the way an Ink is made: the
 * file's record as it stands is read from the mirror, its base held to the
 * file's gen, the change applied by FileRecordSplice / FileRecordRelabel /
 * FileRecordBuild — every allocation made here, with no lock held — and the
 * new record put in the map in one hold of the map's lock (FileMapRecordPut),
 * the ground it no longer names handed to the Fallow of the mount (M-1).
 * It is the half of BookPut a mount takes (§16.2 п.15); the other half — a
 * writer's — is the next step's.
 *
 * ‼ A STITCH IS APPLIED WHOLE OR NOT AT ALL. What a stitch changes is gathered
 * first (RibbonStage) and laid into the mirror only when every record of it
 * has been read and taken: a stitch a record of which this reader may not
 * apply — a kind it does not know, a body longer than it knows, a Born of a
 * file that exists, a Coined that renames an id — is not applied, and the
 * volume may only be read (UnderGlass). A record that does not fit the file
 * it names (its base, its blocks, its Slip) is the rule of rot (§3.9): said,
 * and the file rolled back to where the stitch found it — every record of it
 * in that stitch passed over, as the Undone the session's first stitch lays
 * down for the verdict will pass them over for the next mount (§2.8, errata.h),
 * so this mount and that one leave the file the same (H-1).
 *
 * Read the way volume-read replays the same ribbon (tools/volumeread/
 * bookmount.py _Undone, _Replay, _Record, _File, _Settle).
 */

/* One u64 more for a growing list: false with no memory. */
bool RibbonListAdd(uint64_t** list, uint32_t* n, uint32_t* cap, uint64_t v)
{
    if (*n == *cap) {
        uint32_t  more = *cap ? *cap * 2u : 16u;
        uint64_t* to   = kmalloc(sizeof(uint64_t) * more);
        if (!to)
            return false;
        if (*n)
            memcpy(to, *list, sizeof(uint64_t) * *n);
        kfree(*list);
        *list = to;
        *cap  = more;
    }
    (*list)[(*n)++] = v;
    return true;
}

static int RibbonU64Order(const void* a, const void* b, void* ctx)
{
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    (void)ctx;
    return x < y ? -1 : x > y;
}

void RibbonListSorted(uint64_t* list, uint32_t* n)
{
    uint32_t kept = 0;
    KSort(list, *n, sizeof(uint64_t), RibbonU64Order, NULL);
    for (uint32_t i = 0; i < *n; i++)
        if (!kept || list[kept - 1u] != list[i])
            list[kept++] = list[i];
    *n = kept;
}

bool RibbonListHas(const uint64_t* list, uint32_t n, uint64_t v)
{
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (list[mid] < v)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo < n && list[lo] == v;
}

/* A grown array of `size`-byte items: false with no memory. */
bool RibbonGrow(void** at, uint64_t n, uint64_t* cap, size_t size)
{
    if (n < *cap)
        return true;
    uint64_t more = *cap ? *cap * 2u : 8u;
    void*    to   = kmalloc(size * more);
    if (!to)
        return false;
    if (n)
        memcpy(to, *at, size * n);
    kfree(*at);
    *at  = to;
    *cap = more;
    return true;
}

// ----------------------------------------------------------------------------
// The Undones, gathered first
// ----------------------------------------------------------------------------

error_t RibbonSkipAdd(RibbonSkipSet* set, uint64_t seq, uint64_t fid)
{
    uint64_t cap = set->cap;
    if (!RibbonGrow((void**)&set->at, set->n, &cap, sizeof(RibbonSkip)))
        return ERR_NO_MEMORY;
    set->cap = (uint32_t)cap;
    set->at[set->n++] = (RibbonSkip){ seq, fid };
    return OK;
}

static int RibbonSkipOrder(const void* a, const void* b, void* ctx)
{
    const RibbonSkip* x = a;
    const RibbonSkip* y = b;
    (void)ctx;
    if (x->seq != y->seq)
        return x->seq < y->seq ? -1 : 1;
    return x->fid < y->fid ? -1 : x->fid > y->fid;
}

void RibbonSkipSorted(RibbonSkipSet* set)
{
    uint32_t kept = 0;
    KSort(set->at, set->n, sizeof(RibbonSkip), RibbonSkipOrder, NULL);
    for (uint32_t i = 0; i < set->n; i++)
        if (!kept || RibbonSkipOrder(&set->at[kept - 1u], &set->at[i], NULL) != 0)
            set->at[kept++] = set->at[i];
    set->n = kept;
}

/* Where (seq, fid) lies in a sorted set, or would. */
static uint32_t RibbonSkipAt(const RibbonSkipSet* set, uint64_t seq, uint64_t fid)
{
    RibbonSkip want = { seq, fid };
    uint32_t   lo = 0, hi = set->n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (RibbonSkipOrder(&set->at[mid], &want, NULL) < 0)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

error_t RibbonSkipPut(RibbonSkipSet* set, uint64_t seq, uint64_t fid)
{
    uint32_t at  = RibbonSkipAt(set, seq, fid);
    uint64_t cap = set->cap;
    if (at < set->n && set->at[at].seq == seq && set->at[at].fid == fid)
        return OK;
    if (!RibbonGrow((void**)&set->at, set->n, &cap, sizeof(RibbonSkip)))
        return ERR_NO_MEMORY;
    set->cap = (uint32_t)cap;
    memmove(&set->at[at + 1u], &set->at[at], sizeof(RibbonSkip) * (set->n - at));
    set->at[at] = (RibbonSkip){ seq, fid };
    set->n++;
    return OK;
}

bool RibbonSkipHas(const RibbonSkipSet* set, uint64_t seq, uint64_t fid)
{
    uint32_t at = RibbonSkipAt(set, seq, fid);
    return at < set->n && set->at[at].seq == seq && set->at[at].fid == fid;
}

/* Why an Undone may name a stitch, in bookmount.py's words. */
static const char* RibbonUndoneWhy(uint64_t why, char* out, size_t room)
{
    if (why == UNDONE_NOT_LANDED)
        return "why 1: its data did not land (NOT_LANDED)";
    if (why == UNDONE_ROT)
        return "why 2: the file did not match its base and was rolled back (the rule of rot)";
    ksnprintf(out, room, "why %llu, which this reader does not know; the Undone stands all the same",
              (unsigned long long)why);
    return out;
}

/* Whether stitch `seq` was sewn on. */
static bool RibbonSeqSewn(const RibbonWalk* w, uint64_t seq)
{
    for (uint32_t i = 0; i < w->nstitch; i++)
        if (w->stitch[i]->seq == seq)
            return true;
    return false;
}

/* One Undone of stitch `u`, the `i`-th record: its fids passed over in the
 * stitch it names, said — or the stitch not applied for it. */
static error_t RibbonUndoneOne(RibbonReplay* rp, const RibbonUnit* u, uint32_t i)
{
    RibbonWalk*         w = rp->w;
    const RibbonRecord* r = &u->records[i];
    RibbonUndone        d;
    char                why[160], fids[160], said[96];
    uint32_t            used = 0, at = 0;
    uint64_t            id = 0;
    if (RibbonUndoneGet(u->stream + r->body, r->len, &d, &used) == TALLY_BODY_BROKEN) {
        ksnprintf(why, sizeof(why), "stitch %llu carries an Undone that does not read", (unsigned long long)u->seq);
        if (!rp->only)
            TagFSUnderGlass(why);
        rp->passed += rp->only ? 1u : 0u;
        return RibbonListAdd(&rp->refused, &rp->nrefused, &rp->caprefused, u->seq) ? OK : ERR_NO_MEMORY;
    }
    bool after = false;
    for (uint32_t k = 0; k < i; k++)
        after |= u->records[k].kind != RIBBON_UNDONE;
    uint64_t stood = w->slot->head.stitch_seq;
    if (after || !d.seq_back) {
        ksnprintf(why, sizeof(why), "stitch %llu carries an Undone after another record — it is not applied",
                  (unsigned long long)u->seq);
    } else if (d.seq_back > u->seq || u->seq - d.seq_back <= stood) {
        return OK;                                      /* absorbed by the Tally (§2.8) */
    } else if (!RibbonSeqSewn(w, u->seq - d.seq_back)) {
        ksnprintf(why, sizeof(why), "stitch %llu carries an Undone of seq %llu, which is no stitch of this ribbon "
                  "before it — it is not applied", (unsigned long long)u->seq,
                  (unsigned long long)(u->seq - d.seq_back));
    } else {
        size_t n = 0;
        for (uint64_t k = 0; k < d.nfid; k++) {
            TallyIdNext(d.fids, d.fids_bytes, &at, &id, k == 0);
            if (RibbonSkipAdd(&rp->undone, u->seq - d.seq_back, id) != OK)
                return ERR_NO_MEMORY;
            if (n + 24u < sizeof(fids))
                n += (size_t)ksnprintf(fids + n, sizeof(fids) - n, k ? ", %llu" : "%llu", (unsigned long long)id);
        }
        if (!d.nfid)
            ksnprintf(fids, sizeof(fids), "none");
        RibbonSay(w, RIBBON_CODE_UNDONE, "stitch %llu's Undone passes over stitch %llu's records of fid(s) %s — %s "
                  "(§2.8)", (unsigned long long)u->seq, (unsigned long long)(u->seq - d.seq_back), fids,
                  RibbonUndoneWhy(d.why, said, sizeof(said)));
        return OK;
    }
    if (!rp->only)
        TagFSUnderGlass(why);     /* of an older slot's ribbon (Facsimile): the slot does not stand in */
    rp->passed += rp->only ? 1u : 0u;
    return RibbonListAdd(&rp->refused, &rp->nrefused, &rp->caprefused, u->seq) ? OK : ERR_NO_MEMORY;
}

// ----------------------------------------------------------------------------
// A stitch's stage
// ----------------------------------------------------------------------------

/* Why the stitch is not applied, the first reason kept. */
void RibbonRefuse(RibbonStage* st, const char* fmt, ...)
{
    va_list ap;
    if (st->refused[0])
        return;
    va_start(ap, fmt);
    kvsnprintf(st->refused, sizeof(st->refused), fmt, ap);
    va_end(ap);
}

/* Where `fid` lies among the files of the stage, rising by fid, or would. */
static uint32_t RibbonStagedAt(const RibbonStage* st, uint64_t fid)
{
    uint32_t lo = 0, hi = st->nfiles;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (st->files[mid].fid < fid)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

static RibbonStaged* RibbonStagedOf(RibbonStage* st, uint64_t fid)
{
    uint32_t at = RibbonStagedAt(st, fid);
    return at < st->nfiles && st->files[at].fid == fid ? &st->files[at] : NULL;
}

/* The file as the stitch leaves it so far — the stage's (borrowed; NULL when
 * the stitch quitclaimed it) or the mirror's, held, *held said so: the caller
 * lets a held one go. */
FileRecord* RibbonFileNow(RibbonStage* st, uint64_t fid, bool* held)
{
    RibbonStaged* s = RibbonStagedOf(st, fid);
    *held = false;
    if (s)
        return s->rec;
    FileRecord* rec = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    *held = rec != NULL;
    return rec;
}

/* `rec` — one reference, the stage's from here; NULL for a file gone — as the
 * file the stitch leaves. ERR_NO_MEMORY (and `rec` let go). */
error_t RibbonStagePut(RibbonStage* st, uint64_t fid, FileRecord* rec)
{
    RibbonStaged* s = RibbonStagedOf(st, fid);
    if (s) {
        FileRecordRelease(s->rec);
        s->rec = rec;
        return OK;
    }
    uint64_t cap = st->capfiles;
    if (!RibbonGrow((void**)&st->files, st->nfiles, &cap, sizeof(RibbonStaged))) {
        FileRecordRelease(rec);
        return ERR_NO_MEMORY;
    }
    uint32_t at = RibbonStagedAt(st, fid);
    st->capfiles = (uint32_t)cap;
    memmove(&st->files[at + 1u], &st->files[at], sizeof(RibbonStaged) * (st->nfiles - at));
    st->files[at] = (RibbonStaged){ fid, rec };
    st->nfiles++;
    return OK;
}

/* DATA ground the stitch lets go, in Strips a u32 counts; `once` keeps a run
 * the stitch already let go from being let go twice. */
error_t RibbonStageFallow(RibbonStage* st, uint64_t start, uint64_t count, bool once)
{
    for (uint64_t i = 0; once && i < st->nfallow; i++)
        if (st->fallow[i].start == start && st->fallow[i].count == count)
            return OK;
    while (count) {
        uint32_t n = count > UINT32_MAX ? UINT32_MAX : (uint32_t)count;
        if (!RibbonGrow((void**)&st->fallow, st->nfallow, &st->capfallow, sizeof(Strip)))
            return ERR_NO_MEMORY;
        st->fallow[st->nfallow++] = (Strip){ .start = start, .count = n, .kind = STRIP_DATA };
        start += n;
        count -= n;
    }
    return OK;
}

/* The high bits of a file's flags into `list`: the file's entry set, or one
 * added — for 0 too when `zero` keeps it (a stage's: the stitch clears bits
 * the mirror's file wears), else 0 adds none. */
static error_t RibbonHighInto(RibbonHigh** list, uint32_t* n, uint32_t* capn, uint64_t fid, uint32_t high,
                              bool zero)
{
    for (uint32_t i = 0; i < *n; i++) {
        if ((*list)[i].fid == fid) {
            (*list)[i].high = high;
            return OK;
        }
    }
    if (!high && !zero)
        return OK;
    uint64_t cap = *capn;
    if (!RibbonGrow((void**)list, *n, &cap, sizeof(RibbonHigh)))
        return ERR_NO_MEMORY;
    *capn = (uint32_t)cap;
    (*list)[(*n)++] = (RibbonHigh){ fid, high };
    return OK;
}

/* Kept by the stage, so a stitch not applied — or gathered again without the
 * file — leaves the replay's as it found them. */
error_t RibbonHighSet(RibbonStage* st, uint64_t fid, uint32_t high)
{
    return RibbonHighInto(&st->high, &st->nhigh, &st->caphigh, fid, high, true);
}

uint64_t RibbonFlagsOf(const RibbonReplay* rp, const RibbonStage* st, const FileRecord* rec)
{
    for (uint32_t i = 0; st && i < st->nhigh; i++)
        if (st->high[i].fid == rec->file_id)
            return (uint64_t)st->high[i].high << 32 | rec->flags;
    for (uint32_t i = 0; i < rp->nhigh; i++)
        if (rp->high[i].fid == rec->file_id)
            return (uint64_t)rp->high[i].high << 32 | rec->flags;
    return rec->flags;
}

// ----------------------------------------------------------------------------
// A stitch applied whole
// ----------------------------------------------------------------------------

/* The tags `rec` wears that `other` does not — NULL wearing none — each out
 * of (`leave`) or into the tag index. ERR_NO_MEMORY. */
static error_t RibbonTagsMoved(const FileRecord* rec, const FileRecord* other, uint64_t fid, bool leave)
{
    TagFSState* fs = tagfs_get_state();
    for (uint32_t i = 0; rec && i < rec->tag_count; i++) {
        bool both = false;
        for (uint32_t k = 0; other && k < other->tag_count; k++)
            both |= other->tags[k] == rec->tags[i];
        if (both)
            continue;
        if (leave)
            TagBitmapLeave(fs->bitmap_index, rec->tags[i], fid);
        else if (tag_bitmap_set(fs->bitmap_index, rec->tags[i], fid) != 0)
            return ERR_NO_MEMORY;
    }
    return OK;
}

/* One file of the stage into the map of live files: its record put — the map
 * takes the stage's reference — or the file dropped; its tags moved in the
 * index; the file counted. */
static error_t RibbonFileLaid(RibbonReplay* rp, RibbonStaged* s)
{
    TagFSState*  fs  = tagfs_get_state();
    FileRecord*  old = FileMapRecordHold(fs->files, s->fid, NULL);
    RecordWhere  where, was;
    FileMapStake stake = { .count = 0 };
    error_t      rc    = RibbonTagsMoved(old, s->rec, s->fid, true);
    memset(&where, 0, sizeof(where));
    DogEarFileChanged(s->fid, old, s->rec);       /* changed since the Tally the mount stands on */
    if (rc == OK)
        rc = RibbonTagsMoved(s->rec, old, s->fid, false);
    if (rc == OK && s->rec) {
        rc = FileMapStakeFor(fs->files, s->fid, &stake);
        if (rc == OK)
            rc = FileMapRecordPut(fs->files, s->fid, s->rec, &where, 0, &stake, &was);
        else
            FileRecordRelease(s->rec);    /* no stake: the record is the stage's still, and let go here */
        FileMapStakeLetGo(&stake);
        s->rec = NULL;                    /* the map's, put or let go */
        g_book.files += rc == OK && !old;
    } else if (rc == OK) {
        FileMapDrop(fs->files, s->fid, &was);
        g_book.files -= old != NULL;
    }
    FileRecordRelease(old);
    if (rc == OK && !RibbonListAdd(&rp->touched, &rp->ntouched, &rp->captouched, s->fid))
        rc = ERR_NO_MEMORY;
    return rc;
}

/* Everything the stage gathered laid into memory: the files, the tags it
 * coins into the registry, the Use Context, the ground it let go to the
 * mount's Fallow, the digests it carries into the Gloss. */
static error_t RibbonStageLaid(RibbonReplay* rp, RibbonStage* st)
{
    error_t rc = OK;
    char    why[160];
    for (uint32_t i = 0; rc == OK && i < st->nfiles; i++)
        rc = RibbonFileLaid(rp, &st->files[i]);
    for (uint32_t i = 0; rc == OK && i < st->ncoins; i++) {
        const TallyTag* t = &st->coins[i].tag;
        DogEarPut(TALLY_TAGS, (TallyKey){ st->coins[i].id, 0 }, (TallyKey){ st->coins[i].id + 1u, 0 });
        if (!st->coins[i].held) {
            rc = BookTagAsideNoted(st->coins[i].id, t);
            continue;
        }
        rc = RegistryCoin(tagfs_get_state()->registry, (uint32_t)st->coins[i].id,
                          (t->flags & TALLY_TAG_HAS_VALUE) ? TAGFS_TAG_FLAG_HAS_VALUE : 0, t->key,
                          (uint8_t)t->key_len, t->value, (uint16_t)t->value_len);
        if (rc == ERR_ALREADY_EXISTS) {
            ksnprintf(why, sizeof(why), "tag id %llu's text is another id's too, or its id another text's — the "
                      "registry cannot hold the two at once", (unsigned long long)st->coins[i].id);
            TagFSUnderGlass(why);
            rc = OK;
        }
    }
    if (rp->only)
        return rc;      /* Facsimile's: the rest is the Tally's the mount stands on */
    if (st->use_set) {
        DogEarPut(TALLY_USE, (TallyKey){ 0, 0 }, (TallyKey){ 1, 0 });
        rp->use_set = true;
        rp->use     = st->use;
        rp->use_len = st->use_len;
    }
    for (uint64_t i = 0; rc == OK && i < st->nfallow; i++) {
        if (!RibbonGrow((void**)&g_book.fallow, g_book.nfallow, &g_book.capfallow, sizeof(Strip)))
            return ERR_NO_MEMORY;
        g_book.fallow[g_book.nfallow++] = st->fallow[i];
    }
    for (uint64_t i = 0; rc == OK && i < st->ndigests; i++)
        rc = GlossNote(st->digests[i].block, st->digests[i].digest);
    for (uint32_t i = 0; rc == OK && i < st->nhigh; i++)
        rc = RibbonHighInto(&rp->high, &rp->nhigh, &rp->caphigh, st->high[i].fid, st->high[i].high, false);
    return rc;
}

/* The stage let go: the records it held and what it gathered. */
static void RibbonStageLetGo(RibbonStage* st)
{
    for (uint32_t i = 0; i < st->nfiles; i++)
        FileRecordRelease(st->files[i].rec);
    kfree(st->files);
    kfree(st->coins);
    kfree(st->fallow);
    kfree(st->digests);
    kfree(st->high);
}

/* Stitch `u`'s records through their rules into a stage of their own: OK,
 * or ERR_NO_MEMORY. */
static error_t RibbonStitchGathered(RibbonReplay* rp, const RibbonUnit* u, RibbonStage* st)
{
    error_t rc = OK;
    memset(st, 0, sizeof(*st));
    st->u = u;
    if (u->head_read && u->longer)
        RibbonRefuse(st, "its StitchHead or Clasp is %u byte(s) longer than this reader knows — written by a "
                     "kernel that knows more", u->longer);
    for (uint32_t i = 0; rc == OK && !st->refused[0] && i < u->nrecords; i++)
        if (!rp->only || RibbonRecordWithin(u, &u->records[i], rp->only, rp->only_lo, rp->only_hi))
            rc = RibbonRecordApplied(rp, st, &u->records[i]);
    if (rc == OK && !st->refused[0])
        RibbonFreshHeld(st);
    return rc;
}

/*
 * Stitch `u` replayed: every record through its rule, then the whole of it
 * laid into memory — or none of it, said, when a record of it may not be
 * applied.
 *
 * ‼ A FILE THE RULE OF ROT ROLLS BACK LEAVES THE STITCH WHOLE (§2.8, H-1). A
 * record that does not fit its file puts (seq, fid) among the skips at once
 * (ribbon_records.c, RibbonMismatch) — the file's records after it in the
 * stitch are passed over from there — and the stitch is gathered again from
 * its first record, so the file's records BEFORE it are passed over too: the
 * file stands where the stitch found it, and its fresh ground rests in
 * Fallow, exactly as the next mount leaves it once the Undone of the verdict
 * passes over every record of that fid in the stitch. Each gathering again
 * passes over one fid more, so it ends; the files' records of other fids are
 * gathered as they were.
 */
static error_t RibbonStitchApplied(RibbonReplay* rp, const RibbonUnit* u)
{
    RibbonStage* st = kmalloc(sizeof(RibbonStage));
    char         why[256];
    uint32_t     ext = rp->next;
    error_t      rc  = OK;
    if (!st)
        return ERR_NO_MEMORY;
    rc = RibbonStitchGathered(rp, u, st);
    while (rc == OK && st->rot && !st->refused[0]) {
        RibbonStageLetGo(st);
        rp->next = ext;                 /* the files a gathering let go noted no extension */
        rc       = RibbonStitchGathered(rp, u, st);
    }
    if (rc == OK && st->refused[0] && rp->only) {
        rp->passed++;                   /* Facsimile's: the slot does not stand in for the range */
    } else if (rc == OK && st->refused[0]) {
        ksnprintf(why, sizeof(why), "stitch %llu is not applied: %s", (unsigned long long)u->seq, st->refused);
        TagFSUnderGlass(why);
    } else if (rc == OK && !st->refused[0]) {
        rc = RibbonStageLaid(rp, st);
        g_book.said.applied += rc == OK && !rp->only;
    }
    RibbonStageLetGo(st);
    kfree(st);
    return rc;
}

// ----------------------------------------------------------------------------
// What the replay leaves the mount to decide (bookmount.py _Settle)
// ----------------------------------------------------------------------------

/* The DATA ground of `fid` outside the data run, as the census writes it:
 * how many runs. */
static uint32_t RibbonOutside(const FileRecord* rec, char* out, size_t room)
{
    uint64_t  lo = g_book.frame.data_block, hi = g_book.data_end;
    uint32_t  n  = 0;
    size_t    at = 0;
    StripWalk walk;
    Strip     run;
    out[0] = '\0';
    StripWalkBegin(rec, &walk);
    while (StripWalkNextGround(&walk, &run)) {
        if (run.start >= lo && run.start + run.count <= hi)
            continue;
        if (at + 40u < room)
            at += (size_t)ksnprintf(out + at, room - at, n ? ", %llu+%u" : "%llu+%u",
                                    (unsigned long long)run.start, run.count);
        n++;
    }
    return n;
}

/* Whether a DATA run of `rec` meets one of another live file's. */
static bool RibbonOnOthers(const FileRecord* rec)
{
    TagFSState* fs   = tagfs_get_state();
    uint64_t    from = 0, ids[16];
    uint32_t    n;
    while ((n = FileMapRows(fs->files, from, ids, 16)) > 0) {
        for (uint32_t i = 0; i < n; i++) {
            FileRecord* other = ids[i] == rec->file_id ? NULL : FileMapRecordHold(fs->files, ids[i], NULL);
            StripWalk   a, b;
            Strip       x, y;
            bool        meet = false;
            if (!other)
                continue;
            StripWalkBegin(other, &a);
            while (!meet && StripWalkNextGround(&a, &x)) {
                StripWalkBegin(rec, &b);
                while (!meet && StripWalkNextGround(&b, &y))
                    meet = x.start < y.start + y.count && y.start < x.start + x.count;
            }
            FileRecordRelease(other);
            if (meet)
                return true;
        }
        from = ids[n - 1] + 1;
    }
    return false;
}

/* A file the replay changed that stands outside the data run: said, locked. */
static error_t RibbonOutsideLocked(RibbonWalk* w, uint64_t fid)
{
    char        runs[200];
    FileRecord* rec = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    if (!rec)
        return OK;
    uint32_t n = RibbonOutside(rec, runs, sizeof(runs));
    FileRecordRelease(rec);
    if (!n)
        return OK;
    RibbonSay(w, RIBBON_CODE_EXTENT_OUTSIDE, "fid %llu names blocks %s outside the data run [%llu, %llu) — it is "
              "locked", (unsigned long long)fid, runs, (unsigned long long)g_book.frame.data_block,
              (unsigned long long)g_book.data_end);
    return BookFileLockLive(fid, "it names ground outside the data run");
}

/* What of a file the replay leaves standing this kernel cannot carry on: a
 * run with an extension, flags past 32 bits. Slips on its last block it
 * carries on (§16.5 п.13) — counted, true, for the Tally that may not take
 * its slice before they are laid in a block (LooseLeaves). */
static bool RibbonCarriedSaid(const RibbonReplay* rp, uint64_t fid)
{
    char        why[192];
    FileRecord* rec = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    if (!rec)
        return false;
    bool slipped = rec->slip != NULL;
    if (RibbonListHas(rp->ext, rp->next, fid)) {
        ksnprintf(why, sizeof(why), "a run of fid %llu carries an extension this kernel cannot carry on (CODEC, "
                  "SHARED or COPY, §8)", (unsigned long long)fid);
        TagFSUnderGlass(why);
    }
    if (RibbonFlagsOf(rp, NULL, rec) >> 32) {
        ksnprintf(why, sizeof(why), "fid %llu carries flags past the 32 bits this kernel holds — written by a "
                  "kernel that knows more", (unsigned long long)fid);
        TagFSUnderGlass(why);
    }
    FileRecordRelease(rec);
    return slipped;
}

/* ‼ THE SLIPS A MOUNT CARRIES ON ARE PROMISED AGAIN BY THE RECORDS THAT HOLD
 * THEM (file_map.h, FileMapSlips; block_map.h, AllotmentPromised). A Tally
 * lays no Slip, so every one the mount holds is a file's the replay touched:
 * the Slips the Tally waits for (LooseLeaves) and the blocks promised are one
 * number here, and two is a DEFECT, said. */
static void RibbonPromisesHeld(uint64_t loose)
{
    uint64_t slips = FileMapSlips(tagfs_get_state()->files);
    if (slips == loose)
        return;
#if CONFIG_STRICT
    panic("[TagFS] DEFECT: the mount carries %llu Slip(s) on and its records hold %llu", (unsigned long long)loose,
          (unsigned long long)slips);
#endif
    kprintf("[TagFS] DEFECT: the mount carries %llu Slip(s) on and its records hold %llu — the blocks promised to "
            "their tails are the records'\n", (unsigned long long)loose, (unsigned long long)slips);
}

/* The replay's last word on the files it changed: those outside the data run
 * locked; those rolled back by the rule of rot onto blocks another file now
 * holds locked (§3.9 — the rule rolls back only onto ground no one else
 * owns), and no verdict of the mount's: a file locked is not rolled back, and
 * the next mount locks it again by the same rule; what this kernel cannot
 * carry on said. */
static error_t RibbonSettled(RibbonReplay* rp)
{
    error_t rc = OK;
    RibbonListSorted(rp->touched, &rp->ntouched);
    RibbonListSorted(rp->mismatched, &rp->nmismatched);
    RibbonListSorted(rp->ext, &rp->next);
    for (uint32_t i = 0; rc == OK && i < rp->ntouched; i++)
        rc = RibbonOutsideLocked(rp->w, rp->touched[i]);
    for (uint32_t i = 0; rc == OK && i < rp->nmismatched; i++) {
        FileRecord* rec = FileMapRecordHold(tagfs_get_state()->files, rp->mismatched[i], NULL);
        bool        on  = rec && RibbonOnOthers(rec);
        FileRecordRelease(rec);
        if (on) {
            ErrataLeaves(rp->mismatched[i]);
            rc = BookFileLockLive(rp->mismatched[i], "it is rolled back to blocks another file now holds (§3.9)");
        }
    }
    uint64_t loose = 0;
    for (uint32_t i = 0; rc == OK && i < rp->ntouched; i++)
        loose += RibbonCarriedSaid(rp, rp->touched[i]);
    LooseLeavesMounted(loose);
    if (rc == OK)
        RibbonPromisesHeld(loose);
    return rc;
}

// ----------------------------------------------------------------------------
// Pass 2
// ----------------------------------------------------------------------------

static void RibbonReplayLetGo(RibbonReplay* rp)
{
    kfree(rp->undone.at);
    kfree(rp->refused);
    kfree(rp->touched);
    kfree(rp->mismatched);
    kfree(rp->ext);
    kfree(rp->high);
    kfree(rp);
}

/* Every stitch's Undones gathered (§2.8): the records they pass over, or the
 * stitch they stand in not applied. */
static error_t RibbonUndones(RibbonReplay* rp)
{
    error_t rc = OK;
    for (uint32_t s = 0; rc == OK && s < rp->w->nstitch; s++) {
        const RibbonUnit* u = rp->w->stitch[s];
        for (uint32_t i = 0; rc == OK && i < u->nrecords; i++)
            if (u->records[i].kind == RIBBON_UNDONE)
                rc = RibbonUndoneOne(rp, u, i);
    }
    return rc;
}

error_t RibbonTranscribe(RibbonWalk* w, const char** what)
{
    RibbonReplay* rp = kmalloc(sizeof(RibbonReplay));
    error_t       rc = rp ? OK : ERR_NO_MEMORY;
    bool          read = true;
    if (rp) {
        memset(rp, 0, sizeof(*rp));
        rp->w = w;
        rc    = RibbonUndones(rp);
    }
    if (rc == OK) {
        RibbonSkipSorted(&rp->undone);
        rc   = RibbonVerify(w, &rp->undone);
        read = rc == OK || rc == ERR_NO_MEMORY;
    }
    for (uint32_t s = 0; rc == OK && s < w->nstitch; s++)
        if (!RibbonListHas(rp->refused, rp->nrefused, w->stitch[s]->seq))
            rc = RibbonStitchApplied(rp, w->stitch[s]);
    if (rc == OK && rp->use_set)
        rc = BookUseRecalled(rp->use, rp->use_len);
    if (rc == OK)
        rc = RibbonSettled(rp);
    if (rc == OK)
        rc = ErrataClosed();
    if (rp)
        RibbonReplayLetGo(rp);
    if (rc == ERR_NO_MEMORY)
        *what = "there is no memory to replay the ribbon past its Tally";
    else if (rc != OK && !read)
        *what = "its medium would not give up the data of the ribbon's newest stitch";
    else if (rc != OK)
        *what = "the memory every format shares would not take a record of the ribbon past its Tally";
    return rc;
}

error_t RibbonFacsimile(RibbonWalk* w, uint64_t tree, uint64_t lo, uint64_t hi, uint64_t upto, uint32_t* replayed,
                        bool* whole)
{
    RibbonReplay* rp = kmalloc(sizeof(RibbonReplay));
    error_t       rc = rp ? OK : ERR_NO_MEMORY;
    *replayed = 0;
    if (rp) {
        memset(rp, 0, sizeof(*rp));
        rp->w       = w;
        rp->only    = tree;
        rp->only_lo = lo;
        rp->only_hi = hi;
    }
    for (uint32_t s = 0; rc == OK && s < w->nstitch && w->stitch[s]->seq <= upto; s++)
        for (uint32_t i = 0; rc == OK && i < w->stitch[s]->nrecords; i++)
            if (w->stitch[s]->records[i].kind == RIBBON_UNDONE)
                rc = RibbonUndoneOne(rp, w->stitch[s], i);
    if (rc == OK)
        RibbonSkipSorted(&rp->undone);
    for (uint32_t s = 0; rc == OK && s < w->nstitch && w->stitch[s]->seq <= upto; s++) {
        if (RibbonListHas(rp->refused, rp->nrefused, w->stitch[s]->seq))
            continue;
        rc = RibbonStitchApplied(rp, w->stitch[s]);
        *replayed += rc == OK;
    }
    *whole = rp && rp->passed == 0;
    if (rp)
        RibbonReplayLetGo(rp);
    return rc;
}
