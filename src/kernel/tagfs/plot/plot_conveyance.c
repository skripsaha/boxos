#include "plot_internal.h"

/*
 * What a Plot conveys (plot.h, PlotConveyance): the difference between the
 * runs of the record it was loaded from and the runs it leaves, as splices of
 * that record — the one change an Ink makes of a file's runs, and the one a
 * Shaped record of the book says was made (spec_v2 §2.8, §3.7).
 *
 * ‼ THE DIFFERENCE IS TAKEN, NOT KEPT (spec_v2 §16.2 p.13). A Plot works on a
 * flat copy of the runs and changes it in place — a trim, an append, a gather,
 * a run put where another was — and never knew where its changes lie in the
 * record. The Ink finds them once it holds the record the Plot was loaded
 * from: one walk of both in the file's order, O(runs) — the work the copy of
 * every run a v1 Ink packs costs already.
 */

static bool StripSame(const Strip *a, const Strip *b)
{
    return a->start == b->start && a->count == b->count && a->kind == b->kind &&
           a->birth == b->birth;
}

/*
 * The splices that make the record's Strips into `runs`, into `out` (NULL:
 * only counted): how many. Both are walked in the file's order, block by
 * block: where both lists are at the same block and their Strips are the
 * same Strip, nothing changed; anywhere else a splice begins, and it goes on,
 * the list behind moving first, until both meet at one block again with the
 * same Strip next — or both end. A Strip is the same only whole and in the
 * same place: two Strips that meet are not one, as the record keeps them
 * (file_record.h). A file's blocks never move along it, so every splice but a
 * last one puts in as many blocks as it takes out, and its `at` is the same
 * in the file before and after.
 */
static uint64_t ConveyanceSplices(const FileRecord *cur, const Strip *runs, uint32_t n,
                                  BookSplice *out)
{
    StripWalk w;
    Strip     o;
    uint64_t  po = 0, pn = 0, count = 0;
    uint32_t  j = 0;
    StripWalkBegin(cur, &w);
    bool has = StripWalkNext(&w, &o);

    while (has || j < n) {
        if (has && j < n && po == pn && StripSame(&o, &runs[j])) {
            po += o.count;
            pn += runs[j].count;
            j++;
            has = StripWalkNext(&w, &o);
            continue;
        }
        uint64_t at = pn, from = po;
        uint32_t first = j;
        for (;;) {
            bool old_behind = has && (j >= n || po <= pn);
            bool new_behind = j < n && (!has || pn <= po);
            if (old_behind) {
                po += o.count;
                has = StripWalkNext(&w, &o);
            }
            if (new_behind)
                pn += runs[j++].count;
            if ((!has && j >= n) || (po == pn && has && j < n && StripSame(&o, &runs[j])))
                break;
        }
        if (out)
            out[count] = (BookSplice){ .at = at, .remove = po - from, .ninsert = j - first,
                                       .insert = j > first ? &runs[first] : NULL };
        count++;
    }
    return count;
}

error_t PlotConveyance(const Plot *p, const FileRecord *cur, BookSplice **out, uint64_t *count)
{
    *out   = NULL;
    *count = ConveyanceSplices(cur, p->runs, p->run_count, NULL);
    if (*count == 0)
        return OK;
    *out = kmalloc(sizeof(BookSplice) * *count);
    if (!*out) {
        *count = 0;
        return ERR_NO_MEMORY;
    }
    ConveyanceSplices(cur, p->runs, p->run_count, *out);
    return OK;
}

#if CONFIG_STRICT || defined(CONFIG_VOLUME_TESTS)

/*
 * ‼ FORMAT V1 IS WHAT IT WAS (spec_v2 §16.1 p.1). Memory's record is built by
 * new code — a header and a Tract of Parcels, spliced, relabelled or built
 * whole — while the medium's is packed as format v1 always packed it, so a
 * STRICT or VOLUME_TESTS kernel holds the two to each other after every Ink
 * that lays a record of format v1: the record v1 packed, read back, says the
 * header memory holds — id, flags, size, times, name, tags — and lays every
 * block where memory's Strips lay it. After a Plot's Ink, memory's Strips are
 * the Plot's own, and the ground its splices free is the ground the Plot let
 * go. A disagreement is said as a DEFECT, and stopped at under STRICT.
 */

/* Whether two lists of Strips lay the file's blocks alike, block for block —
 * each block of the same kind and, on ground, on the same block — however
 * each cuts its Strips. */
static bool StripsLayAlike(const Strip *a, uint64_t na, const Strip *b, uint64_t nb)
{
    uint64_t i = 0, j = 0, ia = 0, jb = 0;   /* the Strips, and how far into each */
    while (i < na && j < nb) {
        uint64_t len = a[i].count - ia < b[j].count - jb ? a[i].count - ia : b[j].count - jb;
        if (a[i].kind != b[j].kind ||
            (a[i].kind == STRIP_DATA && a[i].start + ia != b[j].start + jb))
            return false;
        ia += len;
        jb += len;
        if (ia == a[i].count) {
            i++;
            ia = 0;
        }
        if (jb == b[j].count) {
            j++;
            jb = 0;
        }
    }
    return i == na && j == nb;
}

