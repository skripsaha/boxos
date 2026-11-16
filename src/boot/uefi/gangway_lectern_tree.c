/*
 * The Lectern's walk of a tree's leaves in key order — FILES, for the files
 * the system keeps in a role, the files the loader asks for by name and the
 * scenes (*.scene), and TAGS, for the ids of the tags a person's settings ask
 * a scene by — and the files it follows, kept by fid.
 *
 * A walk holds one node a level: the path from the root to the leaf in hand,
 * each node read from the first copy that holds as a node of the range its
 * parent's separators give it (§2.5, §2.9) — every node before its children,
 * the order the kernel's walk reads them (tally_walk.c) and volume-read's
 * (tally.py TreeRead), so a copy that did not hold is said in that order.
 * A child no copy of which holds is its range rotten (§4.5 п.3).
 *
 * A file is followed as the kernel's mount holds it (tally_leaves.c,
 * tally.py Tally._Files): its header, and the Parcels that cover its blocks
 * in a row from block 0; a file an element of which does not read, whose
 * Parcels leave a gap or overlap or fall short, that names ground outside the
 * data run, or that a rotten range of FILES touches is locked — no kernel.
 */

#include "gangway_lectern_book.h"

/* What a walk hands its reader: each leaf item with the seq of the node it
 * lies in (a Parcel counts its runs' births from it, §2.9), and each range
 * that does not read. 1 goes on; 0 no room ends the walk. */
typedef struct {
    int  (*leaf)(LecternBook *b, void *ctx, TallyKey key, const uint8_t *body, uint32_t len, uint64_t seq);
    int  (*rotten)(LecternBook *b, void *ctx, const TallyRange *r);
    void  *ctx;
} LecternLeaves;

/* A level of the path: its node, its range, the next item and where it is. */
typedef struct {
    LecternNode node;
    TallyRange  range;
    uint64_t    i;
    uint32_t    at;
    TallyKey    prev;
} LecternFrame;

/* The child the item just read of an inner frame names, read into the frame
 * below with the range [its key, the next item's key — or the frame's own
 * end): 1 read, 0 rotten (handed to the reader), -1 no room. */
static int LecternChild(LecternBook *b, uint64_t tree, uint64_t level, LecternFrame *f, TallyKey key,
                        const uint8_t *body, uint32_t len, const LecternLeaves *v)
{
    LecternFrame *up = &f[level], *kid = &f[level - 1u];
    TallyRange    r  = { key, up->range.hi, 1, up->range.has_hi };
    uint32_t      at = up->at, b2, l2, failed;
    TallyKey      next;
    BookPtr       ptr;
    uint64_t      count;

    if (up->i < up->node.head.items &&
        TallyItemGet(up->node.room, up->node.stream, &at, tree, key, &next, &b2, &l2)) {
        r.hi     = next;
        r.has_hi = 1;
    }
    if (TallyInnerGet(body, len, &count, &ptr) == TALLY_BODY_BROKEN) return v->rotten(b, v->ctx, &r) ? 0 : -1;
    LecternNodeSays s = LecternNodeRead(b, tree, level - 1u, &ptr, &r, &kid->node, &failed);
    if (s == NODE_NO_ROOM) return -1;
    if (s == NODE_ROTTEN) return v->rotten(b, v->ctx, &r) ? 0 : -1;
    LecternCopiesSay(b, tree, level - 1u, &ptr, failed);
    kid->range = r;
    kid->i     = 0;
    kid->at    = kid->node.items_at;
    kid->prev  = kid->node.head.first;
    return 1;
}

/* The leaves of `tree` of the slot stood on, in key order, handed to `v`: 1,
 * or 0 no room. A tree the slot names no root of has none. */
