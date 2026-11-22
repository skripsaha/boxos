#include "docket.h"
#include "../settle/binder.h"
#include "volume_digest.h"
#include "volume_ribbon.h"
#include "../settle/sewing.h"
#include "../integrity/gloss.h"
#include "../file_map/tail_slip.h"

/*
 * The Docket (docket.h): the Jots waiting for the next stitch, in a list by fid
 * of JOT_LEVELS levels — a Jot stands in as many as its fid's hash gives it, a
 * quarter of a level's in the next — so a file's Jot is found, and a new one
 * put, in a few steps a level with nothing asked of the heap: the levels are
 * the Jot's own, made with it before the lock. A list rather than a table: the
 * stitch lays its records by fid, and the list is in that order already.
 *
 * Everything here but JotMake, JotsLetGo and DocketSettled is under the
 * volume's state lock.
 */

static Jot*     s_head[JOT_LEVELS];
static uint64_t s_count;
static uint64_t s_takes;     /* the TAKEs the Docket has had, and the drops */

/* The levels a Jot of `fid` stands in: one, and one more for each pair of
 * low bits of its hash that are both 0 — a quarter of the Jots of a level. */
static uint32_t JotLevelsOf(uint64_t fid)
{
    uint64_t h      = WyMix(fid, 0x9E3779B97F4A7C15ull);
    uint32_t levels = 1;
    while (levels < JOT_LEVELS && (h & 3u) == 0) {
        levels++;
        h >>= 2;
    }
    return levels;
}

Jot* JotMake(JotKind kind, uint64_t fid, FileRecord* was, FileRecord* now)
{
    uint32_t levels = JotLevelsOf(fid);
    size_t   size   = sizeof(Jot) + sizeof(Jot*) * levels;
    Jot*     j      = kmalloc(size);
    if (!j) {
        FileRecordRelease(was);
        FileRecordRelease(now);
        return NULL;
    }
    memset(j, 0, size);
    j->fid    = fid;
    j->kind   = kind;
    j->levels = levels;
    j->was    = was;
    j->now    = now;
    return j;
}

/* The splices a BUILD found for the Jot let go of. */
static void JotSplicesLetGo(Jot* j)
{
    kfree(j->splices);
    kfree(j->inserted);
    j->splices  = NULL;
    j->inserted = NULL;
    j->nsplices = 0;
}

void JotsLetGo(Jot* list)
{
    while (list) {
        Jot* next = list->next[0];
        FileRecordRelease(list->was);
        FileRecordRelease(list->now);
        JotSplicesLetGo(list);
        kfree(list);
        list = next;
    }
}

/*
 * A Born and what follows it are still a Born — of the newest state — unless
 * the file is given up before a stitch named it: then nothing at all (L-8).
 * Anything else is the newer change, from what settled: two Labeleds one, a
 * change and a Quitclaim a Quitclaim. Nothing follows a Quitclaim, and no file
 * is born twice: a fid is never given again (§2.8).
 */
JotKind JotMerged(JotKind older, JotKind newer)
{
#if CONFIG_STRICT
    if (older == JOT_NONE || older == JOT_QUITCLAIM || newer == JOT_BORN || newer == JOT_NONE)
        panic("[TagFS] DEFECT: a change of kind %u merged after one of kind %u — a file given up changed, "
              "or born twice", (unsigned)newer, (unsigned)older);
#endif
    if (older == JOT_BORN)
        return newer == JOT_QUITCLAIM ? JOT_NONE : JOT_BORN;
    return newer;
}

/* The Jot of `fid`, or NULL; and in before[l] the last Jot of level l below
 * it — NULL for the head. */
static Jot* DocketFind(uint64_t fid, Jot** before)
{
    Jot* at = NULL;
    for (uint32_t l = JOT_LEVELS; l-- > 0;) {
        Jot* n = at ? at->next[l] : s_head[l];
        while (n && n->fid < fid) {
            at = n;
            n  = n->next[l];
        }
        before[l] = at;
    }
    Jot* n = at ? at->next[0] : s_head[0];
    return n && n->fid == fid ? n : NULL;
}

