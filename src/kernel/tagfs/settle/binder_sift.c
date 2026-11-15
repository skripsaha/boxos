#include "binder_internal.h"

/*
 * TipIn's sifting of one tree of the Tally before (tip_in.h, TipInSift) at the
 * slice of a Tally laid in (binder_kept.c, BinderTipInTake): FILES, TAGS and
 * USE out of the Foliation, DIGESTS its inner nodes out of the Foliation and
 * its leaves out of the reads' own table (mount/tally_read.h).
 *
 * ‼ THE LEAVES SIFTED ARE THE CHILDREN OF A PARENT THAT CHANGED. A leaf a span
 * meets, and every leaf a row of them may take in as its margin, lies under a
 * node of level 1 the span meets too — TipIn never offers a leaf under another
 * parent; a node of level 1 a copy of which did not hold is laid again with
 * every child of it named. Every other leaf lies under a node named as it
 * lies, or inside one, and nothing of it is said. So TipInSift is handed the
 * children of those nodes of level 1 alone — found by halving the leaves, as
 * many as the spans meet — and the tree's first leaf, whose range TipInSift
 * holds open below; and every inner node, a few hundredths of the leaves. A
 * Tally of a volume of a million leaves sifts what its changes' parents hold.
 *
 * A range of DIGESTS no copy of whose inner node reads is a place of the
 * reads' table at that node's level (book_mount.c, BookDigestRot): it is the
 * inner node the Foliation holds, never a leaf, and that node reads in no
 * copy.
 */

// ----------------------------------------------------------------------------
// The leaves and nodes of the Tally before
// ----------------------------------------------------------------------------

/* Whether `a` and `b` are one node: its level and its first copy. */
static bool PlaceSame(const TallyPlace* a, const TallyPlace* b)
{
    return a->level == b->level && a->ptr.seq == b->ptr.seq && a->ptr.copies && b->ptr.copies &&
           a->ptr.block[0] == b->ptr.block[0];
}

/* Whether the mount found no copy of the node at `p` of `tree` to read. */
static bool MountRotten(uint64_t tree, const TallyPlace* p)
{
    for (uint32_t i = 0; i < g_binder.nrot; i++)
        if (g_binder.rot[i].tree == tree && PlaceSame(&g_binder.rot[i].at, p))
            return true;
    return false;
}

uint32_t BinderSiftLeaves(uint64_t tree)
{
    uint32_t n = 0;
    if (tree == TALLY_DIGESTS)
        return TallyDigestLeaves();
    (void)FoliationLeaves(tree, &n);
    return n;
}

bool BinderSiftLeafAt(uint64_t tree, uint32_t i, TallyPlace* p, bool* rotten)
{
    uint32_t             n = 0;
    const FoliationNode* f = NULL;
    if (tree == TALLY_DIGESTS)
        return TallyDigestLeafAt(i, p, rotten);
    f = FoliationLeaves(tree, &n);
    if (i >= n)
        return false;
    *p      = f[i].place;
    *rotten = MountRotten(tree, p);
    return true;
}

/* The first leaf from the second on whose lower key is not below `key` — the
 * leaves past `n` when none is. */
static uint32_t LeafFrom(uint64_t tree, uint32_t n, TallyKey key)
{
    return BinderSiftLeafFrom(tree, 1, n, key);
}

uint32_t BinderSiftLeafFrom(uint64_t tree, uint32_t from, uint32_t n, TallyKey key)
{
    uint32_t   lo = from, hi = n;
    TallyPlace p;
    bool       rotten;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (BinderSiftLeafAt(tree, mid, &p, &rotten) && p.has_lo && !TipInKeyLess(tree, p.lo, key))
            hi = mid;
        else
            lo = mid + 1u;
    }
    return lo < n ? lo : n;
}

uint32_t BinderSiftLeafOf(uint64_t tree, uint32_t n, TallyKey key)
{
    uint32_t   lo = 1, hi = n;
    TallyPlace q;
    bool       rotten = false;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (BinderSiftLeafAt(tree, mid, &q, &rotten) && (!q.has_lo || !TipInKeyLess(tree, key, q.lo)))
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo - 1u;
}

/* Whether the node of DIGESTS at `p` reads in no copy: the place of the reads'
 * table its first key lies in is its range, at its own level, rotten. */