static int LecternWalk(LecternBook *b, uint64_t tree, const LecternLeaves *v)
{
    const SpineRoot *root = &b->roots[tree];
    TallyRange       all  = { { 0, 0 }, { 0, 0 }, 0, 0 };
    uint64_t         top = root->level, level = top;
    uint32_t         failed;
    LecternFrame    *f;
    int              rc = 1;

    if (!(b->rooted & (1u << tree))) return 1;
    if (top + 1u == 0 || top + 1u > ~(uint64_t)0 / sizeof(LecternFrame) ||
        !(f = (LecternFrame *)LecternTake(b, (top + 1u) * sizeof(LecternFrame))))
        return 0;
    LecternZero(f, (top + 1u) * sizeof(LecternFrame));
    LecternNodeSays s = LecternNodeRead(b, tree, top, &root->ptr, &all, &f[top].node, &failed);
    if (s != NODE_READ) {
        LecternGive(b, f, (top + 1u) * sizeof(LecternFrame));
        return s == NODE_ROTTEN && v->rotten(b, v->ctx, &all);
    }
    LecternCopiesSay(b, tree, top, &root->ptr, failed);
    f[top].range = all;
    f[top].at    = f[top].node.items_at;
    f[top].prev  = f[top].node.head.first;
    while (rc == 1) {
        LecternFrame *w = &f[level];
        TallyKey      key;
        uint32_t      body, len;
        if (w->i == w->node.head.items) {
            LecternNodeGive(b, &w->node);
            if (level++ == top) break;
            continue;
        }
        TallyItemGet(w->node.room, w->node.stream, &w->at, tree, w->prev, &key, &body, &len);
        w->prev = key;
        w->i++;
        if (level == 0) {
            rc = v->leaf(b, v->ctx, key, w->node.room + body, len, w->node.seq) ? 1 : -1;
            continue;
        }
        int kid = LecternChild(b, tree, level, f, key, w->node.room + body, len, v);
        if (kid < 0) rc = -1;
        if (kid > 0) level--;
    }
    for (uint64_t l = 0; l <= top; l++) LecternNodeGive(b, &f[l].node);
    LecternGive(b, f, (top + 1u) * sizeof(LecternFrame));
    return rc == 1;
}

/* ------------------------------------------------------------------------- */
/* The files followed, by fid                                                */
/* ------------------------------------------------------------------------- */

static uint64_t FileAt(const LecternBook *b, uint64_t fid)
{
    uint64_t lo = 0, hi = b->nfiles;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2u;
        if (b->files[mid].fid < fid) lo = mid + 1u;
        else hi = mid;
    }
    return lo;
}

LecternFile *LecternFileOf(LecternBook *b, uint64_t fid)
{
    uint64_t at = FileAt(b, fid);
    return at < b->nfiles && b->files[at].fid == fid ? &b->files[at] : (LecternFile *)0;
}

void LecternFileGive(const LecternBook *b, LecternFile *f)
{
    LecternGive(b, f->runs, f->capruns * sizeof(LecternRun));
    LecternGive(b, f->tail, LECTERN_TAIL_BYTES);
    LecternGive(b, f->tags, f->captags * sizeof(uint64_t));
    f->runs = (LecternRun *)0;
    f->tail = (uint8_t *)0;
    f->tags = (uint64_t *)0;
    f->ntags = f->captags = 0;
}

int LecternFileTags(LecternBook *b, LecternFile *f, const uint8_t *ids, uint32_t bytes, uint64_t n)
{
    uint32_t at = 0;
    uint64_t id = 0;
    f->ntags = 0;
    for (uint64_t i = 0; i < n && TallyIdNext(ids, bytes, &at, &id, i == 0); i++) {
        if (!LecternGrow(b, (void **)&f->tags, f->ntags, &f->captags, sizeof(uint64_t))) return 0;
        f->tags[f->ntags++] = id;
    }
    return 1;
}

int LecternFilePut(LecternBook *b, const LecternFile *f)
{
    uint64_t at = FileAt(b, f->fid);
    if (at < b->nfiles && b->files[at].fid == f->fid) {
        LecternFileGive(b, &b->files[at]);
        b->files[at] = *f;
        return 1;
    }
    if (!LecternGrow(b, (void **)&b->files, b->nfiles, &b->capfiles, sizeof(LecternFile))) return 0;
    for (uint64_t i = b->nfiles; i > at; i--) b->files[i] = b->files[i - 1u];
    b->files[at] = *f;
    b->nfiles++;
    return 1;
}

