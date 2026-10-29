#include "tract.h"

/*
 * The Tract and its Parcels (tract.h): built, spliced, found and let go.
 *
 * Every walk of the tree is a loop: a path down it is held in a TractPath —
 * a node, a kid and a first block for each level, TRACT_HEIGHT_MOST of them
 * at most (file_record.h says why that is enough) — in the heap or a frame's
 * fixed few hundred bytes, never in frames of a recursion.
 *
 * A splice is a B+-tree's range replace with its path copied: the Parcels it
 * touches are gathered, cut and built anew; then, level by level up to the
 * root, the nodes over the range are gathered with the new ones in place of
 * the old, cut evenly into nodes again — joined first with a neighbour when
 * fewer than TRACT_KIDS_LEAST are left — and built anew. Everything else is
 * shared with the old Tract, which is never touched.
 */

typedef struct {
    const Tract* node[TRACT_HEIGHT_MOST];   /* by level: the node the path goes through */
    uint32_t     slot[TRACT_HEIGHT_MOST];   /* ... the kid of it it takes */
    uint64_t     start[TRACT_HEIGHT_MOST];  /* ... and the file's block the node begins at */
} TractPath;

// ----------------------------------------------------------------------------
// Parcels and kids
// ----------------------------------------------------------------------------

static Parcel* ParcelMake(const Strip* strips, uint32_t count)
{
    Parcel* p = kmalloc(ParcelBytes(count));
    if (!p)
        return NULL;
    uint64_t* end = (uint64_t*)(void*)&p->strip[count];
    p->refs   = 1;
    p->count  = count;
    p->blocks = 0;
    memcpy(p->strip, strips, sizeof(Strip) * count);
    for (uint32_t i = 0; i < count; i++) {
        p->blocks += strips[i].count;
        end[i] = p->blocks;
    }
    return p;
}

static void ParcelLetGo(const Parcel* p)
{
    if (p && __atomic_sub_fetch(&((Parcel*)p)->refs, 1, __ATOMIC_ACQ_REL) == 0)
        kfree((Parcel*)p);
}

/* A kid of a node of `level`: a Parcel at level 0, else a node one down. */
static uint64_t KidBlocks(const void* kid, uint16_t level)
{
    return level == 0 ? ((const Parcel*)kid)->blocks : TractBlocks(kid);
}

static uint64_t KidStrips(const void* kid, uint16_t level)
{
    return level == 0 ? ((const Parcel*)kid)->count : ((const Tract*)kid)->strips;
}

static uint64_t KidBytes(const void* kid, uint16_t level)
{
    return level == 0 ? ParcelBytes(((const Parcel*)kid)->count) : ((const Tract*)kid)->bytes;
}

static void KidHold(const void* kid, uint16_t level)
{
    volatile uint32_t* refs = level == 0 ? &((Parcel*)kid)->refs : &((Tract*)kid)->refs;
    __atomic_add_fetch(refs, 1, __ATOMIC_ACQ_REL);
}

static void KidLetGo(const void* kid, uint16_t level)
{
    if (level == 0)
        ParcelLetGo(kid);
    else
        TractLetGo(kid);
}

/* What a builder holds of `count` kids of nodes of `level`, let go, and the
 * list with them. */
static void KidsLetGo(const void** kids, uint64_t count, uint16_t level)
{
    for (uint64_t i = 0; kids && i < count; i++)
        KidLetGo(kids[i], level);
    kfree(kids);
}

/* The kid of `t` block k of it lies under: the first whose end is past k. */
static uint32_t KidAt(const Tract* t, uint64_t k, uint64_t* steps)
{
    uint32_t lo = 0, hi = t->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        (*steps)++;
        if (t->kid[mid].end > k)
            hi = mid;
        else
            lo = mid + 1u;
    }
    return lo;
}

static uint64_t KidStart(const Tract* t, uint32_t i)
{
    return i ? t->kid[i - 1u].end : 0;
}