static bool TableRotten(const TallyPlace* p, uint32_t n)
{
    TallyPlace q;
    bool       rotten = false;
    if (n == 0 || !BinderSiftLeafAt(TALLY_DIGESTS, p->has_lo ? BinderSiftLeafOf(TALLY_DIGESTS, n, p->lo) : 0, &q,
                                    &rotten))
        return false;
    return rotten && q.level == p->level && (!q.has_hi || !p->has_lo || TipInKeyLess(TALLY_DIGESTS, p->lo, q.hi));
}

// ----------------------------------------------------------------------------
// The parents that changed
// ----------------------------------------------------------------------------

/* The node of level 1 of `run` (`n`) whose range holds `key`: the last whose
 * lower key is not past it — the first for a key below every one. */
static uint32_t ParentOf(uint64_t tree, const FoliationNode* run, uint32_t n, TallyKey key)
{
    uint32_t lo = 1, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (!run[mid].place.has_lo || !TipInKeyLess(tree, key, run[mid].place.lo))
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo - 1u;
}

/* Every node of level 1 a span meets — its range from its own lower key to
 * the next one's, the first open below — or a copy of which did not hold,
 * marked in `mark`. */
static void ParentsMarked(uint64_t tree, const FoliationNode* run, uint32_t n, const DogEarSpan* span,
                          uint32_t nspan, bool* mark)
{
    for (uint32_t s = 0; s < nspan; s++)
        for (uint32_t j = ParentOf(tree, run, n, span[s].lo), first = j; j < n; j++) {
            if (j > first && run[j].place.has_lo && !TipInKeyLess(tree, run[j].place.lo, span[s].hi))
                break;
            mark[j] = true;
        }
    for (uint32_t j = 0; j < n; j++)
        mark[j] |= run[j].relay;
}

/* The leaves [*from, *to) under the node of level 1 `j` of `run`. */
static void ChildrenOf(uint64_t tree, uint32_t nleaf, const FoliationNode* run, uint32_t n, uint32_t j,
                       uint32_t* from, uint32_t* to)
{
    *from = j == 0 || !run[j].place.has_lo ? 0 : LeafFrom(tree, nleaf, run[j].place.lo);
    *to   = j + 1u == n || !run[j + 1u].place.has_lo ? nleaf : LeafFrom(tree, nleaf, run[j + 1u].place.lo);
}

/* One leaf more for the sift — a place of the reads' table above level 0 is
 * an inner node's range, never a leaf. A leaf a copy of which did not hold is
 * never marked: its range is in DogEar (book_ground.c, BookGroundMended;
 * tally_read.c, TallyDigestMended), which marks its parent. */
static void LeafPut(BinderSift* s, uint32_t i)
{
    TallyPlace p;
    bool       rotten = false;
    if (!BinderSiftLeafAt(s->tree, i, &p, &rotten) || p.level != 0)
        return;
    s->leaf[s->n] = (TipInNode){ .place = p, .relay = false, .rotten = rotten };
    s->at[s->n++] = i;
}

/* The leaves the sift is handed: the tree's first, then the children of every
 * parent marked, in key order — every leaf when the tree has no inner node. */
static error_t LeavesGathered(BinderSift* s, uint32_t nleaf, const FoliationNode* run, uint32_t n,
                              const bool* mark)
{
    uint32_t most = 1, from = 0, to = 0;
    for (uint32_t j = 0; j < n; j++) {
        ChildrenOf(s->tree, nleaf, run, n, j, &from, &to);
        most += mark[j] ? to - from : 0;
    }
    if (n == 0)
        most = nleaf;
    s->leaf = kmalloc(sizeof(TipInNode) * (most ? most : 1u));
    s->at   = kmalloc(sizeof(uint32_t) * (most ? most : 1u));
    s->lf   = kmalloc(sizeof(TipInFate) * (most ? most : 1u));
    if (!s->leaf || !s->at || !s->lf)
        return ERR_NO_MEMORY;
    if (n == 0) {
        for (uint32_t i = 0; i < nleaf; i++)
            LeafPut(s, i);
        return OK;
    }
    if (nleaf)
        LeafPut(s, 0);
    for (uint32_t j = 0; j < n; j++) {
        if (!mark[j])
            continue;
        ChildrenOf(s->tree, nleaf, run, n, j, &from, &to);
        for (uint32_t i = from ? from : 1u; i < to; i++)
            LeafPut(s, i);
    }
    return OK;
}

/* Every inner node of the tree, as the Foliation holds them after FoliationLevel
 * ordered them by level and key. */
