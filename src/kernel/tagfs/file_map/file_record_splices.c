#include "tract.h"

/*
 * The splices that make one record of a file into another (file_record.h,
 * FileRecordSplices) — the one change the next stitch says was made of the
 * file between the record the last stitch left it as and the one memory holds
 * now (ink/docket.c, a Shaped of a Jot): FileRecordSplice applied to `was`
 * with them gives `now`, Strip for Strip, so the record the mount's replay
 * builds is the one memory held.
 *
 * ‼ A PIECE OF AN OLD RUN IS THE OLD RUN'S, NOT PUT IN AGAIN (§2.8, §3.1). A
 * block written over in the middle of a run leaves `now` the run's head, the
 * new block and the run's tail — the head and the tail are the old run's
 * blocks, cut where they were cut. Said Strip for Strip, the change would take
 * the whole run out and put all three in: the stitch would carry digests for
 * blocks nobody wrote (the Gloss holds none for a block only a Tally names),
 * count them fresh in its Clasp, flush for them, and give them a new birth;
 * and a cut inside a run would be a run put in again rather than its tail
 * taken out. So a Strip of `now` that lies inside the Strip of `was` holding
 * its place — the same kind and birth, on the same ground — is kept, and only
 * what is not is put in. The replay cuts an old run only where a splice
 * begins or ends, so two kept pieces of one old run that meet in `now` — a cut
 * nothing between them made — could not come back as two; the second is put
 * in, and the replay still builds `now`. No change here makes such a pair:
 * a write puts its own block between the pieces it leaves (plot_window.c).
 *
 * ‼ WHAT THE TWO SHARE IS PASSED OVER, NOT WALKED (§2.9, H-5). A change built
 * `now` from `was` by copying the Parcels it touched and sharing every other
 * (FileRecordSplice); two walks that stand at the head of the same Parcel at
 * the same block of the file step over it whole, so the difference costs the
 * Parcels that differ and one step a Parcel shared, never a step a run.
 */

typedef struct {
    StripWalk w;
    Strip     s;
    bool      has;
    uint64_t  pos;     /* the block the Strip `s` begins at */
} SpliceSide;

static void SpliceSideBegin(const FileRecord* rec, SpliceSide* a)
{
    a->pos = 0;
    StripWalkBegin(rec, &a->w);
    a->has = StripWalkNext(&a->w, &a->s);
}

static void SpliceSideNext(SpliceSide* a)
{
    a->pos += a->s.count;
    a->has  = StripWalkNext(&a->w, &a->s);
}

/* The old side moved on to the Strip that holds block `k`, or past its last. */
static void SpliceSideTo(SpliceSide* o, uint64_t k)
{
    while (o->has && o->pos + o->s.count <= k)
        SpliceSideNext(o);
}

/* Whether the new side's Strip is a piece of the old Strip that holds its
 * first block: of its kind and birth, on its ground, and ending inside it. */
static bool SpliceKept(const SpliceSide* o, const SpliceSide* n)
{
    if (!o->has || o->pos > n->pos)
        return false;
    uint64_t off = n->pos - o->pos;
    return off + n->s.count <= o->s.count && n->s.kind == o->s.kind && n->s.birth == o->s.birth &&
           (n->s.kind != STRIP_DATA || n->s.start == o->s.start + off);
}

/* Both walks at the first Strip of one Parcel at one block: the Parcel passed
 * over in both. */
static bool SpliceSidesShare(SpliceSide* o, SpliceSide* n)
{
    if (!o->has || !n->has || o->pos != n->pos || o->w.parcel != n->w.parcel || o->w.strip != 1u ||
        n->w.strip != 1u)
        return false;
    const Parcel* p = o->w.parcel;
    o->pos    += p->blocks - o->s.count;
    n->pos    += p->blocks - n->s.count;
    o->w.strip = p->count;
    n->w.strip = p->count;
    SpliceSideNext(o);
    SpliceSideNext(n);
    return true;
}

/* A list that grows: the splices and the Strips they put in. */
typedef struct {
    BookSplice* sp;
    uint64_t    nsp, capsp;
    TractGround ins;
} SpliceList;

static error_t SpliceListPut(SpliceList* l, uint64_t at, uint64_t remove, uint64_t first)
{
    if (l->nsp == l->capsp) {
        uint64_t    cap  = l->capsp ? l->capsp * 2u : 8u;
        BookSplice* more = kmalloc(sizeof(BookSplice) * cap);
        if (!more)
            return ERR_NO_MEMORY;
        if (l->nsp)
            memcpy(more, l->sp, sizeof(BookSplice) * l->nsp);
        kfree(l->sp);
        l->sp    = more;
        l->capsp = cap;
    }
    /* `insert` holds the offset into the list's Strips until the list stops
     * growing: the Strips' array moves as it grows. */
    l->sp[l->nsp++] = (BookSplice){ .at = at, .remove = remove, .ninsert = l->ins.count - first,
                                    .insert = (const Strip*)(uintptr_t)first };
    return OK;
}

/* One splice, from the new Strip that is not kept to the next one that is — or
 * the end of `now`, the splice then taking out every old block past where it
 * begins. *ended when it reached the end. */
static error_t SpliceOne(SpliceSide* o, SpliceSide* n, uint64_t was_blocks, SpliceList* l, bool* ended)
{
    uint64_t at = n->pos, first = l->ins.count;
    do {
        error_t rc = TractGroundAdd(&l->ins, &n->s);
        if (rc != OK)
            return rc;
        SpliceSideNext(n);
        SpliceSideTo(o, n->pos);
    } while (n->has && !SpliceKept(o, n));
    *ended = !n->has;
    uint64_t to = n->has ? n->pos : was_blocks;
    return SpliceListPut(l, at, to > at ? to - at : 0, first);
}

error_t FileRecordSplices(const FileRecord* was, const FileRecord* now, BookSplice** out, uint64_t* count,
                          Strip** strips)
{
    SpliceSide o, n;
    SpliceList l     = { 0 };
    error_t    rc    = OK;
    bool       kept  = false, ended = false;
    SpliceSideBegin(was, &o);
    SpliceSideBegin(now, &n);
    while (rc == OK && n.has) {
        SpliceSideTo(&o, n.pos);
        if (SpliceSidesShare(&o, &n)) {
            kept = true;
            continue;
        }
        /* A piece that begins inside the old Strip the kept piece before it
         * lies in would come back joined to it: it is put in. */
        if (SpliceKept(&o, &n) && !(kept && o.pos < n.pos)) {
            kept = true;
            SpliceSideNext(&n);
            continue;
        }
        rc   = SpliceOne(&o, &n, was->blocks, &l, &ended);
        kept = false;
    }
    if (rc == OK && !ended && was->blocks > now->blocks)
        rc = SpliceListPut(&l, now->blocks, was->blocks - now->blocks, l.ins.count);
    if (rc != OK) {
        kfree(l.sp);
        kfree(l.ins.strip);
        return rc;
    }
    for (uint64_t i = 0; i < l.nsp; i++)
        l.sp[i].insert = l.sp[i].ninsert ? l.ins.strip + (uintptr_t)l.sp[i].insert : NULL;
    *out    = l.sp;
    *count  = l.nsp;
    *strips = l.ins.strip;
    return OK;
}
