#include "foliation.h"
#include "binder.h"

/*
 * Foliation (foliation.h): each tree's nodes in two lists — its leaves in key
 * order, and its inner nodes in the order they came: a Tally's draft a level
 * at a time from the leaves up, the mount's as its walk reads the Tally, depth
 * first — for the Tally the next one is laid against, and for the draft of
 * the Tally under way. An inner node is found by its place, never by where it
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
static bool ListPut(FoliationList* l, const TallyPlace* at)
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
    l->node[l->n++] = (FoliationNode){ .place = *at, .relay = false };
    return true;
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
                        const TallyPtr* ptr)
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
    if (!ListPut(l, &p))
        BookLost(&s_draft, s_no_memory);
}

void FoliationDraftDone(uint64_t seq, bool digests)
{
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
