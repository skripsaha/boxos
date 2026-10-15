#include "docket.h"
#include "volume_digest.h"
#include "volume_ribbon.h"
#include "../settle/sewing.h"

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

void JotsLetGo(Jot* list)
{
    while (list) {
        Jot* next = list->next[0];
        FileRecordRelease(list->was);
        FileRecordRelease(list->now);
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
        return;
    }
    /* The newest state is j's; the one it replaces leaves with j. */
    FileRecord* older = p->now;
    p->kind = JotMerged(p->kind, j->kind);
    p->now  = j->now;
    j->now  = older;
    JotSpent(j, spent);
}

void DocketTakeLocked(DocketTaken* out)
{
    out->first = s_head[0];
    out->count = s_count;
    memset(s_head, 0, sizeof(s_head));
    s_count = 0;
}

void DocketBackLocked(DocketTaken* t, Jot** spent)
{
    Jot* before[JOT_LEVELS];
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
        p->kind = JotMerged(o->kind, p->kind);
        o->was  = p->was;
        p->was  = base;
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
}

// ----------------------------------------------------------------------------
// The records a stitch carries of its Jots (§2.8)
// ----------------------------------------------------------------------------

/* The bytes a varint and a tag id take at the most (a record's tag and length
 * take STITCH_RECORD_HEAD_MOST, sewing.h). */
#define JOT_VARINT_MOST       10u
#define JOT_ID_MOST           5u

/* The body of the record a change of `kind` from `was` to `now` becomes, at
 * the most: 0 for none. */
static uint64_t JotBodyMostOf(JotKind kind, const FileRecord* was, const FileRecord* now)
{
    switch (kind) {
    case JOT_BORN:
        return 7u * JOT_VARINT_MOST + now->name_len + JOT_ID_MOST * (uint64_t)now->tag_count + 2u;
    case JOT_LABELED:
        return 6u * JOT_VARINT_MOST + now->name_len + JOT_ID_MOST * ((uint64_t)now->tag_count + was->tag_count);
    case JOT_QUITCLAIM:
        return 2u * JOT_VARINT_MOST;
    default:
        return 0;
    }
}

static uint64_t JotBodyMost(const Jot* j)
{
    return JotBodyMostOf(j->kind, j->was, j->now);
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

void DocketMeasure(const DocketTaken* t, uint64_t* records, uint64_t* bytes, uint64_t* body, uint32_t* tags)
{
    *records = *bytes = *body = 0;
    *tags  = 0;
    for (const Jot* j = t->first; j; j = j->next[0]) {
        uint64_t most = JotBodyMost(j);
        if (!most)
            continue;
        uint32_t worn = (j->now ? j->now->tag_count : 0u) + (j->was ? j->was->tag_count : 0u);
        (*records)++;
        *bytes += STITCH_RECORD_HEAD_MOST + most;
        *body   = most > *body ? most : *body;
        *tags   = worn > *tags ? worn : *tags;
    }
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

/* A Born of the file as `now` holds it: no runs and no Slip — a file this
 * kernel makes on a book is born empty, and no change to its data is taken
 * (ink.c, StitchFilesRefused). */
static uint32_t JotBornPut(const Jot* j, uint8_t* body, uint32_t* scratch, uint8_t* ids)
{
    const FileRecord* f = j->now;
    uint32_t          n = JotTagsRising(f, scratch);
    RibbonBorn        b = {
        .fid = j->fid, .flags = f->flags, .size = f->size, .created_ns = f->created_time,
        .modified_ns = f->modified_time, .name = (const uint8_t*)f->name, .name_len = f->name_len,
        .ntags = n, .tags = ids, .tags_bytes = TallyIdsPut(ids, scratch, n),
    };
    return RibbonBornBodyPut(body, &b);
}

/* A Labeled of what differs between `was` and `now` — the name, the flags set
 * and cleared, the tags put on and taken off — or 0 when nothing does. */
static uint32_t JotLabeledPut(const Jot* j, uint64_t seq, uint8_t* body, uint32_t* scratch, uint8_t* ids)
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
    RibbonLabeled     l   = { .fid = j->fid, .base = seq - was->gen };
    if (was->name_len != now->name_len || memcmp(was->name, now->name, now->name_len) != 0) {
        l.what    |= LABEL_NAME;
        l.name     = (const uint8_t*)now->name;
        l.name_len = now->name_len;
    }
    l.set   = now->flags & ~was->flags;
    l.clear = was->flags & ~now->flags;
    if (l.set || l.clear)
        l.what |= LABEL_FLAGS;
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

error_t DocketLay(DocketTaken* t, uint64_t seq, uint32_t tags, uint8_t* out, uint32_t* at, uint8_t* body,
                  uint32_t* starts, uint32_t* n)
{
    uint32_t* scratch = kmalloc(sizeof(uint32_t) * 2u * ((uint64_t)tags + 1u));
    uint8_t*  ids     = kmalloc((uint64_t)JOT_ID_MOST * tags + 1u);
    error_t   rc      = scratch && ids ? OK : ERR_NO_MEMORY;
    for (Jot* j = t->first; rc == OK && j; j = j->next[0]) {
        uint32_t len = 0, kind = 0;
        j->carried = false;
        if (j->kind == JOT_BORN) {
            kind = RIBBON_BORN;
            len  = JotBornPut(j, body, scratch, ids);
        } else if (j->kind == JOT_LABELED) {
            kind = RIBBON_LABELED;
            len  = JotLabeledPut(j, seq, body, scratch, ids);
        } else if (j->kind == JOT_QUITCLAIM) {
            kind = RIBBON_QUITCLAIM;
            len  = RibbonQuitclaimBodyPut(body, j->fid, seq - j->was->gen);
        }
        if (!len)
            continue;
        j->carried    = true;
        starts[(*n)++] = *at;
        *at          += RibbonRecordPut(out + *at, kind, 0, body, len);
    }
    kfree(scratch);
    kfree(ids);
    return rc;
}