static void DocketInsert(Jot* j, Jot* const* before)
{
    for (uint32_t l = 0; l < j->levels; l++) {
        Jot** link = before[l] ? &before[l]->next[l] : &s_head[l];
        j->next[l] = *link;
        *link      = j;
    }
    s_count++;
}

static void JotSpent(Jot* j, Jot** spent)
{
    j->next[0] = *spent;
    *spent     = j;
}

void DocketPutLocked(Jot* j, Jot** spent)
{
    Jot* before[JOT_LEVELS];
    Jot* p = DocketFind(j->fid, before);
    if (!p) {
        DocketInsert(j, before);
        EndpaperJotIn(j);
        return;
    }
    /* The newest state is j's; the one it replaces leaves with j. */
    FileRecord* older = p->now;
    EndpaperJotOut(p);
    p->kind = JotMerged(p->kind, j->kind);
    p->now  = j->now;
    j->now  = older;
    EndpaperJotIn(p);
    JotSpent(j, spent);
}

void DocketTakeLocked(DocketTaken* out)
{
    out->first = s_head[0];
    out->count = s_count;
    memset(s_head, 0, sizeof(s_head));
    s_count = 0;
    s_takes++;
    EndpaperDocketTaken();
}

void DocketBackLocked(DocketTaken* t, Jot** spent)
{
    Jot* before[JOT_LEVELS];
    EndpaperDocketBack();
    while (t->first) {
        Jot* o = t->first;
        t->first   = o->next[0];
        o->carried = false;
        Jot* p = DocketFind(o->fid, before);
        if (!p) {
            memset(o->next, 0, sizeof(Jot*) * o->levels);
            DocketInsert(o, before);
            continue;
        }
        /* What came since is newer: o's base stays, p's state. */
        FileRecord* base = o->was;
        EndpaperJotOut(o);
        EndpaperJotOut(p);
        p->kind = JotMerged(o->kind, p->kind);
        o->was  = p->was;
        p->was  = base;
        EndpaperJotIn(p);
        JotSpent(o, spent);
    }
    t->count = 0;
}

void DocketSettled(DocketTaken* t, uint64_t seq)
{
    for (Jot* j = t->first; j; j = j->next[0])
        if (j->now)
            __atomic_store_n(&j->now->gen, j->carried ? seq : j->was ? j->was->gen : 0, __ATOMIC_RELAXED);
    JotsLetGo(t->first);
    t->first = NULL;
    t->count = 0;
}

void DocketDropAll(void)
{
    JotsLetGo(s_head[0]);
    memset(s_head, 0, sizeof(s_head));
    s_count = 0;
    s_takes++;
    EndpaperDocketDropped();
}

bool DocketBaseLacks(uint64_t fid, uint64_t k, uint64_t block)
{
    Jot*     before[JOT_LEVELS];
    Jot*     j = DocketFind(fid, before);
    FileSpot spot;
    if (!j)
        return false;
    if (!j->was)
        return j->kind == JOT_BORN;
    return !FileRecordFind(j->was, k, &spot) || spot.kind != STRIP_DATA || spot.block != block;
}

uint64_t DocketTakes(void)
{
    return s_takes;
}

bool DocketWasFrom(uint64_t fid, uint64_t* at, FileRecord** was, FileRecord** now)
{
    Jot* before[JOT_LEVELS];
    (void)DocketFind(fid, before);
    Jot* j = before[0] ? before[0]->next[0] : s_head[0];
    if (!j)
        return false;
    *at  = j->fid;
    *was = j->was;
    if (j->was)
        __atomic_add_fetch(&j->was->refs, 1, __ATOMIC_ACQ_REL);
    if (now) {
        *now = j->now;
        if (j->now)
            __atomic_add_fetch(&j->now->refs, 1, __ATOMIC_ACQ_REL);
    }
    return true;
}

