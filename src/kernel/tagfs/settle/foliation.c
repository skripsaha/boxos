#include "foliation.h"
#include "binder.h"
#include "tip_in.h"

/*
 * Foliation (foliation.h): each tree's nodes in two lists — its leaves in key
 * order, and its inner nodes in the order they came: the mount's as its walk
 * reads the Tally, depth first, a Tally's draft by level and key once it is
 * done — for the Tally the next one is laid against, and for the draft of the
 * Tally under way. An inner node is found by its place, never by where it
 * stands in its list.
 */

#define FOLIATION_TREES  4u

typedef struct {
    FoliationNode* node;
    uint32_t       n;
    uint32_t       cap;
} FoliationList;

typedef struct {
    FoliationList leaf[FOLIATION_TREES];
    FoliationList inner[FOLIATION_TREES];
    bool          ordered[FOLIATION_TREES];   /* its inner nodes by level and key (FoliationLevel) */
    const char*   lost;      /* why it is not known, or NULL */
} FoliationBook;

static FoliationBook s_book;
static FoliationBook s_draft;

static const char s_no_memory[] = "memory would not keep its nodes";
static const char s_unturned[]  = "the reads of a digest were not turned to its DIGESTS — they stand on a Tally "
                                  "before it";

/* The place of `tree` among the Foliation's — UINT32_MAX for a tree no Tally
 * of this kernel lays. */
static uint32_t TreeAt(uint64_t tree)
{
    switch (tree) {
    case TALLY_FILES:
        return 0;
    case TALLY_TAGS:
        return 1;
    case TALLY_USE:
        return 2;
    case TALLY_DIGESTS:
        return 3;
    default:
        return UINT32_MAX;
    }
}

static void BookLetGo(FoliationBook* b)
{
    for (uint32_t i = 0; i < FOLIATION_TREES; i++) {
        kfree(b->leaf[i].node);
        kfree(b->inner[i].node);
    }
    memset(b, 0, sizeof(*b));
}

/* A node more onto `l`: false when the heap refuses the room. */
static bool ListPutNode(FoliationList* l, const FoliationNode* node)
{
    if (l->n == l->cap) {
        uint32_t       cap  = l->cap ? l->cap * 2u : 32u;
        FoliationNode* more = cap > l->cap ? kmalloc(sizeof(FoliationNode) * cap) : NULL;
        if (!more)
            return false;
        if (l->n)
            memcpy(more, l->node, sizeof(FoliationNode) * l->n);
        kfree(l->node);
        l->node = more;
        l->cap  = cap;
    }
    l->node[l->n++] = *node;
    return true;
}

static bool ListPut(FoliationList* l, const TallyPlace* at)
{
    return ListPutNode(l, &(FoliationNode){ .place = *at, .relay = false, .items = 0, .text = 0 });
}

/* `b` lost for `why`: everything it held let go. */
static void BookLost(FoliationBook* b, const char* why)
{
    BookLetGo(b);
    b->lost = why;
}

/* The Foliation lost: said, and a Tally demanded — once a loss. */
static void Lost(const char* why, uint64_t seq)
{
    bool said = s_book.lost != NULL;
    BookLost(&s_book, why);
    if (said)
        return;
    kprintf("[TagFS] the index of Tally %llu is not known (Foliation): %s — a Tally is demanded, and the next is "
            "written whole\n", (unsigned long long)seq, why);
    BinderDemanded();
}

void FoliationNoted(uint64_t tree, const TallyPlace* at, uint64_t seq)
{
    uint32_t t = TreeAt(tree);
    if (t == UINT32_MAX || s_book.lost || (tree == TALLY_DIGESTS && at->level == 0))
        return;
    s_book.ordered[t] = false;
    if (!ListPut(at->level == 0 ? &s_book.leaf[t] : &s_book.inner[t], at))
        Lost(s_no_memory, seq);
}

void FoliationRelay(uint64_t tree, const TallyPlace* at)
{
    uint32_t t = TreeAt(tree);
    if (t == UINT32_MAX || s_book.lost)
        return;
    FoliationList* l = at->level == 0 ? &s_book.leaf[t] : &s_book.inner[t];
    for (uint32_t i = l->n; i > 0; i--) {
        const TallyPlace* p = &l->node[i - 1u].place;
        if (p->level == at->level && p->ptr.seq == at->ptr.seq && p->ptr.copies && at->ptr.copies &&
            p->ptr.block[0] == at->ptr.block[0]) {
            l->node[i - 1u].relay = true;
            return;
        }
    }
}