// ----------------------------------------------------------------------------
// Nodes
// ----------------------------------------------------------------------------

void TractHold(const Tract* t)
{
    if (t)
        __atomic_add_fetch(&((Tract*)t)->refs, 1, __ATOMIC_ACQ_REL);
}

/* Let go with no recursion: a node whose last reference this was is walked
 * kid by kid on a path of at most its level + 1 nodes, each kid let go, and
 * freed when the walk comes back up past it. */
void TractLetGo(const Tract* t)
{
    struct {
        Tract*   node;
        uint32_t next;
    } path[TRACT_HEIGHT_MOST];
    uint32_t depth = 0;

    if (!t || __atomic_sub_fetch(&((Tract*)t)->refs, 1, __ATOMIC_ACQ_REL) != 0)
        return;
    path[depth].node = (Tract*)t;
    path[depth].next = 0;
    depth++;
    while (depth) {
        Tract* n = path[depth - 1u].node;
        if (path[depth - 1u].next == n->count) {
            kfree(n);
            depth--;
            continue;
        }
        const void* kid = n->kid[path[depth - 1u].next++].node;
        if (n->level == 0) {
            ParcelLetGo(kid);
        } else if (__atomic_sub_fetch(&((Tract*)kid)->refs, 1, __ATOMIC_ACQ_REL) == 0) {
            path[depth].node = (Tract*)kid;
            path[depth].next = 0;
            depth++;
        }
    }
}

/* A node of `level` naming `count` kids, a reference taken on each: NULL for
 * no memory. */
static Tract* NodeMake(uint16_t level, const void* const* kids, uint32_t count)
{
    Tract* t = kmalloc(sizeof(Tract) + sizeof(TractKid) * count);
    if (!t)
        return NULL;
    uint64_t end = 0;
    t->refs   = 1;
    t->level  = level;
    t->count  = (uint16_t)count;
    t->strips = 0;
    t->bytes  = sizeof(Tract) + sizeof(TractKid) * count;
    for (uint32_t i = 0; i < count; i++) {
        KidHold(kids[i], level);
        end       += KidBlocks(kids[i], level);
        t->strips += KidStrips(kids[i], level);
        t->bytes  += KidBytes(kids[i], level);
        t->kid[i]  = (TractKid){ .end = end, .node = kids[i] };
    }
    return t;
}

/* How many pieces `count` things are cut into, at most `most` each, as even
 * as they come: every piece then holds at least half the most when there are
 * two or more. */
static uint64_t CutPieces(uint64_t count, uint64_t most)
{
    return (count + most - 1u) / most;
}

static uint64_t CutLength(uint64_t count, uint64_t pieces, uint64_t i)
{
    return count / pieces + (i < count % pieces ? 1u : 0u);
}

/* `count` kids of nodes of `level`, cut evenly into nodes: the nodes in a new
 * list in *out, *nout of them, each held by the caller. ERR_NO_MEMORY with
 * nothing made. */
static error_t NodesCut(const void* const* kids, uint64_t count, uint16_t level,
                        const void*** out, uint64_t* nout)
{
    uint64_t     pieces = CutPieces(count, TRACT_KIDS_MOST);
    const void** nodes  = pieces ? kmalloc(sizeof(void*) * pieces) : NULL;
    *out  = NULL;
    *nout = 0;
    if (pieces && !nodes)
        return ERR_NO_MEMORY;
    for (uint64_t i = 0, at = 0; i < pieces; i++) {
        uint64_t len = CutLength(count, pieces, i);
        if (!(nodes[i] = NodeMake(level, &kids[at], (uint32_t)len))) {
            KidsLetGo(nodes, i, (uint16_t)(level + 1u));
            return ERR_NO_MEMORY;
        }
        at += len;
    }
    *out  = nodes;
    *nout = pieces;
    return OK;
}

