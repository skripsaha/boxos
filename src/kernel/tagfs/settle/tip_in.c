#include "tip_in.h"

/*
 * TipIn's sifting of one tree (tip_in.h): the leaves weighed against the spans
 * in one walk of both — each in key order — the rows and their margins found
 * among the leaves, then each inner node held to the leaves it covers, and the
 * nodes named as they lie that no other such node holds kept, everything under
 * them said not at all.
 */

// ----------------------------------------------------------------------------
// Keys and ranges
// ----------------------------------------------------------------------------

bool TipInKeyLess(uint64_t tree, TallyKey a, TallyKey b)
{
    if (a.k1 != b.k1)
        return a.k1 < b.k1;
    return TallyPairKeyed(tree) && a.k2 < b.k2;
}

static bool KeySame(uint64_t tree, TallyKey a, TallyKey b)
{
    return !TipInKeyLess(tree, a, b) && !TipInKeyLess(tree, b, a);
}

bool TipInHolds(uint64_t tree, const TallyPlace* p, TallyKey k)
{
    return (!p->has_lo || !TipInKeyLess(tree, k, p->lo)) && (!p->has_hi || TipInKeyLess(tree, k, p->hi));
}

bool TipInWithin(uint64_t tree, const TallyPlace* in, const TallyPlace* out)
{
    bool lo = !out->has_lo || (in->has_lo && !TipInKeyLess(tree, in->lo, out->lo));
    bool hi = !out->has_hi || (in->has_hi && !TipInKeyLess(tree, out->hi, in->hi));
    return lo && hi;
}

void TipInLeavesClosed(TallyPlace* p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        p[i].has_lo = n > 1u;
        p[i].has_hi = i + 1u < n;
        p[i].hi     = i + 1u < n ? p[i + 1u].lo : (TallyKey){ 0, 0 };
    }
}

/* Whether the ranges of `a` and `b` meet. */
static bool PlacesMeet(uint64_t tree, const TallyPlace* a, const TallyPlace* b)
{
    bool below = !a->has_lo || !b->has_hi || TipInKeyLess(tree, a->lo, b->hi);
    bool above = !b->has_lo || !a->has_hi || TipInKeyLess(tree, b->lo, a->hi);
    return below && above;
}

/* Whether a span of `span` (in key order, disjoint) meets the range of `p`:
 * the first span that ends past its lower key, and whether it begins before
 * its upper one. */
static bool SpansMeet(uint64_t tree, const DogEarSpan* span, uint32_t n, const TallyPlace* p)
{
    uint32_t lo = 0, hi = n;
    while (p->has_lo && lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (TipInKeyLess(tree, p->lo, span[mid].hi))
            hi = mid;
        else
            lo = mid + 1u;
    }
    return lo < n && (!p->has_hi || TipInKeyLess(tree, span[lo].lo, p->hi));
}

// ----------------------------------------------------------------------------
// The leaves: laid, margins, kept
// ----------------------------------------------------------------------------

/* The lower keys of the inner nodes of level 1, in order: where a parent's
 * first leaf begins. */
typedef struct {
    TallyKey* key;
    uint32_t  n;
} TipInFirsts;

static bool FirstsOf(uint64_t tree, const TipInNode* inner, uint32_t ninner, TipInFirsts* f)
{
    f->n   = 0;
    f->key = NULL;
    for (uint32_t j = 0; j < ninner; j++)
        f->n += inner[j].place.level == 1u && inner[j].place.has_lo;
    if (f->n && !(f->key = kmalloc(sizeof(TallyKey) * f->n)))
        return false;
    uint32_t at = 0;
    for (uint32_t j = 0; j < ninner; j++) {
        if (inner[j].place.level != 1u || !inner[j].place.has_lo)
            continue;
        uint32_t i = at++;
        while (i > 0 && TipInKeyLess(tree, inner[j].place.lo, f->key[i - 1u])) {
            f->key[i] = f->key[i - 1u];
            i--;
        }
        f->key[i] = inner[j].place.lo;
    }
    return true;
}

/* Whether leaf `i + 1` begins where leaf `i` ends: no node no copy of which
 * reads, nor its leaves, between them. */
static bool Adjacent(uint64_t tree, const TipInNode* leaf, uint32_t i)
{
    const TallyPlace* a = &leaf[i].place;
    const TallyPlace* b = &leaf[i + 1u].place;
    return a->has_hi && b->has_lo && KeySame(tree, a->hi, b->lo);
}

/* Whether leaf `i + 1` follows leaf `i` under the same parent: adjacent, and
 * no parent of level 1 begins at the second. */
static bool Siblings(uint64_t tree, const TipInNode* leaf, uint32_t i, const TipInFirsts* f)
{
    const TallyPlace* b = &leaf[i + 1u].place;
    if (!Adjacent(tree, leaf, i))
        return false;
    uint32_t lo = 0, hi = f->n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (TipInKeyLess(tree, f->key[mid], b->lo))
            lo = mid + 1u;
        else
            hi = mid;
    }
    return !(lo < f->n && KeySame(tree, f->key[lo], b->lo));
}