void FoliationLeafHolds(uint64_t tree, uint64_t items, uint64_t text)
{
    uint32_t t = TreeAt(tree);
    if (t == UINT32_MAX || s_book.lost || s_book.leaf[t].n == 0)
        return;
    s_book.leaf[t].node[s_book.leaf[t].n - 1u].items += items;
    s_book.leaf[t].node[s_book.leaf[t].n - 1u].text  += text;
}

uint64_t FoliationLeavesHolding(uint64_t tree)
{
    uint32_t t    = TreeAt(tree);
    uint64_t held = 0;
    if (s_book.lost)
        return UINT64_MAX;
    for (uint32_t i = 0; t != UINT32_MAX && i < s_book.leaf[t].n; i++)
        held += s_book.leaf[t].node[i].items != 0;
    return held;
}

const char* FoliationLost(void)
{
    return s_book.lost;
}

bool FoliationLeafAt(uint64_t tree, uint32_t index, TallyPlace* out)
{
    uint32_t t = TreeAt(tree);
    if (t == UINT32_MAX || index >= s_book.leaf[t].n)
        return false;
    *out = s_book.leaf[t].node[index].place;
    return true;
}

const FoliationNode* FoliationLeaves(uint64_t tree, uint32_t* n)
{
    uint32_t t = TreeAt(tree);
    *n = t == UINT32_MAX || s_book.lost ? 0 : s_book.leaf[t].n;
    return *n ? s_book.leaf[t].node : NULL;
}

const FoliationNode* FoliationInner(uint64_t tree, uint32_t* n)
{
    uint32_t t = TreeAt(tree);
    *n = t == UINT32_MAX || s_book.lost ? 0 : s_book.inner[t].n;
    return *n ? s_book.inner[t].node : NULL;
}

uint64_t FoliationBlocksOlder(uint64_t seq)
{
    uint64_t blocks = 0;
    for (uint32_t t = 0; !s_book.lost && t < FOLIATION_TREES; t++) {
        for (uint32_t i = 0; i < s_book.leaf[t].n; i++)
            blocks += s_book.leaf[t].node[i].place.ptr.seq != seq ? s_book.leaf[t].node[i].place.ptr.sheets : 0;
        for (uint32_t i = 0; i < s_book.inner[t].n; i++)
            blocks += s_book.inner[t].node[i].place.ptr.seq != seq ? s_book.inner[t].node[i].place.ptr.sheets : 0;
    }
    return blocks;
}

void FoliationLetGo(void)
{
    BookLetGo(&s_book);
    BookLetGo(&s_draft);
}

// ----------------------------------------------------------------------------
// The draft of the Tally under way
// ----------------------------------------------------------------------------

void FoliationDraftBegin(void)
{
    BookLetGo(&s_draft);
}

/*
 * A node of the Tally under way, as a mount of it will place it (tally_walk.c,
 * TallyKid): below the root, from the key its parent names it by — every one
 * of a level, the first too — up to the key of the next node of its level, or
 * with no end for the last; the root, from nowhere to nowhere. A level's nodes
 * come in key order, a level at a time, so the one before is closed here.
 */
void FoliationDraftNode(uint64_t tree, uint64_t level, TallyKey key, bool root, uint64_t count,
                        const TallyPtr* ptr, uint64_t items, uint64_t text)
{
    uint32_t t = TreeAt(tree);
    if (t == UINT32_MAX || s_draft.lost || (tree == TALLY_DIGESTS && level == 0))
        return;
    FoliationList* l = level == 0 ? &s_draft.leaf[t] : &s_draft.inner[t];
    TallyPlace     p;
    memset(&p, 0, sizeof(p));
    p.ptr    = *ptr;
    p.level  = level;
    p.count  = count;
    p.lo     = key;
    p.has_lo = !root;
    if (l->n && l->node[l->n - 1u].place.level == level && !root) {
        l->node[l->n - 1u].place.hi     = key;
        l->node[l->n - 1u].place.has_hi = true;
    }
    if (!ListPutNode(l, &(FoliationNode){ .place = p, .relay = false, .items = items, .text = text }))
        BookLost(&s_draft, s_no_memory);
}

/* The first node of `run` (`n`, in key order) that may lie inside `top`: the
 * one before the first whose lower key is not below top's — a tree's first
 * node is open below — or the first of all. */