/* `count` Strips cut evenly into Parcels: a new list in *out, each Parcel
 * held by the caller. ERR_NO_MEMORY with nothing made. */
static error_t ParcelsCut(const Strip* strips, uint64_t count, const void*** out, uint64_t* nout)
{
    uint64_t     pieces = CutPieces(count, PARCEL_STRIPS_MOST);
    const void** parcels = pieces ? kmalloc(sizeof(void*) * pieces) : NULL;
    *out  = NULL;
    *nout = 0;
    if (pieces && !parcels)
        return ERR_NO_MEMORY;
    for (uint64_t i = 0, at = 0; i < pieces; i++) {
        uint64_t len = CutLength(count, pieces, i);
        if (!(parcels[i] = ParcelMake(&strips[at], (uint32_t)len))) {
            KidsLetGo(parcels, i, 0);
            return ERR_NO_MEMORY;
        }
        at += len;
    }
    *out  = parcels;
    *nout = pieces;
    return OK;
}

/* ‼ A TRACT IS NEVER TALLER THAN TRACT_HEIGHT_MOST, by the fill every node
 * keeps (file_record.h). A build that would make it taller is a defect of
 * that fill, refused here rather than walked past the end of a path. */
static error_t TractTooTall(uint32_t level)
{
#if CONFIG_STRICT
    panic("[TagFS] DEFECT: a Tract of %u levels was about to be built — its nodes did not "
          "keep their fill", level + 1u);
#endif
    kprintf("[TagFS] DEFECT: a Tract of %u levels was about to be built — its nodes did not keep "
            "their fill; the change is refused\n", level + 1u);
    return ERR_NO_MEMORY;
}

/* The top of a Tract: `items`, kids of nodes of `level` that a build holds —
 * Parcels at level 0 — cut into nodes level by level until one node names
 * them all, and a root of a single kid above level 0 given up for that kid.
 * The builder's references and the list are let go; *out NULL when there are
 * no items. */
static error_t TractRise(const void** items, uint64_t count, uint16_t level, const Tract** out)
{
    *out = NULL;
    while (count > 1 || (count == 1 && level == 0)) {
        const void** up  = NULL;
        uint64_t     nup = 0;
        error_t      rc  = level >= TRACT_HEIGHT_MOST ? TractTooTall(level)
                                                     : NodesCut(items, count, level, &up, &nup);
        KidsLetGo(items, count, level);
        if (rc != OK)
            return rc;
        items = up;
        count = nup;
        level++;
    }
    if (count == 0) {
        kfree(items);
        return OK;
    }
    const Tract* root = items[0];
    kfree(items);
    while (root->level > 0 && root->count == 1) {
        const Tract* kid = root->kid[0].node;
        TractHold(kid);
        TractLetGo(root);
        root = kid;
    }
    *out = root;
    return OK;
}

error_t TractOfStrips(const Strip* strips, uint64_t count, const Tract** out)
{
    const void** parcels = NULL;
    uint64_t     n       = 0;
    *out = NULL;
    for (uint64_t i = 0; i < count; i++)
        if (strips[i].count == 0 || strips[i].kind > STRIP_HOLE)
            return ERR_INVALID_ARGUMENT;
    error_t rc = ParcelsCut(strips, count, &parcels, &n);
    return rc == OK ? TractRise(parcels, n, 0, out) : rc;
}

/*
 * ‼ A PARCEL OF THE MEDIUM IS ONE PARCEL OF MEMORY (§16.1 п.7, §2.9: the
 * boundary is the writer's, the reader does not read it again). A splice cuts
 * only the Parcels it touched (SpliceGather), so a file written in pieces lies
 * in the Parcels its splices left — not the even cut of all its Strips — and a
 * Tally lays memory's one for one. Cut again at the mount, memory would hold
 * a file its Tally does not, and DogEar, the keys changed since that Tally,
 * would miss every key the new cut moved (settle/dog_ear.c ParcelsChanged).
 * A piece of more than PARCEL_STRIPS_MOST — a run past a Strip's count laid in
 * as more than one — is cut as evenly as it goes, and *recut says so.
 */