error_t DocketBirths(const DocketTaken* t, uint64_t seq, uint64_t* fid_most)
{
    error_t rc = OK;
    for (const Jot* j = t->first; j && rc == OK; j = j->next[0]) {
        if (!j->carried)
            continue;
        if (j->fid > *fid_most)
            *fid_most = j->fid;
        if (j->kind == JOT_BORN && j->now) {
            StripWalk w;
            Strip     s;
            StripWalkBegin(j->now, &w);
            while (rc == OK && StripWalkNextGround(&w, &s))
                rc = s.kind == STRIP_SHARED ? BirthsSharedNoted(j->fid, &s, 1, seq) : BirthsNoted(&s, 1, seq);
        } else if (j->nsplices) {
            /* A Shaped carried, whatever the Jot's kind: a Labeled after a
             * write merges to one (JotMerged) and still lays its Shaped
             * (JotChangedPut). */
            uint64_t n = 0;
            for (uint64_t i = 0; i < j->nsplices; i++)
                n += j->splices[i].ninsert;
            rc = BirthsNoted(j->inserted, n, seq);
            if (rc == OK)
                rc = BirthsSharedNoted(j->fid, j->inserted, n, seq);
        }
    }
    return rc;
}

// ----------------------------------------------------------------------------
// The records a stitch carries of its Jots (§2.8)
// ----------------------------------------------------------------------------

/* The bytes a varint, a tag id and a run of the ribbon's form take at the most
 * (a record's tag and length take STITCH_RECORD_HEAD_MOST, sewing.h). */
#define JOT_VARINT_MOST       10u
#define JOT_ID_MOST           5u
#define JOT_RUN_MOST          18u    /* head 5, start 10, a run SHARED's list of extensions 3 (RibbonSharedList) */

/* The list of the programs `rec` shows its tags to at the most — its count
 * and each id — or 0 for a record that shows none. */
static uint64_t JotShownMost(const FileRecord* rec)
{
    return rec->shown_count ? JOT_VARINT_MOST * (1u + (uint64_t)rec->shown_count) : 0u;
}

/* The same of a Labeled from `was` to `now`: `now`'s list, and its count alone
 * when it shows none and `was` showed some. */
static uint64_t JotShownChangeMost(const FileRecord* was, const FileRecord* now)
{
    return now->shown_count ? JotShownMost(now) : was->shown_count ? JOT_VARINT_MOST : 0u;
}

/* Whether `was` and `now` keep the same first bytes. */
static bool JotHeadSame(const FileRecord* was, const FileRecord* now)
{
    return was->head_len == now->head_len && memcmp(was->head, now->head, now->head_len) == 0;
}

/* The first bytes of a record as a Born or a Labeled HEAD lays them, at the
 * most: their count and them. */
#define JOT_HEAD_MOST (1u + TAGFS_HEAD_MOST)

/* The body of a Born of `now` at the most: its fields, its runs, a digest a
 * block of them, the empty Slip, the program that made it when a program did,
 * the programs it shows its tags to, its first bytes and its passport. */
static uint64_t JotBornMost(const FileRecord* now)
{
    return 8u * JOT_VARINT_MOST + now->name_len + JOT_ID_MOST * (uint64_t)now->tag_count +
           JOT_RUN_MOST * now->strip_count + 8u * now->blocks + 2u + (now->made_by ? JOT_VARINT_MOST : 0u) +
           JotShownMost(now) + (now->head_len ? JOT_HEAD_MOST : 0u) + (now->passport ? JOT_VARINT_MOST : 0u);
}

/* The body of the record a change of `kind` from `was` to `now` becomes, at
 * the most: 0 for none. */
static uint64_t JotBodyMostOf(JotKind kind, const FileRecord* was, const FileRecord* now)
{
    switch (kind) {
    case JOT_BORN:
        return JotBornMost(now);
    case JOT_LABELED:
    case JOT_SHAPED:
        return 7u * JOT_VARINT_MOST + now->name_len + JOT_ID_MOST * ((uint64_t)now->tag_count + was->tag_count) +
               JotShownChangeMost(was, now) + (JotHeadSame(was, now) ? 0u : JOT_HEAD_MOST) +
               (was->passport != now->passport ? JOT_VARINT_MOST : 0u);
    case JOT_QUITCLAIM:
        return 2u * JOT_VARINT_MOST;
    default:
        return 0;
    }
}