int LecternFileCopy(LecternBook *b, LecternFile *to, const LecternFile *from)
{
    *to         = *from;
    to->runs    = (LecternRun *)0;
    to->tail    = (uint8_t *)0;
    to->tags    = (uint64_t *)0;
    to->capruns = from->nruns ? from->nruns : 1u;
    to->captags = from->ntags;
    to->runs    = (LecternRun *)LecternTake(b, to->capruns * sizeof(LecternRun));
    if (from->tail) to->tail = (uint8_t *)LecternTake(b, LECTERN_TAIL_BYTES);
    if (from->ntags) to->tags = (uint64_t *)LecternTake(b, from->ntags * sizeof(uint64_t));
    if (!to->runs || (from->tail && !to->tail) || (from->ntags && !to->tags)) {
        LecternFileGive(b, to);
        return 0;
    }
    LecternCopy(to->runs, from->runs, from->nruns * sizeof(LecternRun));
    if (from->tail) LecternCopy(to->tail, from->tail, LECTERN_TAIL_BYTES);
    if (from->ntags) LecternCopy(to->tags, from->tags, from->ntags * sizeof(uint64_t));
    return 1;
}

/* ------------------------------------------------------------------------- */
/* FILES: the files kept in a role, those called by the names asked, scenes */
/* ------------------------------------------------------------------------- */

typedef enum { FILE_UNSEEN, FILE_FOLLOWED, FILE_PASSED } LecternFileState;

/* The file whose elements the walk is in. */
typedef struct {
    uint64_t         fid;
    LecternFileState state;
    LecternFile      f;
    uint64_t         covered;   /* the blocks its Parcels cover so far, from block 0 */
} LecternFilesWalk;

/* The walk done with the file it was in: followed when its Parcels cover its
 * blocks, all of them, and nothing locked it on the way. */
static int FileDone(LecternBook *b, LecternFilesWalk *w)
{
    int kept = 1;
    if (w->state == FILE_FOLLOWED && w->covered == (w->f.size + VOLUME_BLOCK_BYTES - 1u) / VOLUME_BLOCK_BYTES)
        kept = LecternFilePut(b, &w->f);
    else if (w->state == FILE_FOLLOWED)
        LecternFileGive(b, &w->f);
    LecternZero(&w->f, sizeof(w->f));
    w->state = FILE_UNSEEN;
    return kept;
}

/* A file's header: followed when its flags put it in a role, when it is
 * called by a name the loader asks for or as a scene is, or when the ribbon
 * does any of those — its name and its tags kept. 1, or 0 no room. */
static int FileHead(LecternBook *b, LecternFilesWalk *w, uint64_t fid, const uint8_t *body, uint32_t len)
{
    TallyFileHead h;

    w->state = FILE_PASSED;
    if (TallyFileHeadGet(body, len, &h) == TALLY_BODY_BROKEN) return 1;
    LecternZero(&w->f, sizeof(w->f));
    w->f.fid     = fid;
    w->f.gen     = h.gen;
    w->f.size    = h.size;
    w->f.flags   = h.flags;
    w->f.called  = LecternNamed(b->called, b->ncalled, h.name, h.name_len);
    if (!LecternRoleOf(w->f.flags) && !w->f.called && !LecternSceneNamed(h.name, h.name_len) &&
        !LecternListHas(b->interest, b->ninterest, fid)) {
        LecternFileGive(b, &w->f);
        return 1;
    }
    LecternFileNamed(&w->f, h.name, h.name_len);
    if (!LecternFileTags(b, &w->f, h.tags, h.tags_bytes, h.ntags)) return 0;
    w->state   = FILE_FOLLOWED;
    w->covered = 0;
    return 1;
}

/* A Parcel of the file followed, from its file block `from`, in a node of
 * `seq`: its runs taken on — or the file locked, given back. */
static int FileParcel(LecternBook *b, LecternFilesWalk *w, uint64_t from, const uint8_t *body, uint32_t len,
                      uint64_t seq)
{
    uint32_t at = 0;
    uint64_t n = 0, cursor = 0, end = b->desk->block_end, low = b->desk->frame.data_block;
    int      fits = from == w->covered && TallyTake(body, len, &at, &n);

    for (uint64_t i = 0; fits && i < n; i++) {
        TallyRun r;
        if (!TallyRunGet(body, len, &at, &cursor, seq, &r) ||
            (!r.hole && (r.start < low || r.start > end || r.count > end - r.start))) {
            fits = 0;
            break;
        }
        if (!LecternGrow(b, (void **)&w->f.runs, w->f.nruns, &w->f.capruns, sizeof(LecternRun))) return 0;
        w->f.runs[w->f.nruns++] = (LecternRun){ r.start, r.count, r.hole, RibbonRunCarried(&r) };
        w->covered += r.count;
    }
    if (!fits) {
        LecternFileGive(b, &w->f);
        w->state = FILE_PASSED;
    }
    return 1;
}