error_t TractOfParcels(const Strip* strips, uint64_t count, const uint32_t* cut, uint64_t ncut,
                       const Tract** out, bool* recut)
{
    uint64_t pieces = 0, sum = 0, made = 0, at = 0;
    *out   = NULL;
    *recut = false;
    for (uint64_t i = 0; i < count; i++)
        if (strips[i].count == 0 || strips[i].kind > STRIP_HOLE)
            return ERR_INVALID_ARGUMENT;
    for (uint64_t i = 0; i < ncut; i++) {
        if (cut[i] == 0)
            return ERR_TAGFS_CORRUPTED;
        sum    += cut[i];
        pieces += CutPieces(cut[i], PARCEL_STRIPS_MOST);
        *recut |= cut[i] > PARCEL_STRIPS_MOST;
    }
    if (sum != count)
        return ERR_TAGFS_CORRUPTED;
    const void** parcels = pieces ? kmalloc(sizeof(void*) * pieces) : NULL;
    if (pieces && !parcels)
        return ERR_NO_MEMORY;
    for (uint64_t i = 0; i < ncut; i++) {
        uint64_t n = CutPieces(cut[i], PARCEL_STRIPS_MOST);
        for (uint64_t j = 0; j < n; j++, made++) {
            uint64_t len = CutLength(cut[i], n, j);
            if (!(parcels[made] = ParcelMake(&strips[at], (uint32_t)len))) {
                KidsLetGo(parcels, made, 0);
                return ERR_NO_MEMORY;
            }
            at += len;
        }
    }
    return TractRise(parcels, pieces, 0, out);
}

// ----------------------------------------------------------------------------
// Found
// ----------------------------------------------------------------------------

/* The path down to the Parcel holding block k of `root` (k < its blocks). */
static void TractDescend(const Tract* root, uint64_t k, TractPath* path)
{
    const Tract* t     = root;
    uint64_t     base  = 0;
    uint64_t     steps = 0;
    for (;;) {
        uint32_t i = KidAt(t, k - base, &steps);
        path->node[t->level]  = t;
        path->slot[t->level]  = i;
        path->start[t->level] = base;
        base += KidStart(t, i);
        if (t->level == 0)
            return;
        t = t->kid[i].node;
    }
}

/* The Parcel at the end of a path, and the block it begins at. */
static const Parcel* PathParcel(const TractPath* path, uint64_t* start)
{
    const Tract* leaf = path->node[0];
    *start = path->start[0] + KidStart(leaf, path->slot[0]);
    return leaf->kid[path->slot[0]].node;
}

bool TractFind(const Tract* root, uint64_t k, const Parcel** out, uint64_t* k_in,
               uint64_t* steps)
{
    if (!root || k >= TractBlocks(root))
        return false;
    const Tract* t = root;
    for (;;) {
        uint32_t i = KidAt(t, k, steps);
        k -= KidStart(t, i);
        if (t->level == 0) {
            *out  = t->kid[i].node;
            *k_in = k;
            return true;
        }
        t = t->kid[i].node;
    }
}

bool TractWalkAt(const Tract* root, uint64_t k, StripWalk* w, uint64_t* begins)
{
    uint64_t steps = 0, at = k;
    w->depth  = 0;
    w->parcel = NULL;
    w->strip  = 0;
    if (!root || k >= TractBlocks(root))
        return false;
    for (const Tract* t = root;;) {
        uint32_t i = KidAt(t, at, &steps);
        at -= KidStart(t, i);
        w->node[w->depth] = t;
        w->slot[w->depth] = i;
        w->depth++;
        if (t->level == 0) {
            const Parcel*   p   = t->kid[i].node;
            const uint64_t* end = ParcelEnds(p);
            uint32_t        lo = 0, hi = p->count;
            while (lo < hi) {
                uint32_t mid = lo + (hi - lo) / 2u;
                if (end[mid] > at)
                    hi = mid;
                else
                    lo = mid + 1u;
            }
            w->parcel = p;
            w->strip  = lo;
            *begins   = k - (at - (lo ? end[lo - 1u] : 0));
            return true;
        }
        t = t->kid[i].node;
    }
}