/* A change's claims add up past what the records they merge into take: a Born
 * and a Labeled after it claim more than the one Born of the last state, two
 * Labeleds more than one of the last — so what the claims of the Jots a stitch
 * takes hold, the stitch's records fit. The Coineds of the tags a change
 * brings are no claim of its own: the registry counts every one given an id
 * (settle/ribbon.c, RibbonHeld). */
uint64_t DocketClaimOf(JotKind kind, const FileRecord* was, const FileRecord* now)
{
    return STITCH_RECORD_HEAD_MOST + JotBodyMostOf(kind, was, now);
}

/* A Shaped's fields — fid, base, size, mtime, its count of splices — a
 * splice's at, remove and count, and a digest a block of fresh data. */
/* A Slip of `len` bytes past the empty one every record counts: its two
 * fields at their widest and its bytes. */
static uint64_t JotSlipMost(uint64_t len)
{
    return len ? 2u * JOT_VARINT_MOST + len : 0u;
}

uint64_t DocketClaimOfShaped(uint64_t runs, uint64_t nsplice, uint64_t fresh, uint64_t slip)
{
    return STITCH_RECORD_HEAD_MOST + 5u * JOT_VARINT_MOST + 2u + nsplice * (3u * JOT_VARINT_MOST + 2u * JOT_RUN_MOST) +
           runs * JOT_RUN_MOST + 8u * fresh + JotSlipMost(slip);
}

/* Whether the file's data changed between the two: its size, its time, its
 * runs — the splices the BUILD found — or its tail (TailSlipDelta). */
static bool JotShapes(const Jot* j)
{
    return j->was && j->now && (j->nsplices || j->slip_len || j->was->size != j->now->size ||
                                j->was->modified_time != j->now->modified_time);
}

/* The Shaped's body at the most, its splices found. */
static uint64_t JotShapedMost(const Jot* j)
{
    uint64_t runs = 0, fresh = 0;
    for (uint64_t i = 0; i < j->nsplices; i++) {
        runs += j->splices[i].ninsert;
        for (uint64_t k = 0; k < j->splices[i].ninsert; k++)
            fresh += j->splices[i].insert[k].kind == STRIP_DATA ? j->splices[i].insert[k].count : 0u;
    }
    return DocketClaimOfShaped(runs, j->nsplices, fresh, 0) - 2u * JOT_RUN_MOST * j->nsplices -
           STITCH_RECORD_HEAD_MOST;
}

/* The records `j` becomes at the most into the measure: how many, the bytes,
 * the longest body. */
static void JotMeasured(const Jot* j, uint64_t* records, uint64_t* bytes, uint64_t* body)
{
    uint64_t most[2] = { JotBodyMostOf(j->kind, j->was, j->now), 0 };
    if (j->kind == JOT_BORN)
        most[0] += JotSlipMost(j->slip_len);
    if ((j->kind == JOT_LABELED || j->kind == JOT_SHAPED) && JotShapes(j))
        most[1] = JotShapedMost(j) + JotSlipMost(j->slip_len);
    else if (j->kind == JOT_BORN && (j->now->flags & TAGFS_FILE_DELETED))
        most[1] = JotBodyMostOf(JOT_LABELED, j->now, j->now);
    for (uint32_t k = 0; k < 2u; k++) {
        if (!most[k])
            continue;
        (*records)++;
        *bytes += STITCH_RECORD_HEAD_MOST + most[k];
        *body   = most[k] > *body ? most[k] : *body;
    }
}

/* The runs of fresh data the records of `j` will name, and their blocks:
 * every DATA Strip of a Born, every DATA Strip a Shaped's splices put in —
 * the ones DocketLay lays with a digest each (JotRunPut); a run SHARED names
 * ground written before it, and is none of them (§2.7). */