static uint32_t UnderFrom(uint64_t tree, const FoliationNode* run, uint32_t n, const TallyPlace* top)
{
    uint32_t lo = 0, hi = n;
    while (top->has_lo && lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (run[mid].place.has_lo && !TipInKeyLess(tree, run[mid].place.lo, top->lo))
            hi = mid;
        else
            lo = mid + 1u;
    }
    return lo ? lo - 1u : 0;
}

/* Whether the node at `p`, and every one after it in key order, begins past
 * `top`. */
static bool UnderPast(uint64_t tree, const TallyPlace* p, const TallyPlace* top)
{
    return top->has_hi && p->has_lo && !TipInKeyLess(tree, p->lo, top->hi);
}

/* Every node of the Foliation under `top`, by halving each level's nodes in
 * key order to the first of them — a Tally names hundreds of nodes as they
 * lie over a Foliation of a volume's, each looked at under its own range. */
void FoliationDraftUnder(uint64_t tree, const TallyPlace* top)
{
    uint32_t t = TreeAt(tree);
    if (t == UINT32_MAX || s_draft.lost)
        return;
    if (s_book.lost) {
        BookLost(&s_draft, s_book.lost);
        return;
    }
    const FoliationList* leaf = &s_book.leaf[t];
    for (uint32_t i = top->level ? UnderFrom(tree, leaf->node, leaf->n, top) : leaf->n;
         i < leaf->n && !s_draft.lost && !UnderPast(tree, &leaf->node[i].place, top); i++)
        if (TipInWithin(tree, &leaf->node[i].place, top) &&
            !ListPutNode(&s_draft.leaf[t], &(FoliationNode){ .place = leaf->node[i].place,
                                                             .items = leaf->node[i].items,
                                                             .text  = leaf->node[i].text }))
            BookLost(&s_draft, s_no_memory);
    for (uint64_t level = 1; level < top->level && !s_draft.lost; level++) {
        uint32_t             n   = 0;
        const FoliationNode* run = FoliationLevel(tree, level, &n);
        for (uint32_t i = run ? UnderFrom(tree, run, n, top) : 0;
             i < n && !s_draft.lost && !UnderPast(tree, &run[i].place, top); i++)
            if (TipInWithin(tree, &run[i].place, top) && !ListPut(&s_draft.inner[t], &run[i].place))
                BookLost(&s_draft, s_no_memory);
    }
}

/* Whether node `a` goes before `b`: by level, then by lower key — none
 * first. */
static bool NodeBefore(uint64_t tree, const FoliationNode* a, const FoliationNode* b)
{
    if (a->place.level != b->place.level)
        return a->place.level < b->place.level;
    if (a->place.has_lo != b->place.has_lo)
        return !a->place.has_lo;
    return a->place.has_lo && TipInKeyLess(tree, a->place.lo, b->place.lo);
}

static void NodeSift(uint64_t tree, FoliationNode* n, uint32_t at, uint32_t count)
{
    for (;;) {
        uint32_t big = at, l = 2u * at + 1u, r = l + 1u;
        if (l < count && NodeBefore(tree, &n[big], &n[l]))
            big = l;
        if (r < count && NodeBefore(tree, &n[big], &n[r]))
            big = r;
        if (big == at)
            return;
        FoliationNode x = n[at];
        n[at]  = n[big];
        n[big] = x;
        at     = big;
    }
}

/* The first node of `n` (`count`, in key order) whose lower key lies past
 * `key`: *out, false for none. */
static bool FirstPast(uint64_t tree, const FoliationNode* n, uint32_t count, TallyKey key, TallyKey* out)
{
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (!n[mid].place.has_lo || !TipInKeyLess(tree, key, n[mid].place.lo))
            lo = mid + 1u;
        else
            hi = mid;
    }
    if (lo == count)
        return false;
    *out = n[lo].place.lo;
    return true;
}

/* Node `p` of `level` closed: its upper key the least lower key past its own
 * among the nodes of its level and every level above — `levels`, the inner
 * nodes' runs of one level each — as its parent's next separator gives it to
 * a mount. A node a Tally named as it lay, and those under it, came apart
 * from the walk; a rotten node's children came not at all. */
static void PlaceClosed(uint64_t tree, TallyPlace* p, const FoliationNode* own, uint32_t nown,
                        const FoliationNode* inner, uint32_t ninner)
{
    TallyKey best = { 0, 0 }, k;
    bool     has  = FirstPast(tree, own, nown, p->lo, &best);
    for (uint32_t i = 0; i < ninner;) {
        uint32_t end = i;
        while (end < ninner && inner[end].place.level == inner[i].place.level)
            end++;
        if (inner[i].place.level > p->level && FirstPast(tree, inner + i, end - i, p->lo, &k) &&
            (!has || TipInKeyLess(tree, k, best))) {
            best = k;
            has  = true;
        }
        i = end;
    }
    p->hi     = has ? best : (TallyKey){ 0, 0 };
    p->has_hi = has;
}