static error_t InnerGathered(BinderSift* s, uint32_t nleaf)
{
    uint32_t             ni = 0;
    const FoliationNode* fi = FoliationInner(s->tree, &ni);
    s->inner = kmalloc(sizeof(TipInNode) * (ni ? ni : 1u));
    s->inf   = kmalloc(sizeof(TipInFate) * (ni ? ni : 1u));
    if (!s->inner || !s->inf)
        return ERR_NO_MEMORY;
    for (uint32_t j = 0; j < ni; j++)
        s->inner[j] = (TipInNode){ .place = fi[j].place, .relay = fi[j].relay,
                                   .rotten = s->tree == TALLY_DIGESTS ? TableRotten(&fi[j].place, nleaf)
                                                                      : MountRotten(s->tree, &fi[j].place) };
    s->ninner = ni;
    return OK;
}

error_t BinderSiftTree(BinderSift* s, uint64_t tree)
{
    uint32_t             n1 = 0, nspan = 0, nleaf = BinderSiftLeaves(tree);
    const FoliationNode* run = FoliationLevel(tree, 1, &n1);
    const DogEarSpan*    sp  = DogEarTakenSpans(tree, &nspan);
    bool*                mark = n1 ? kmalloc(sizeof(bool) * n1) : NULL;
    error_t              rc   = n1 && !mark ? ERR_NO_MEMORY : OK;
    memset(s, 0, sizeof(*s));
    s->tree = tree;
    if (rc == OK) {
        if (n1)
            memset(mark, 0, sizeof(bool) * n1);
        ParentsMarked(tree, run, n1, sp, nspan, mark);
        rc = LeavesGathered(s, nleaf, run, n1, mark);
    }
    kfree(mark);
    if (rc == OK)
        rc = InnerGathered(s, nleaf);
    if (rc == OK)
        rc = BinderSiftAgain(s);
    if (rc != OK)
        BinderSiftLetGo(s);
    return rc;
}

error_t BinderSiftAgain(BinderSift* s)
{
    uint32_t          nspan = 0;
    const DogEarSpan* sp    = DogEarTakenSpans(s->tree, &nspan);
    return TipInSift(s->tree, s->leaf, s->n, s->inner, s->ninner, sp, nspan, s->lf, s->inf);
}

/* The blocks one copy of the node at `p` takes, on an atom of `grid`. */
static uint64_t NodeBlocks(const TallyPlace* p, uint32_t grid)
{
    return (p->ptr.sheets + grid - 1u) / grid * grid;
}

/* A leaf laid that keeps no key of the state lays nothing, laid in or whole:
 * neither count takes it (§16.6 п.6, binder_live.c) — a margin keeps every
 * key it had, none of its range changed. Both lose the same blocks, so laid in
 * against whole is weighed as it would be with them; `sifted` keeps them, so
 * a Tally whose every leaf laid emptied is weighed — and, no cheaper laid in,
 * written whole — as before (binder_kept.c, BinderTipInTake). Asked once a
 * Tally, a leaf laid at a time, at its slice. */
void BinderSiftCount(const BinderSift* s, uint32_t grid, TipInCount* c)
{
    TallyPlace p;
    bool       rotten;
    uint64_t   whole = 0, empty = 0;
    for (uint32_t i = 0; i < s->n; i++) {
        uint64_t blocks = NodeBlocks(&s->leaf[i].place, grid);
        if (s->lf[i] == TIP_IN_LAID && !BinderLeafLives(s->tree, &s->leaf[i].place, s->at[i] == 0, true))
            empty += blocks;
        else if (s->lf[i] == TIP_IN_LAID || s->lf[i] == TIP_IN_MARGIN)
            c->tip += blocks;
    }
    for (uint32_t j = 0; j < s->ninner; j++) {
        c->tip += s->inf[j] == TIP_IN_LAID ? NodeBlocks(&s->inner[j].place, grid) : 0;
        whole  += NodeBlocks(&s->inner[j].place, grid);
    }
    for (uint32_t i = 0; BinderSiftLeafAt(s->tree, i, &p, &rotten); i++)
        whole += p.level == 0 ? NodeBlocks(&p, grid) : 0;
    c->whole  += whole - empty;
    c->sifted += whole;
}

void BinderSiftLetGo(BinderSift* s)
{
    kfree(s->leaf);
    kfree(s->at);
    kfree(s->lf);
    kfree(s->inner);
    kfree(s->inf);
    memset(s, 0, sizeof(*s));
}