static void JotFresh(const Jot* j, uint64_t* runs, uint64_t* fresh)
{
    Strip     s;
    StripWalk w;
    if (j->kind == JOT_BORN) {
        StripWalkBegin(j->now, &w);
        while (StripWalkNext(&w, &s))
            if (s.kind == STRIP_DATA) {
                (*runs)++;
                *fresh += s.count;
            }
        return;
    }
    if ((j->kind != JOT_LABELED && j->kind != JOT_SHAPED) || !JotShapes(j))
        return;
    for (uint64_t i = 0; i < j->nsplices; i++)
        for (uint64_t k = 0; k < j->splices[i].ninsert; k++)
            if (j->splices[i].insert[k].kind == STRIP_DATA) {
                (*runs)++;
                *fresh += j->splices[i].insert[k].count;
            }
}

error_t DocketMeasure(DocketTaken* t, uint64_t* records, uint64_t* bytes, uint64_t* body, uint32_t* tags,
                      uint64_t* runs, uint64_t* fresh)
{
    *records = *bytes = *body = *runs = *fresh = 0;
    *tags = 0;
    for (Jot* j = t->first; j; j = j->next[0]) {
        JotSplicesLetGo(j);
        if ((j->kind == JOT_LABELED || j->kind == JOT_SHAPED) && j->was && j->now) {
            error_t rc = FileRecordSplices(j->was, j->now, &j->splices, &j->nsplices, &j->inserted);
            if (rc != OK)
                return rc;
        }
        j->slip_len = j->kind == JOT_BORN || (j->was && j->now)
                          ? TailSlipDelta(j->was, j->now, j->nsplices > 0, &j->slip_off, NULL)
                          : 0u;
        uint32_t worn = (j->now ? j->now->tag_count : 0u) + (j->was ? j->was->tag_count : 0u);
        JotMeasured(j, records, bytes, body);
        JotFresh(j, runs, fresh);
        *tags = worn > *tags ? worn : *tags;
    }
    return OK;
}

static int JotIdOrder(const void* a, const void* b, void* ctx)
{
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    (void)ctx;
    return x < y ? -1 : x > y;
}

/* The tags `rec` wears, rising, into `out`: how many. */
static uint32_t JotTagsRising(const FileRecord* rec, uint32_t* out)
{
    memcpy(out, rec->tags, sizeof(uint32_t) * rec->tag_count);
    KSort(out, rec->tag_count, sizeof(uint32_t), JotIdOrder, NULL);
    return rec->tag_count;
}

/* The tags of `a` that `b` does not wear — both rising — into `out`: how many. */
static uint32_t JotTagsLess(const uint32_t* a, uint32_t na, const uint32_t* b, uint32_t nb, uint32_t* out)
{
    uint32_t n = 0, k = 0;
    for (uint32_t i = 0; i < na; i++) {
        while (k < nb && b[k] < a[i])
            k++;
        if (k == nb || b[k] != a[i])
            out[n++] = a[i];
    }
    return n;
}

/* What a record lays apart from its body before the body is put together: the
 * runs it carries, of the ribbon's form, and their digests. */
typedef struct {
    uint8_t* runs;
    uint32_t runs_bytes;
    uint8_t* digests;
    uint64_t ndigests;
    uint64_t cursor;     /* the end of the last DATA run laid, as the ribbon's form counts from it */
    uint8_t* tail;       /* a block's room for the bytes of its Slip */
    uint8_t* shown;      /* room for the list of the programs a record shows its tags to */
} JotApart;

/* The Slip a record of `j` lays: the span of the last block changed since
 * `was` (TailSlipDelta, measured by DocketMeasure), its bytes in a->tail. */
static RibbonSlip JotSlipPut(const Jot* j, JotApart* a)
{
    uint32_t off = 0;
    uint32_t len = j->slip_len ? TailSlipDelta(j->was, j->now, j->nsplices > 0, &off, a->tail) : 0u;
    return (RibbonSlip){ .off = off, .len = len, .bytes = a->tail };
}