// ----------------------------------------------------------------------------
// Spliced
// ----------------------------------------------------------------------------

/* What one splice works with, in the heap: three paths do not fit a frame. */
typedef struct {
    const Tract* root;
    uint64_t     blocks;      /* the root's */
    TractPath    a;           /* down to the first Parcel the splice replaces */
    TractPath    b;           /* ... and to the last */
    TractPath    side;        /* down to a neighbour the range is joined with */
    uint64_t     a_start;     /* the block the first begins at */
    uint64_t     b_end;       /* the block past the last */
    Strip*       strips;      /* the Strips that take their place */
    uint64_t     count;
} SpliceWork;

error_t TractGroundAdd(TractGround* g, const Strip* s)
{
    if (g->count == g->room) {
        uint64_t room  = g->room ? g->room * 2u : 16u;
        Strip*   grown = kmalloc(sizeof(Strip) * room);
        if (!grown)
            return ERR_NO_MEMORY;
        if (g->count)
            memcpy(grown, g->strip, sizeof(Strip) * g->count);
        kfree(g->strip);
        g->strip = grown;
        g->room  = room;
    }
    g->strip[g->count++] = *s;
    return OK;
}

/* `len` blocks of Strip `s` from its `off`-th: a DATA piece begins where its
 * blocks do; a hole names no block, and keeps the start it had. */
static Strip StripPiece(const Strip* s, uint64_t off, uint64_t len)
{
    Strip p = *s;
    if (s->kind == STRIP_DATA)
        p.start = s->start + off;
    p.count = (uint32_t)len;
    return p;
}

/* The pieces of Parcel `p`, which begins at block `pstart`, that lie before
 * block `edge` (or, `after`, from it on): appended to the work's Strips. */
static void ParcelKeep(SpliceWork* w, const Parcel* p, uint64_t pstart, uint64_t edge, bool after)
{
    const uint64_t* end = ParcelEnds(p);
    for (uint32_t i = 0; i < p->count; i++) {
        uint64_t a = pstart + (i ? end[i - 1u] : 0), b = pstart + end[i];
        if (!after && a < edge)
            w->strips[w->count++] = StripPiece(&p->strip[i], 0, (b < edge ? b : edge) - a);
        else if (after && b > edge) {
            uint64_t off = edge > a ? edge - a : 0;
            w->strips[w->count++] = StripPiece(&p->strip[i], off, b - a - off);
        }
    }
}

/* The DATA ground of Parcel `p`, which begins at block `pstart`, lying in
 * [lo, hi) of the file: added to `took`. */
static error_t ParcelTook(const Parcel* p, uint64_t pstart, uint64_t lo, uint64_t hi,
                          TractGround* took)
{
    const uint64_t* end = ParcelEnds(p);
    for (uint32_t i = 0; i < p->count; i++) {
        uint64_t a = pstart + (i ? end[i - 1u] : 0), b = pstart + end[i];
        uint64_t x = a > lo ? a : lo, y = b < hi ? b : hi;
        if (x < y && p->strip[i].kind == STRIP_DATA) {
            Strip piece = StripPiece(&p->strip[i], x - a, y - x);
            if (TractGroundAdd(took, &piece) != OK)
                return ERR_NO_MEMORY;
        }
    }
    return OK;
}

/* Every DATA piece the splice takes out, Parcel by Parcel from the first it
 * replaces to the last. */
