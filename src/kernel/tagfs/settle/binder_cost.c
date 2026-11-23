#include "binder_internal.h"

/*
 * What the next Tally is reckoned to cost (binder.c, TallyCostBytes): laid in
 * among the nodes of the Tally before (TipIn), it lays again the leaves the
 * keys DogEar holds meet and every inner node over one of them, up to the root
 * — the dirty leaves and their path, one copy each; a leaf at what is left
 * live in it, its Sheets while a key of the state lies in its range and none
 * when none does, its path all the same: the parent that drops it is laid
 * (§16.6 п.6, binder_live.c). The leaves of FILES, TAGS and USE and every
 * inner node are the Foliation's, the leaves of DIGESTS the reads' own table
 * (tally_read.h); a tree the Tally before has no node of costs one block once
 * a key of it changed. One rule with volume-read (tools/volumeread/dog_ear.py,
 * TipInBlocks): what census holds the bound line to.
 *
 * ‼ A LOOK, NOT A HOLD. DogEar is looked at a few spans a hold of its lock, so
 * a span a read of a digest puts in meanwhile may be passed over — it is
 * counted at the next look, a stitch on. Leaves and inner nodes are counted in
 * key order, each once: every span after the one before, every node of a
 * level after the one before it.
 */

#define COST_SPANS  8u          /* spans of DogEar looked at a hold of its lock */

typedef struct {
    uint64_t tree;
    uint32_t nleaf;
    uint32_t next;                          /* the leaf past the last counted */
    uint32_t above[BINDER_LEVELS_MOST];     /* level L: the node of its run past the last counted */
    uint64_t blocks;
} CostTree;

static bool CostLeaf(TagFSVolume* vol, uint64_t tree, uint32_t i, TallyPlace* out)
{
    bool rotten;
    return tree == TALLY_DIGESTS ? TallyDigestLeafAt(vol, i, out, &rotten) : FoliationLeafAt(vol, tree, i, out);
}

/* The leaf `key` lies in: the last whose lower key is not past it — the first
 * for a key below every separator. */
static uint32_t LeafOf(TagFSVolume* vol, const CostTree* c, TallyKey key)
{
    uint32_t   lo = 1, hi = c->nleaf;
    TallyPlace p;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (CostLeaf(vol, c->tree, mid, &p) && (!p.has_lo || !TipInKeyLess(c->tree, key, p.lo)))
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo - 1u;
}

/* The node of a level's run `key` lies in, the same way. */
static uint32_t NodeOf(uint64_t tree, const FoliationNode* run, uint32_t n, TallyKey key)
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

/* The inner nodes over leaf `p` — the tree's first when `first` — counted:
 * at every level the node its first key lies in, once. */
static void PathCounted(TagFSVolume* vol, CostTree* c, const TallyPlace* p, bool first)
{
    for (uint64_t level = 1; level < BINDER_LEVELS_MOST; level++) {
        uint32_t             n   = 0;
        const FoliationNode* run = FoliationLevel(vol, c->tree, level, &n);
        if (!run)
            return;
        uint32_t at = first || !p->has_lo ? 0 : NodeOf(c->tree, run, n, p->lo);
        if (at + 1u > c->above[level]) {
            c->blocks       += run[at].place.ptr.sheets;
            c->above[level]  = at + 1u;
        }
    }
}

/* Every leaf span `s` meets, at what is left live in it, and its path,
 * counted once. */
static void SpanCounted(TagFSVolume* vol, CostTree* c, const DogEarSpan* s)
{
    TallyPlace p;
    for (uint32_t i = LeafOf(vol, c, s->lo), from = i; i < c->nleaf && CostLeaf(vol, c->tree, i, &p); i++) {
        if (i > from && p.has_lo && !TipInKeyLess(c->tree, p.lo, s->hi))
            return;
        if (i < c->next)
            continue;
        c->blocks += BinderLeafLives(vol, c->tree, &p, i == 0, false) ? p.ptr.sheets : 0;
        c->next    = i + 1u;
        PathCounted(vol, c, &p, i == 0);
    }
}

/* One tree's dirty leaves and their path, in blocks of one copy. */
static uint64_t TreeCost(TagFSVolume* vol, uint64_t tree)
{
    CostTree   c;
    DogEarSpan s[COST_SPANS];
    memset(&c, 0, sizeof(c));
    c.tree = tree;
    if (tree == TALLY_DIGESTS)
        c.nleaf = TallyDigestLeaves(vol);
    else
        (void)FoliationLeaves(vol, tree, &c.nleaf);
    for (uint32_t from = 0, got; (got = DogEarHeldSpans(vol, tree, from, s, COST_SPANS)) > 0; from += got) {
        if (c.nleaf == 0)
            return 1u;
        for (uint32_t i = 0; i < got; i++)
            SpanCounted(vol, &c, &s[i]);
    }
    return c.blocks;
}

uint64_t BinderTipInBlocks(TagFSVolume* vol)
{
    static const uint64_t trees[] = { TALLY_FILES, TALLY_TAGS, TALLY_USE, TALLY_DIGESTS };
    uint64_t              blocks  = 0;
    if (FoliationLost(vol) || DogEarHeldLost(vol))
        return UINT64_MAX;
    for (uint32_t i = 0; i < sizeof(trees) / sizeof(trees[0]); i++)
        blocks += TreeCost(vol, trees[i]);
    return blocks;
}
