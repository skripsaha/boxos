#include "binder_internal.h"
#include "escheat.h"

/*
 * The nodes a Tally names as they lie (binder.h, BinderKept): TipIn's sifting
 * of the Tally before into them at the slice (tip_in.h), and the walk's look
 * at them as it lays a tree a level at a time (binder_nodes.c,
 * binder_items.c).
 *
 * ‼ THE SLICE IS WHAT CHANGED, NOT EVERY FILE (§4.3 TALLY_TAKE, H-5). A Tally
 * laid in takes the records of the files whose keys lie in the leaves it lays
 * or offers a row — the files DogEar names and their leaves' neighbours —
 * never a walk of every live file; every other key of FILES, TAGS, USE and
 * DIGESTS lies under a node it names as it lies. A Tally is written whole when there
 * is nothing known to lay it in against: the Foliation lost, or DogEar.
 *
 * ‼ ONE PASS FORWARD A LEVEL (spec_v2 §16.4 п.5, map tally §2). The nodes
 * named as they lie are kept in the order of tree and key, and a tree's walk
 * meets them in that order at every level: three cursors — the next one of the
 * level below for the level above, the first not behind the node being
 * filled, and at the leaves the first whose range ends past the last key —
 * only go forward, so a Tally of thousands of them looks at each a few times
 * a level, never once an element.
 */

// ----------------------------------------------------------------------------
// TipIn at the slice
// ----------------------------------------------------------------------------

static const BinderFids s_every = { 0, UINT64_MAX };

/* A run of file ids more for the slice — joined with the one before when they
 * meet. */
static error_t FidsPut(BinderRun* r, uint64_t from, uint64_t to)
{
    if (r->nfids && from <= r->fids[r->nfids - 1u].to) {
        if (to > r->fids[r->nfids - 1u].to)
            r->fids[r->nfids - 1u].to = to;
        return OK;
    }
    if (!BinderListRoom32((void**)&r->fids, r->nfids, &r->capfids, sizeof(BinderFids)))
        return ERR_NO_MEMORY;
    r->fids[r->nfids++] = (BinderFids){ from, to };
    return OK;
}

/* The file ids a leaf of FILES holds keys of: from its lower key's — the
 * tree's first leaf from none — to its upper key's, that file too when the
 * leaf ends inside it. */
static error_t LeafFids(BinderRun* r, const TallyPlace* p, bool first)
{
    uint64_t from = p->has_lo && !first ? p->lo.k1 : 0;
    uint64_t to   = !p->has_hi ? UINT64_MAX : p->hi.k2 && p->hi.k1 != UINT64_MAX ? p->hi.k1 + 1u : p->hi.k1;
    return FidsPut(r, from, to);
}

/* The sift's fates put in the run: every node it keeps named as it lies, and
 * the file ids of every leaf of FILES it lays or offers sliced, in order — and
 * of a rotten node it names, whose parent is laid anew: no file of its range
 * is in the state, and one born past it is laid past it (BinderKeptCovers).
 * A rotten inner node's leaves are none of the sift's, so only the last one,
 * past every leaf, has a file past it to slice. */
error_t BinderSiftTaken(BinderRun* r, const BinderSift* s)
{
    uint32_t             nf = 0;
    const FoliationNode* fl = s->tree == TALLY_DIGESTS ? NULL : FoliationLeaves(s->tree, &nf);
    error_t              rc = OK;
    if (s->n == 0 && s->ninner == 0)
        return s->tree == TALLY_FILES ? FidsPut(r, 0, UINT64_MAX) : OK;
    for (uint32_t i = 0; rc == OK && i < s->n; i++) {
        const FoliationNode* f = fl && s->at[i] < nf ? &fl[s->at[i]] : NULL;
        if (s->lf[i] == TIP_IN_KEPT || s->lf[i] == TIP_IN_MARGIN)
            rc = BinderKeptPut(r, &(BinderKept){ .tree = s->tree, .at = s->leaf[i].place,
                                                 .rotten = s->leaf[i].rotten, .margin = s->lf[i] == TIP_IN_MARGIN,
                                                 .items = f ? f->items : 0, .text = f ? f->text : 0 });
        if (rc == OK && s->tree == TALLY_FILES &&
            (s->lf[i] == TIP_IN_LAID || s->lf[i] == TIP_IN_MARGIN || (s->lf[i] == TIP_IN_KEPT && s->leaf[i].rotten)))
            rc = LeafFids(r, &s->leaf[i].place, s->at[i] == 0);
    }
    for (uint32_t j = 0; rc == OK && j < s->ninner; j++) {
        if (s->inf[j] != TIP_IN_KEPT)
            continue;
        rc = BinderKeptPut(r, &(BinderKept){ .tree = s->tree, .at = s->inner[j].place,
                                             .rotten = s->inner[j].rotten });
        if (rc == OK && s->tree == TALLY_FILES && s->inner[j].rotten && !s->inner[j].place.has_hi)
            rc = LeafFids(r, &s->inner[j].place, false);
    }
    return rc;
}