/* A copy of `n` DATA Strips as their ground (StripsAsGround): NULL for no
 * memory, with *out_n untouched. */
static Strip *GroundOf(const Strip *s, uint64_t n, uint64_t *out_n)
{
    Strip *g = kmalloc(sizeof(Strip) * (n ? n : 1u));
    if (!g)
        return NULL;
    if (n)
        memcpy(g, s, sizeof(Strip) * n);
    *out_n = StripsAsGround(g, n);
    return g;
}

/* Said, and in a STRICT kernel stopped at: a record memory holds that is not
 * the one it must be — `how`, and `what` of it. */
static bool MirrorDisagrees(uint64_t fid, const char *how, const char *what)
{
#if CONFIG_STRICT
    panic("[TagFS] DEFECT: the record memory holds of file %llu %s — %s", fid, how, what);
#endif
    kprintf("[TagFS] DEFECT: the record memory holds of file %llu %s — %s\n", fid, how, what);
    return false;
}

/* What of memory's record `rec` the record format v1 packed (`v1`, read back)
 * does not say, `flat` its Strips laid flat; NULL when every field agrees. */
static const char *PackV1Differs(const FileRecord *rec, const TagFSMetadata *v1, const Strip *flat,
                                 uint32_t n)
{
    if (v1->file_id != rec->file_id)
        return "its file id";
    if (v1->flags != rec->flags)
        return "its flags";
    if (v1->size != rec->size)
        return "its size";
    if (v1->created_time != rec->created_time || v1->modified_time != rec->modified_time)
        return "its times";
    if (strcmp(v1->filename, rec->name) != 0)
        return "its name";
    if (v1->tag_count != rec->tag_count ||
        (rec->tag_count && memcmp(v1->tag_ids, rec->tags, sizeof(uint32_t) * rec->tag_count) != 0))
        return "its tags";
    if (!StripsLayAlike(flat, n, v1->extents, v1->extent_count))
        return "its runs, which lay its blocks elsewhere";
    return NULL;
}

bool PackV1Agrees(const FileRecord *rec, const uint8_t *packed, uint32_t bytes)
{
    Strip        *flat = NULL;
    uint32_t      n    = 0;
    const char   *what = NULL;
    TagFSMetadata v1;

    memset(&v1, 0, sizeof(v1));
    error_t unpacked = UnpackV1(packed, bytes, TagFSLayout()->data_blocks, &v1, NULL);
    if (unpacked != OK && unpacked != ERR_NO_MEMORY)
        what = "a record format v1 packed that does not read back";
    else if (unpacked != OK || FileRecordFlatten(rec, &flat, &n) != OK)
        kprintf("[TagFS] file %llu: the record memory holds was not held to the one format v1 "
                "packed — there was no memory for the check\n", rec->file_id);
    else
        what = PackV1Differs(rec, &v1, flat, n);
    kfree(flat);
    tagfs_metadata_free(&v1);
    return what ? MirrorDisagrees(rec->file_id, "is not the one format v1 packed", what) : true;
}

bool PlotConveyanceAgrees(const Plot *p, const FileRecord *cur, const BookSplice *sp, uint64_t nsp,
                          const FileRecord *rec, const uint8_t *packed, uint32_t bytes)
{
    Strip      *flat  = NULL, *freed = NULL, *plots = NULL;
    FileRecord *again = NULL;
    uint32_t    n     = 0;
    uint64_t    nfreed = 0, nplots = 0;
    const char *what  = NULL;

    /* The ground freed is asked of the same splices applied once more: the
     * Ink's own did not ask it, so the check costs a STRICT kernel what it
     * costs and the Ink nothing. */
    bool held = FileRecordFlatten(rec, &flat, &n) == OK &&
                FileRecordSplice(cur, sp, nsp, rec->size, rec->modified_time, &again, &freed,
                                 &nfreed) == OK &&
                (plots = GroundOf(p->freed, p->freed_count, &nplots)) != NULL;
    if (!held)
        kprintf("[TagFS] file %llu: the record memory holds was not held to its Plot — there was "
                "no memory for the check\n", p->fid);
    else if (n != p->run_count || (n && memcmp(flat, p->runs, sizeof(Strip) * n) != 0))
        what = "its Strips are not the Plot's runs";
    else if ((nfreed = StripsAsGround(freed, nfreed)) != nplots ||
             (nplots && memcmp(freed, plots, sizeof(Strip) * nplots) != 0))
        what = "the ground it frees is not the ground the Plot let go";
    kfree(flat);
    kfree(freed);
    kfree(plots);
    FileRecordRelease(again);
    if (what)
        return MirrorDisagrees(p->fid, "does not say what its Plot did", what);
    return PackV1Agrees(rec, packed, bytes);
}

#endif /* CONFIG_STRICT || CONFIG_VOLUME_TESTS */