/* `s` laid as a run of the ribbon's form, and the digests the Gloss holds for
 * its blocks after the ones laid — a block the Gloss does not name is a
 * defect of the writer, said, and laid as no digest a block of data has. A
 * run SHARED is laid with its extension and no digest: its blocks are held to
 * the one digest their ground carries (§2.8). */
static void JotRunPut(JotApart* a, const Strip* s, uint64_t fid)
{
    TallyRun r = { .start = s->start, .count = s->count, .birth = 0, .hole = s->kind == STRIP_HOLE };
    if (s->kind == STRIP_SHARED)
        r.ext = RibbonSharedList(&r.ext_len);
    a->runs_bytes += RibbonRunPut(a->runs + a->runs_bytes, &r, &a->cursor);
    if (r.hole || s->kind == STRIP_SHARED)
        return;
    if (!GlossCopy(s->start, s->count, a->digests + a->ndigests * 8u)) {
#if CONFIG_STRICT
        panic("[TagFS] DEFECT: file %llu puts in blocks %llu..%llu whose digests the Gloss does not hold",
              (unsigned long long)fid, (unsigned long long)s->start, (unsigned long long)(s->start + s->count - 1u));
#endif
        kprintf("[TagFS] DEFECT: file %llu puts in blocks %llu..%llu whose digests the Gloss does not hold — "
                "the stitch names them with none, and a read of them is refused ERR_CHECKSUM\n",
                (unsigned long long)fid, (unsigned long long)s->start, (unsigned long long)(s->start + s->count - 1u));
        memset(a->digests + a->ndigests * 8u, 0, (size_t)s->count * 8u);
    }
    a->ndigests += s->count;
}

/* A Born of the file as `now` holds it: its runs, the digest of every block of
 * them, and its tail's Slip — of a file born with its bytes in a hole, all of
 * them (§2.8). */
static uint32_t JotBornPut(const Jot* j, uint8_t* body, uint32_t* scratch, uint8_t* ids, JotApart* a)
{
    const FileRecord* f = j->now;
    uint32_t          n = JotTagsRising(f, scratch);
    StripWalk         w;
    Strip             s;
    StripWalkBegin(f, &w);
    while (StripWalkNext(&w, &s))
        JotRunPut(a, &s, j->fid);
    RibbonBorn b = {
        .fid = j->fid, .flags = f->flags & ~(uint32_t)TAGFS_FILE_DELETED, .size = f->size, .created_ns = f->created_time,
        .modified_ns = f->modified_time, .name = (const uint8_t*)f->name, .name_len = f->name_len,
        .ntags = n, .tags = ids, .tags_bytes = TallyIdsPut(ids, scratch, n), .nruns = f->strip_count,
        .runs = a->runs, .runs_bytes = a->runs_bytes, .ndigests = a->ndigests, .digests = a->digests,
        .slip = JotSlipPut(j, a), .made_by = f->made_by, .nshown = f->shown_count, .shown = a->shown,
        .shown_bytes = TallyFidsPut(a->shown, f->shown_to, f->shown_count), .head_len = f->head_len,
        .head = f->head, .passport = f->passport,
    };
    return RibbonBornBodyPut(body, &b);
}

/* A Shaped of how the file's data changed between `was` and `now`: its
 * splices, each `at` from the end of the one before in the file as it stands
 * after it (§2.8), the digests of the blocks they put in, and the Slip of
 * its tail's change — the cycle's, never the whole tail (§16.5 п.13). */