/* The Tally written whole: every file, and only the nodes no copy of which
 * reads named as they lie. */
static error_t WholeTaken(BinderRun* r)
{
    r->w.tip = false;
    r->nkept = 0;
    r->nfids = 0;
    BinderDigestsLetGo(r);
    error_t rc = BinderRotsTaken(r);
    return rc == OK ? FidsPut(r, s_every.from, s_every.to) : rc;
}

/*
 * ‼ A TALLY LAID IN THAT WOULD COST A WHOLE ONE IS WRITTEN WHOLE (§16.5 п.16).
 * Both measures counted dry, before anything is sliced: the nodes laying it
 * in lays and offers, at the size they had, against every node of the trees
 * it sifts. One no cheaper is no Tally laid in — a whole one names nothing as
 * it lies, and its Tally-Quires go back whole once the slots stop naming them.
 * DIGESTS is sifted here and held: the leaves it lays are read a turn each
 * before it is known which of them read (binder_digests.c).
 */
error_t BinderTipInTake(BinderRun* r)
{
    static const uint64_t trees[] = { TALLY_FILES, TALLY_TAGS, TALLY_USE, TALLY_DIGESTS };
    TipInCount count = { 0, 0 };
    error_t    rc    = OK;
    if (FoliationLost() || DogEarTakenLost() || EscheatTaken())
        return WholeTaken(r);
    r->w.tip = true;
    for (uint32_t i = 0; rc == OK && i < sizeof(trees) / sizeof(trees[0]); i++) {
        BinderSift  one;
        BinderSift* s = trees[i] == TALLY_DIGESTS ? &r->digests : &one;
        rc = BinderSiftTree(s, trees[i]);
        if (rc == OK)
            BinderSiftCount(s, r->w.grid, &count);
        if (rc == OK && s == &one)
            rc = BinderSiftTaken(r, s);
        if (s == &one)
            BinderSiftLetGo(s);
    }
    if (rc == OK && count.whole && count.tip >= count.whole)
        return WholeTaken(r);
#if BINDER_CHECKS
    /* The self-check holds every element of the state to the Tally before:
     * its slice is every file (binder_check.c). */
    r->nfids = 0;
    if (rc == OK)
        rc = FidsPut(r, s_every.from, s_every.to);
#endif
    if (rc == ERR_NO_MEMORY)
        kprintf("[TagFS] no memory to lay the Tally of the state after stitch %llu in among the nodes of the one "
                "before it — it is refused, and the next is tried a cycle on\n", (unsigned long long)r->k);
    return rc;
}

// ----------------------------------------------------------------------------
// The walk's look at the nodes named as they lie
// ----------------------------------------------------------------------------

void BinderKeptTreeBegin(BinderWork* w)
{
    uint32_t i = 0;
    while (i < w->nkept && w->kept[i].tree < w->tree)
        i++;
    w->tree_kept = i;
    while (i < w->nkept && w->kept[i].tree == w->tree)
        i++;
    w->tree_kept_end = i;
    BinderKeptLevelBegin(w);
}

void BinderKeptLevelBegin(BinderWork* w)
{
    w->keyed    = false;
    w->kept_at  = w->tree_kept;
    w->bar_at   = w->tree_kept;
    w->cover_at = w->tree_kept;
}

BinderKept* BinderKeptAt(BinderWork* w, TallyKey key)
{
    while (w->cover_at < w->tree_kept_end) {
        const BinderKept* k = &w->kept[w->cover_at];
        if (!k->at.has_hi || BinderKeyLess(w->tree, key, k->at.hi))
            break;
        w->cover_at++;
    }
    if (w->cover_at == w->tree_kept_end)
        return NULL;
    BinderKept* k = &w->kept[w->cover_at];
    return !k->taken && BinderKeptCovers(k, key) ? k : NULL;
}