static error_t SpliceTook(SpliceWork* w, const BookSplice* sp, TractGround* took)
{
    uint64_t      pstart = 0;
    const Parcel* p      = PathParcel(&w->a, &pstart);
    for (;;) {
        if (ParcelTook(p, pstart, sp->at, sp->at + sp->remove, took) != OK)
            return ERR_NO_MEMORY;
        if (pstart + p->blocks >= sp->at + sp->remove)
            return OK;
        TractDescend(w->root, pstart + p->blocks, &w->side);
        p = PathParcel(&w->side, &pstart);
    }
}

/* The Parcels the splice touches, found, and the Strips that replace them
 * gathered: theirs before `at`, the inserted ones, theirs from at + remove
 * on. An insertion at the end touches the last Parcel. Room is left for a
 * neighbour's Strips (SpliceJoin). */
static error_t SpliceGather(SpliceWork* w, const BookSplice* sp, TractGround* took)
{
    uint64_t first = sp->at < w->blocks ? sp->at : w->blocks - 1u;
    uint64_t last  = sp->remove ? sp->at + sp->remove - 1u : first;
    uint64_t fstart = 0, lstart = 0;
    TractDescend(w->root, first, &w->a);
    TractDescend(w->root, last, &w->b);
    const Parcel* f = PathParcel(&w->a, &fstart);
    const Parcel* l = PathParcel(&w->b, &lstart);
    w->a_start = fstart;
    w->b_end   = lstart + l->blocks;
    w->strips  = kmalloc(sizeof(Strip) * (sp->ninsert + 3u * PARCEL_STRIPS_MOST));
    if (!w->strips)
        return ERR_NO_MEMORY;
    ParcelKeep(w, f, fstart, sp->at, false);
    memcpy(&w->strips[w->count], sp->insert, sizeof(Strip) * sp->ninsert);
    w->count += sp->ninsert;
    ParcelKeep(w, l, lstart, sp->at + sp->remove, true);
    return took && sp->remove ? SpliceTook(w, sp, took) : OK;
}

/* Fewer Strips left than a Parcel beside another holds at least: a neighbour
 * of the range — the next, else the one before — joins them. */
static void SpliceJoin(SpliceWork* w)
{
    uint64_t      start = 0;
    const Parcel* p;
    if (w->count == 0 || w->count >= PARCEL_STRIPS_LEAST)
        return;
    if (w->b_end < w->blocks) {
        TractDescend(w->root, w->b_end, &w->b);
        p = PathParcel(&w->b, &start);
        memcpy(&w->strips[w->count], p->strip, sizeof(Strip) * p->count);
        w->b_end = start + p->blocks;
    } else if (w->a_start > 0) {
        TractDescend(w->root, w->a_start - 1u, &w->a);
        p = PathParcel(&w->a, &start);
        memmove(&w->strips[p->count], w->strips, sizeof(Strip) * w->count);
        memcpy(w->strips, p->strip, sizeof(Strip) * p->count);
        w->a_start = start;
    } else {
        return;
    }
    w->count += p->count;
}

/* The kids of the nodes of level L over the range, gathered with `items` —
 * what replaces the range one level down — in place of the old: into a new
 * list of borrowed pointers. Fewer than a node beside another holds at least,
 * below the root: a neighbour of the range at this level joins them, and the
 * path past it is the range's from here up. */