/* A leaf a row may take in: kept, and readable. */
static bool MarginMay(const TipInNode* leaf, const TipInFate* fate, uint32_t i)
{
    return fate[i] == TIP_IN_KEPT && !leaf[i].rotten;
}

/* Whether a span meets leaf `i`: every key below the tree's first separator
 * is its first leaf's. */
static bool LeafMet(uint64_t tree, const TipInNode* leaf, uint32_t i, const DogEarSpan* span, uint32_t nspan)
{
    TallyPlace p = leaf[i].place;
    p.has_lo     = p.has_lo && i > 0;
    return SpansMeet(tree, span, nspan, &p);
}

/* The leaves laid — a span meets them, or a copy of them did not hold — and
 * the margin of each row of them: leaves laid one after another, whatever
 * their parents, are packed as one row between the nodes named as they lie. */
static void LeavesSifted(uint64_t tree, const TipInNode* leaf, uint32_t nleaf, const DogEarSpan* span,
                         uint32_t nspan, const TipInFirsts* f, TipInFate* fate)
{
    for (uint32_t i = 0; i < nleaf; i++)
        fate[i] = !leaf[i].rotten && (leaf[i].relay || LeafMet(tree, leaf, i, span, nspan)) ? TIP_IN_LAID
                                                                                           : TIP_IN_KEPT;
    for (uint32_t i = 0; i < nleaf;) {
        if (fate[i] != TIP_IN_LAID) {
            i++;
            continue;
        }
        uint32_t end = i;
        while (end + 1u < nleaf && fate[end + 1u] == TIP_IN_LAID && Adjacent(tree, leaf, end))
            end++;
        if (end + 1u < nleaf && MarginMay(leaf, fate, end + 1u) && Siblings(tree, leaf, end, f))
            fate[end + 1u] = TIP_IN_MARGIN;
        else if (i > 0 && MarginMay(leaf, fate, i - 1u) && Siblings(tree, leaf, i - 1u, f))
            fate[i - 1u] = TIP_IN_MARGIN;
        i = end + 1u;
    }
}

// ----------------------------------------------------------------------------
// The inner nodes, and what is under the ones named as they lie
// ----------------------------------------------------------------------------

/* Whether a leaf laid or offered meets the range of `p`: the touched leaves'
 * places `touched` (`n`, in key order) halved to the first that ends past its
 * lower key. */
static bool TouchedMeet(uint64_t tree, const TallyPlace* const* touched, uint32_t n, const TallyPlace* p)
{
    uint32_t lo = 0, hi = n;
    while (p->has_lo && lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (!touched[mid]->has_hi || TipInKeyLess(tree, p->lo, touched[mid]->hi))
            hi = mid;
        else
            lo = mid + 1u;
    }
    return lo < n && PlacesMeet(tree, touched[lo], p);
}

/* Whether an inner node a copy of which did not hold lies under `p`. */
static bool RelayUnder(uint64_t tree, const TipInNode* inner, uint32_t ninner, const TallyPlace* p)
{
    for (uint32_t j = 0; j < ninner; j++)
        if (inner[j].relay && !inner[j].rotten && inner[j].place.level < p->level &&
            TipInWithin(tree, &inner[j].place, p))
            return true;
    return false;
}

static void InnerSifted(uint64_t tree, const TipInNode* inner, uint32_t ninner, const TallyPlace* const* touched,
                        uint32_t ntouched, TipInFate* fate)
{
    bool relays = false;
    for (uint32_t j = 0; j < ninner && !relays; j++)
        relays = inner[j].relay && !inner[j].rotten;
    for (uint32_t j = 0; j < ninner; j++) {
        const TipInNode* n = &inner[j];
        bool laid = !n->rotten && (n->relay || TouchedMeet(tree, touched, ntouched, &n->place) ||
                                   (relays && RelayUnder(tree, inner, ninner, &n->place)));
        fate[j] = laid ? TIP_IN_LAID : TIP_IN_KEPT;
    }
}

/* Whether `a` goes before `b` among the kept: by lower key — none first —
 * the higher level first at one key, so a node comes before every one inside
 * it. */
static bool TopBefore(uint64_t tree, const TallyPlace* a, const TallyPlace* b)
{
    if (a->has_lo != b->has_lo)
        return !a->has_lo;
    if (a->has_lo && !KeySame(tree, a->lo, b->lo))
        return TipInKeyLess(tree, a->lo, b->lo);
    return a->level > b->level;
}

static void TopSift(uint64_t tree, const TallyPlace** t, uint32_t at, uint32_t n)
{
    for (;;) {
        uint32_t big = at, l = 2u * at + 1u, r = l + 1u;
        if (l < n && TopBefore(tree, t[big], t[l]))
            big = l;
        if (r < n && TopBefore(tree, t[big], t[r]))
            big = r;
        if (big == at)
            return;
        const TallyPlace* x = t[at];
        t[at]  = t[big];
        t[big] = x;
        at     = big;
    }
}