const BinderKept* BinderKeptBars(BinderWork* w, TallyKey key)
{
    if (!w->keyed)
        return NULL;
    while (w->bar_at < w->tree_kept_end && (!w->kept[w->bar_at].at.has_lo ||
                                            !BinderKeyLess(w->tree, w->fill.last, w->kept[w->bar_at].at.lo)))
        w->bar_at++;
    const BinderKept* bar = NULL;
    for (uint32_t i = w->bar_at; i < w->tree_kept_end && !BinderKeyLess(w->tree, key, w->kept[i].at.lo); i++)
        if (w->kept[i].at.level >= w->level && !w->kept[i].taken)
            bar = &w->kept[i];
    return bar;
}

bool BinderKeptAbove(const BinderWork* w, uint64_t level)
{
    for (uint32_t i = w->tree_kept; i < w->tree_kept_end; i++)
        if (w->kept[i].at.level >= level && !w->kept[i].taken)
            return true;
    return false;
}

/* A leaf of the Tally before named as it lies, as the reserve's measure
 * counts a leaf a Tally lays (endpaper_fact.h). */
static void KeptLeafLaid(BinderWork* w, uint64_t tree, const FoliationNode* leaf)
{
    EndpaperLaidNode(&w->laid, tree, 0, leaf->place.ptr.sheets, w->grid,
                     leaf->place.has_lo ? leaf->place.lo.k1 : 0);
    if (tree == TALLY_FILES) {
        w->laid.runs       += leaf->items;
        w->laid.run_leaves += leaf->items != 0;
    }
    if (tree == TALLY_TAGS) {
        w->laid.tags      += leaf->items;
        w->laid.tags_text += leaf->text;
    }
}

/* The one of the tree's nodes named as it lies, from `*j` on (`end`), whose
 * range may hold `leaf`: the last whose lower key is not past the leaf's. */
static const BinderKept* KeptOver(const BinderWork* w, uint64_t tree, const FoliationNode* leaf, uint32_t* j,
                                  uint32_t end)
{
    while (leaf->place.has_lo && *j + 1u < end && w->kept[*j + 1u].at.has_lo &&
           !TipInKeyLess(tree, leaf->place.lo, w->kept[*j + 1u].at.lo))
        (*j)++;
    return *j < end ? &w->kept[*j] : NULL;
}

/*
 * ‼ THE RESERVE MEASURES THE WHOLE BOOK A TALLY NAMES (§16.4 п.7, §16.5 п.17):
 * Tn is every live node of the last Tally, its own and the ones it names as
 * they lie. So the walk counts each leaf of FILES and TAGS under a node it
 * names as it lies — as Foliation holds it, what it holds besides — and each
 * of DIGESTS as the reads' table holds it (binder_digests.c), as the leaves it
 * lays are counted; a rotten node, whose leaves nobody knows, leaves its tree
 * to the measure from above.
 */
void BinderKeptLaid(BinderWork* w)
{
    static const uint64_t trees[] = { TALLY_FILES, TALLY_TAGS };
    for (uint32_t i = 0; i < w->nkept; i++)
        if (!w->kept[i].taken && w->kept[i].rotten)
            EndpaperLaidNode(&w->laid, w->kept[i].tree, w->kept[i].at.level, 0, w->grid,
                             w->kept[i].at.has_lo ? w->kept[i].at.lo.k1 : 0);
    for (uint32_t t = 0; t < sizeof(trees) / sizeof(trees[0]); t++) {
        uint32_t begin = 0, end, n = 0;
        while (begin < w->nkept && w->kept[begin].tree < trees[t])
            begin++;
        for (end = begin; end < w->nkept && w->kept[end].tree == trees[t];)
            end++;
        const FoliationNode* leaf = begin < end ? FoliationLeaves(trees[t], &n) : NULL;
        for (uint32_t i = 0, j = begin; i < n; i++) {
            const BinderKept* k = KeptOver(w, trees[t], &leaf[i], &j, end);
            if (k && !k->taken && !k->rotten && TipInWithin(trees[t], &leaf[i].place, &k->at))
                KeptLeafLaid(w, trees[t], &leaf[i]);
        }
    }
    if (w->tip)
        BinderDigestsKeptLaid(w);
}

const BinderKept* BinderKeptRoot(const BinderWork* w)
{
    if (w->tree_kept_end != w->tree_kept + 1u)
        return NULL;
    const BinderKept* k = &w->kept[w->tree_kept];
    return !k->taken && !k->rotten && !k->at.has_lo && !k->at.has_hi ? k : NULL;
}