static uint32_t JotShapedPut(const Jot* j, uint64_t seq, uint8_t* body, JotApart* a)
{
    uint64_t end = 0;
    for (uint64_t i = 0; i < j->nsplices; i++) {
        const BookSplice* sp = &j->splices[i];
        a->runs_bytes += BookVarintPut(a->runs + a->runs_bytes, sp->at - end);
        a->runs_bytes += BookVarintPut(a->runs + a->runs_bytes, sp->remove);
        a->runs_bytes += BookVarintPut(a->runs + a->runs_bytes, sp->ninsert);
        end = sp->at;
        for (uint64_t k = 0; k < sp->ninsert; k++) {
            JotRunPut(a, &sp->insert[k], j->fid);
            end += sp->insert[k].count;
        }
    }
    RibbonShaped sh = { .fid = j->fid, .base = seq - j->was->gen, .size = j->now->size,
                        .mtime_ns = j->now->modified_time, .nsplice = j->nsplices, .splices = a->runs,
                        .splices_bytes = a->runs_bytes, .ndigests = a->ndigests, .digests = a->digests,
                        .slip = JotSlipPut(j, a) };
    return RibbonShapedBodyPut(body, &sh);
}

/* Whether `was` and `now` show their tags to the same programs. */
static bool JotShownSame(const FileRecord* was, const FileRecord* now)
{
    return was->shown_count == now->shown_count &&
           (!now->shown_count || memcmp(was->shown_to, now->shown_to, sizeof(uint64_t) * now->shown_count) == 0);
}

/* A Labeled of what differs between `was` and `now` — the name, the flags set
 * and cleared (never the deleted bit: the term says that, KEEP; nor the shown
 * one: the list says that, SHOWN; nor the head one: the bytes say that,
 * HEAD; nor the passport's, PASSPORT), the tags put on and taken off, the time
 * the file is kept deleted until, the programs it shows its tags to, its first
 * bytes, its passport — over the file at `base`, or 0 when nothing does. `shown` is room for that list. */
static uint32_t JotLabeledPut(const Jot* j, uint64_t base, uint8_t* body, uint32_t* scratch, uint8_t* ids,
                              uint8_t* shown)
{
    const FileRecord* was = j->was;
    const FileRecord* now = j->now;
    uint32_t*         w   = scratch;
    uint32_t*         n   = w + was->tag_count;
    uint32_t*         add = n + now->tag_count;
    uint32_t          nw  = JotTagsRising(was, w), nn = JotTagsRising(now, n);
    uint32_t          na  = JotTagsLess(n, nn, w, nw, add);
    uint32_t*         drop = add + na;
    uint32_t          nd  = JotTagsLess(w, nw, n, nn, drop);
    RibbonLabeled     l   = { .fid = j->fid, .base = base };
    if (was->name_len != now->name_len || memcmp(was->name, now->name, now->name_len) != 0) {
        l.what    |= LABEL_NAME;
        l.name     = (const uint8_t*)now->name;
        l.name_len = now->name_len;
    }
    const uint32_t own = TAGFS_FILE_DELETED | TAGFS_FILE_SHOWN | TAGFS_FILE_HEAD | TAGFS_FILE_PASSPORT;   /* the verbs' bits */
    l.set   = now->flags & ~was->flags & ~own;
    l.clear = was->flags & ~now->flags & ~own;
    if (l.set || l.clear)
        l.what |= LABEL_FLAGS;
    if (was->keep_until != now->keep_until) {
        l.what   |= LABEL_KEEP;
        l.keep_ns = now->keep_until;
    }
    if (!JotShownSame(was, now)) {
        l.what       |= LABEL_SHOWN;
        l.nshown      = now->shown_count;
        l.shown       = shown;
        l.shown_bytes = TallyFidsPut(shown, now->shown_to, now->shown_count);
    }
    if (!JotHeadSame(was, now)) {
        l.what    |= LABEL_HEAD;
        l.head_len = now->head_len;
        l.head     = now->head;
    }
    if (was->passport != now->passport) {
        l.what    |= LABEL_PASSPORT;
        l.passport = now->passport;
    }
    if (na || nd) {
        l.what      |= LABEL_TAGS;
        l.nadd       = na;
        l.add        = ids;
        l.add_bytes  = TallyIdsPut(ids, add, na);
        l.ndrop      = nd;
        l.drop       = ids + l.add_bytes;
        l.drop_bytes = TallyIdsPut(ids + l.add_bytes, drop, nd);
    }
    return l.what ? RibbonLabeledBodyPut(body, &l) : 0u;
}