/* A list of a tree's nodes sorted by level and key — a heap sort, in place,
 * n log n whatever order they came in: the inner nodes, and a draft's leaves,
 * which a Tally laid in puts down a run at a time — the leaves it laid, then
 * those under each node it named as it lay, a level of the walk after the
 * other — each run in order, the runs in none. */
static void NodesOrdered(uint64_t tree, FoliationList* l)
{
    for (uint32_t i = l->n / 2u; i-- > 0;)
        NodeSift(tree, l->node, i, l->n);
    for (uint32_t end = l->n; end > 1u; end--) {
        FoliationNode x = l->node[0];
        l->node[0]       = l->node[end - 1u];
        l->node[end - 1u] = x;
        NodeSift(tree, l->node, 0, end - 1u);
    }
}

/* Every node of a draft's tree closed (PlaceClosed): the inner nodes sorted by
 * level and key first. */
static void PlacesClosed(uint64_t tree, FoliationList* leaf, FoliationList* inner)
{
    NodesOrdered(tree, inner);
    for (uint32_t i = 0; i < leaf->n; i++)
        if (leaf->node[i].place.has_lo)
            PlaceClosed(tree, &leaf->node[i].place, leaf->node, leaf->n, inner->node, inner->n);
    for (uint32_t i = 0; i < inner->n;) {
        uint32_t end = i;
        while (end < inner->n && inner->node[end].place.level == inner->node[i].place.level)
            end++;
        for (uint32_t j = i; j < end; j++)
            if (inner->node[j].place.has_lo)
                PlaceClosed(tree, &inner->node[j].place, inner->node + i, end - i, inner->node, inner->n);
        i = end;
    }
}

static const uint64_t s_trees[FOLIATION_TREES] = { TALLY_FILES, TALLY_TAGS, TALLY_USE, TALLY_DIGESTS };

void FoliationDraftDone(uint64_t seq, bool digests)
{
    for (uint32_t t = 0; !s_draft.lost && t < FOLIATION_TREES; t++) {
        NodesOrdered(s_trees[t], &s_draft.leaf[t]);
        PlacesClosed(s_trees[t], &s_draft.leaf[t], &s_draft.inner[t]);
        s_draft.ordered[t] = true;
    }
    if (!s_draft.lost && !digests)
        BookLost(&s_draft, s_unturned);
    if (s_draft.lost) {
        const char* why = s_draft.lost;
        BookLetGo(&s_draft);
        Lost(why, seq);
        return;
    }
    BookLetGo(&s_book);
    s_book = s_draft;
    memset(&s_draft, 0, sizeof(s_draft));
}

void FoliationDraftLetGo(void)
{
    BookLetGo(&s_draft);
}

/* The mount's inner nodes come depth first, as its walk reaches them; a level's
 * own lie in key order among them, but apart. Sorted the first time a level is
 * asked for, they lie a level a run — a draft's already do. */
const FoliationNode* FoliationLevel(uint64_t tree, uint64_t level, uint32_t* n)
{
    uint32_t t = TreeAt(tree);
    *n = 0;
    if (t == UINT32_MAX || s_book.lost || level == 0)
        return NULL;
    FoliationList* l = &s_book.inner[t];
    if (!s_book.ordered[t]) {
        NodesOrdered(tree, l);
        s_book.ordered[t] = true;
    }
    uint32_t lo = 0, hi = l->n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (l->node[mid].place.level < level)
            lo = mid + 1u;
        else
            hi = mid;
    }
    uint32_t end = lo;
    while (end < l->n && l->node[end].place.level == level)
        end++;
    *n = end - lo;
    return *n ? l->node + lo : NULL;
}

#ifdef CONFIG_VOLUME_TESTS
bool FoliationStandNode(uint64_t tree, uint32_t index, FoliationNode* out)
{
    uint32_t t = TreeAt(tree);
    if (t == UINT32_MAX)
        return false;
    if (index < s_book.leaf[t].n) {
        *out = s_book.leaf[t].node[index];
        return true;
    }
    index -= s_book.leaf[t].n;
    if (index >= s_book.inner[t].n)
        return false;
    *out = s_book.inner[t].node[index];
    return true;
}
#endif