/* Places `t` (`n`) in key order, a node before every one inside it — a heap
 * sort, as many as a Tally's inner nodes are. */
static void PlacesOrdered(uint64_t tree, const TallyPlace** t, uint32_t n)
{
    for (uint32_t i = n / 2u; i-- > 0;)
        TopSift(tree, t, i, n);
    for (uint32_t end = n; end > 1u; end--) {
        const TallyPlace* x = t[0];
        t[0]       = t[end - 1u];
        t[end - 1u] = x;
        TopSift(tree, t, 0, end - 1u);
    }
}

/* The inner nodes kept that no other kept one holds, in key order — their
 * ranges apart from one another: `out` (room for every inner node), how many. */
static uint32_t KeptTops(uint64_t tree, const TipInNode* inner, uint32_t ninner, const TipInFate* fate,
                         const TallyPlace** out)
{
    uint32_t n = 0, tops = 0;
    for (uint32_t j = 0; j < ninner; j++)
        if (fate[j] == TIP_IN_KEPT)
            out[n++] = &inner[j].place;
    PlacesOrdered(tree, out, n);
    for (uint32_t i = 0; i < n; i++)
        if (tops == 0 || !TipInWithin(tree, out[i], out[tops - 1u]))
            out[tops++] = out[i];
    return tops;
}

/* Whether a kept node of the tops other than `p` itself holds `p`. */
static bool UnderTops(uint64_t tree, const TallyPlace* const* tops, uint32_t n, const TallyPlace* p)
{
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (!tops[mid]->has_lo || (p->has_lo && !TipInKeyLess(tree, p->lo, tops[mid]->lo)))
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo > 0 && tops[lo - 1u] != p && tops[lo - 1u]->level > p->level && TipInWithin(tree, p, tops[lo - 1u]);
}

/* The places that touch the nodes above them, in key order — `out` (room for
 * every leaf and inner node), how many: every leaf laid or offered, and every
 * node no copy of which reads that a span meets. A rotten node touches its
 * parent as a laid leaf does: the parent laid anew names it as it lies, and
 * what was born in its range is left out and said, what was born past it laid
 * past it — as a whole Tally does (binder_items.c, BinderKeptCovers). Kept
 * under a node named as it lies, no Tally that saw the change would name it.
 * The leaves the sift is handed are none of a rotten node's, so the ranges
 * stay apart from one another. */
static uint32_t Touched(uint64_t tree, const TipInNode* leaf, uint32_t nleaf, const TipInFate* leaf_fate,
                        const TipInNode* inner, uint32_t ninner, const DogEarSpan* span, uint32_t nspan,
                        const TallyPlace** out)
{
    uint32_t n = 0;
    bool     rot = false;
    for (uint32_t i = 0; i < nleaf; i++)
        if (leaf_fate[i] == TIP_IN_LAID || leaf_fate[i] == TIP_IN_MARGIN ||
            (leaf[i].rotten && LeafMet(tree, leaf, i, span, nspan)))
            out[n++] = &leaf[i].place;
    for (uint32_t j = 0; j < ninner; j++)
        if (inner[j].rotten && SpansMeet(tree, span, nspan, &inner[j].place)) {
            out[n++] = &inner[j].place;
            rot      = true;
        }
    if (rot)
        PlacesOrdered(tree, out, n);
    return n;
}

error_t TipInSift(uint64_t tree, const TipInNode* leaf, uint32_t nleaf, const TipInNode* inner, uint32_t ninner,
                  const DogEarSpan* span, uint32_t nspan, TipInFate* leaf_fate, TipInFate* inner_fate)
{
    TipInFirsts        f;
    uint32_t           most = nleaf + ninner, ntouched = 0, ntops = 0;
    const TallyPlace** list = kmalloc(sizeof(TallyPlace*) * (most ? most : 1u));
    if (!list || !FirstsOf(tree, inner, ninner, &f)) {
        kfree(list);
        return ERR_NO_MEMORY;
    }
    LeavesSifted(tree, leaf, nleaf, span, nspan, &f, leaf_fate);
    ntouched = Touched(tree, leaf, nleaf, leaf_fate, inner, ninner, span, nspan, list);
    InnerSifted(tree, inner, ninner, list, ntouched, inner_fate);
    ntops = KeptTops(tree, inner, ninner, inner_fate, list);
    for (uint32_t j = 0; j < ninner; j++)
        if (inner_fate[j] == TIP_IN_KEPT && UnderTops(tree, list, ntops, &inner[j].place))
            inner_fate[j] = TIP_IN_UNDER;
    for (uint32_t i = 0; i < nleaf; i++)
        if (leaf_fate[i] == TIP_IN_KEPT && UnderTops(tree, list, ntops, &leaf[i].place))
            leaf_fate[i] = TIP_IN_UNDER;
    kfree(f.key);
    kfree(list);
    return OK;
}