/* One record into the stream at *at, its offset into starts[(*n)++]. */
static void JotRecordPut(uint8_t* out, uint32_t* at, uint32_t* starts, uint32_t* n, uint32_t kind,
                         const uint8_t* body, uint32_t len)
{
    starts[(*n)++] = *at;
    *at           += RibbonRecordPut(out + *at, kind, 0, body, len);
}

/* The records of a changed file: a Shaped of its data, then a Labeled of its
 * fields over the gen the Shaped gave it — the stitch's own seq (§3.7). */
static void JotChangedPut(Jot* j, uint64_t seq, uint8_t* out, uint32_t* at, uint8_t* body, uint32_t* scratch,
                          uint8_t* ids, JotApart* a, uint32_t* starts, uint32_t* n)
{
    uint64_t base = seq - j->was->gen;
    if (JotShapes(j)) {
        JotRecordPut(out, at, starts, n, RIBBON_SHAPED, body, JotShapedPut(j, seq, body, a));
        j->carried = true;
        base       = 0;
    }
    uint32_t len = JotLabeledPut(j, base, body, scratch, ids, a->shown);
    if (len) {
        JotRecordPut(out, at, starts, n, RIBBON_LABELED, body, len);
        j->carried = true;
    }
}

error_t DocketLay(DocketTaken* t, uint64_t seq, uint32_t tags, uint8_t* out, uint32_t* at, uint8_t* body,
                  uint32_t* starts, uint32_t* n, uint64_t* fresh)
{
    uint64_t  room    = 0;
    for (const Jot* j = t->first; j; j = j->next[0])
        room = JotBodyMostOf(j->kind, j->was, j->now) > room ? JotBodyMostOf(j->kind, j->was, j->now) : room;
    for (const Jot* j = t->first; j; j = j->next[0])
        room = j->nsplices && JotShapedMost(j) > room ? JotShapedMost(j) : room;
    uint32_t* scratch = kmalloc(sizeof(uint32_t) * 2u * ((uint64_t)tags + 1u));
    uint8_t*  ids     = kmalloc((uint64_t)JOT_ID_MOST * tags + 1u);
    /* A record's list of the programs it shows its tags to is counted in its
     * body at the most (JotShownMost, JotShownChangeMost): room for the body
     * is room for it. */
    JotApart  a       = { .runs = kmalloc(room + 1u), .digests = kmalloc(room + 1u), .tail = kmalloc(TAGFS_BLOCK_SIZE),
                          .shown = kmalloc(room + 1u) };
    error_t   rc      = scratch && ids && a.runs && a.digests && a.tail && a.shown ? OK : ERR_NO_MEMORY;
    for (Jot* j = t->first; rc == OK && j; j = j->next[0]) {
        a.runs_bytes = 0;
        a.ndigests   = 0;
        a.cursor     = 0;
        j->carried   = false;
        if (j->kind == JOT_BORN) {
            JotRecordPut(out, at, starts, n, RIBBON_BORN, body, JotBornPut(j, body, scratch, ids, &a));
            j->carried = true;
            if (j->now->flags & TAGFS_FILE_DELETED) {
                RibbonLabeled kept = { .fid = j->fid, .what = LABEL_KEEP, .keep_ns = j->now->keep_until };
                JotRecordPut(out, at, starts, n, RIBBON_LABELED, body, RibbonLabeledBodyPut(body, &kept));
            }
        } else if (j->kind == JOT_LABELED || j->kind == JOT_SHAPED) {
            JotChangedPut(j, seq, out, at, body, scratch, ids, &a, starts, n);
        } else if (j->kind == JOT_QUITCLAIM) {
            JotRecordPut(out, at, starts, n, RIBBON_QUITCLAIM, body,
                         RibbonQuitclaimBodyPut(body, j->fid, seq - j->was->gen));
            j->carried = true;
        }
        *fresh += a.ndigests;
    }
    kfree(scratch);
    kfree(ids);
    kfree(a.runs);
    kfree(a.digests);
    kfree(a.tail);
    kfree(a.shown);
    return rc;
}