static error_t LevelGather(SpliceWork* w, uint16_t L, const void* const* items, uint64_t n,
                           const void*** out, uint64_t* nout)
{
    const Tract* a  = w->a.node[L];
    const Tract* b  = w->b.node[L];
    uint32_t     sa = w->a.slot[L], sb = w->b.slot[L];
    uint64_t     ng = sa + n + (b->count - sb - 1u);
    const void** g  = kmalloc(sizeof(void*) * (ng + TRACT_KIDS_MOST));
    const Tract* side = NULL;
    uint64_t     at  = 0;

    if (!g)
        return ERR_NO_MEMORY;
    if (L < w->root->level && ng < TRACT_KIDS_LEAST) {
        uint64_t b_end = w->b.start[L] + TractBlocks(b);
        if (w->a.start[L] > 0)
            TractDescend(w->root, w->a.start[L] - 1u, &w->side);
        else if (b_end < w->blocks)
            TractDescend(w->root, b_end, &w->side);
        side = w->a.start[L] > 0 || b_end < w->blocks ? w->side.node[L] : NULL;
    }
    if (side && w->a.start[L] > 0)
        for (uint32_t i = 0; i < side->count; i++)
            g[at++] = side->kid[i].node;
    for (uint32_t i = 0; i < sa; i++)
        g[at++] = a->kid[i].node;
    for (uint64_t i = 0; i < n; i++)
        g[at++] = items[i];
    for (uint32_t i = sb + 1u; i < b->count; i++)
        g[at++] = b->kid[i].node;
    if (side && w->a.start[L] == 0)
        for (uint32_t i = 0; i < side->count; i++)
            g[at++] = side->kid[i].node;
    if (side) {
        TractPath* to = w->a.start[L] > 0 ? &w->a : &w->b;
        for (uint16_t up = (uint16_t)(L + 1u); up <= w->root->level; up++) {
            to->node[up]  = w->side.node[up];
            to->slot[up]  = w->side.slot[up];
            to->start[up] = w->side.start[up];
        }
    }
    *out  = g;
    *nout = at;
    return OK;
}

/* The new Parcels put in place of the range, and every level over it rebuilt
 * up to the root and past it: the new Tract in *out. The Parcels' list and
 * the builder's references are let go whatever the answer. */
static error_t SpliceLevels(SpliceWork* w, const void** items, uint64_t n, const Tract** out)
{
    uint16_t top = w->root->level;
    for (uint16_t L = 0; L <= top; L++) {
        const void** g   = NULL;
        const void** up  = NULL;
        uint64_t     ng  = 0, nup = 0;
        error_t      rc  = LevelGather(w, L, items, n, &g, &ng);
        if (rc == OK)
            rc = NodesCut(g, ng, L, &up, &nup);
        kfree(g);
        KidsLetGo(items, n, L);
        if (rc != OK)
            return rc;
        items = up;
        n     = nup;
    }
    return TractRise(items, n, (uint16_t)(top + 1u), out);
}

error_t TractSplice(const Tract* root, const BookSplice* sp, const Tract** out, TractGround* took)
{
    uint64_t blocks = TractBlocks(root), put = 0;
    *out = NULL;
    for (uint64_t i = 0; i < sp->ninsert; i++) {
        if (sp->insert[i].count == 0 || sp->insert[i].kind > STRIP_HOLE ||
            put > UINT64_MAX - sp->insert[i].count)
            return ERR_INVALID_ARGUMENT;
        put += sp->insert[i].count;
    }
    if (sp->at > blocks || sp->remove > blocks - sp->at || put > UINT64_MAX - (blocks - sp->remove))
        return ERR_INVALID_ARGUMENT;
    if (!root)
        return TractOfStrips(sp->insert, sp->ninsert, out);
    if (sp->remove == 0 && sp->ninsert == 0) {
        TractHold(root);
        *out = root;
        return OK;
    }

    SpliceWork*  w       = kmalloc(sizeof(SpliceWork));
    const void** parcels = NULL;
    uint64_t     n       = 0;
    if (!w)
        return ERR_NO_MEMORY;
    memset(w, 0, sizeof(*w));
    w->root   = root;
    w->blocks = blocks;
    error_t rc = SpliceGather(w, sp, took);
    if (rc == OK) {
        SpliceJoin(w);
        rc = ParcelsCut(w->strips, w->count, &parcels, &n);
    }
    if (rc == OK)
        rc = SpliceLevels(w, parcels, n, out);
    kfree(w->strips);
    kfree(w);
    return rc;
}