static int FileSeen(LecternBook *b, void *ctx, TallyKey key, const uint8_t *body, uint32_t len, uint64_t seq)
{
    LecternFilesWalk *w = (LecternFilesWalk *)ctx;
    if (key.k1 != w->fid || w->state == FILE_UNSEEN) {
        if (!FileDone(b, w)) return 0;
        w->fid = key.k1;
        if (key.k2 != 0) {
            w->state = FILE_PASSED;
            return 1;
        }
        return FileHead(b, w, key.k1, body, len);
    }
    if (w->state != FILE_FOLLOWED) return 1;
    return FileParcel(b, w, key.k2 - 1u, body, len, seq);
}

static int FileLost(LecternBook *b, void *ctx, const TallyRange *r)
{
    (void)ctx;
    if (!LecternGrow(b, (void **)&b->rotten, b->nrotten, &b->caprotten, sizeof(TallyRange))) return 0;
    b->rotten[b->nrotten++] = *r;
    return 1;
}

/* Whether a rotten range of FILES reaches into the keys of `fid`. */
static int FileRotten(const LecternBook *b, uint64_t fid)
{
    TallyKey lo = { fid, 0 }, hi = { fid + 1u, 0 };
    for (uint64_t i = 0; i < b->nrotten; i++) {
        const TallyRange *r = &b->rotten[i];
        if ((!r->has_lo || TallyKeyBefore(r->lo, hi)) && (!r->has_hi || TallyKeyBefore(lo, r->hi))) return 1;
    }
    return 0;
}

int LecternFiles(LecternBook *b)
{
    LecternFilesWalk w;
    LecternLeaves    v = { FileSeen, FileLost, &w };
    uint64_t         kept = 0;

    LecternZero(&w, sizeof(w));
    if (!LecternWalk(b, TALLY_FILES, &v) || !FileDone(b, &w)) {
        LecternFileGive(b, &w.f);
        return 0;
    }
    for (uint64_t i = 0; i < b->nfiles; i++) {
        if (FileRotten(b, b->files[i].fid))
            LecternFileGive(b, &b->files[i]);
        else
            b->files[kept++] = b->files[i];
    }
    b->nfiles = kept;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* TAGS: the ids of a tag a person's settings ask a scene by                 */
/* ------------------------------------------------------------------------- */

typedef struct {
    const LecternSpelling *spelling;
    uint64_t             **ids;
    uint64_t              *n, *cap;
} LecternTagsWalk;

static int TagSeen(LecternBook *b, void *ctx, TallyKey key, const uint8_t *body, uint32_t len, uint64_t seq)
{
    LecternTagsWalk *w = (LecternTagsWalk *)ctx;
    TallyTag         t;
    (void)seq;
    if (TallyTagGet(body, len, &t) == TALLY_BODY_BROKEN || !LecternSpells(w->spelling, &t)) return 1;
    return LecternListAdd(b, w->ids, w->n, w->cap, key.k1);
}

/* A range of TAGS that does not read names no tag a scene can be asked by. */
static int TagLost(LecternBook *b, void *ctx, const TallyRange *r)
{
    (void)b;
    (void)ctx;
    (void)r;
    return 1;
}

int LecternTagIds(LecternBook *b, const char *word, uint64_t **ids, uint64_t *n, uint64_t *cap)
{
    LecternSpelling s;
    LecternSpell(word, &s);
    if (!s.key_len) return 1;
    LecternTagsWalk w = { &s, ids, n, cap };
    LecternLeaves   v = { TagSeen, TagLost, &w };
    return LecternWalk(b, TALLY_TAGS, &v) && LecternCoinedIds(b, &s, ids, n, cap);
}
