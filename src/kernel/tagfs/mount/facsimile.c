#include "facsimile.h"
#include "book_mount.h"
#include "ribbon.h"
#include "../settle/dog_ear.h"

/*
 * Facsimile (facsimile.h). The walk of the Tally stood on notes each rotten
 * range of FILES and TAGS and sets aside each file one reaches; once it is
 * done the ranges are weighed against the slots before, newest first — TAGS
 * first, so a FILES range replayed knows every tag a TAGS range gave back:
 *
 *   1. every authoritative root of the slot reads (BookSlotReads) — the slot
 *      is one a mount could stand on (bookmount.py: its mount not refused);
 *   2. its tree of the range, walked by its inner nodes alone, names the
 *      leaves the range's keys reach, and none of them, nor a node above
 *      them, reads in no copy — a node shared with the Tally stood on rots
 *      for both, and the slot is passed over;
 *   3. its ribbon from its `at` reaches the stitch the Tally stood on took in
 *      — or the slot's Tally took in that very stitch;
 *   4. every file the range reaches stands in the slot's Tally, and its
 *      ribbon to that stitch is replayed for the range's keys whole — no
 *      stitch of them refused, no Undone that would not hold.
 *
 * Rules 1–3 are weighed before anything is laid in; rule 4 is known only by
 * laying the range in. The first slot that holds gives the range back: the
 * elements of those leaves of the range's keys — read once, by rule 2's walk,
 * and kept — laid into memory as the walk lays the Tally stood on's, then the
 * ribbon's records of those keys alone replayed to that stitch
 * (RibbonFacsimile). For FILES the keys are whole files — every fid the range
 * reaches into, its header or a Parcel of it inside the range — as the files
 * set aside are; two rotten FILES ranges that reach into one fid are one
 * range to Facsimile, given back whole or not at all, so no file is laid in
 * from one side while the other side still names it rotten. A FILES range
 * rule 4 fails is laid out again (BookFacsimileUndone) and the next slot
 * weighed; of TAGS what the take coined cannot be laid out, so the volume
 * goes under glass, as the mount's own replay does for a refused stitch
 * (§16.1 п.2). A range no slot gives back is said and locks what it always
 * did (BookRangeRotsLaid, BookAsideSettled).
 *
 * Only the leaves the range reaches are read: an older slot's inner nodes are
 * few, and the mount reads every one of them for its ground anyway
 * (book_ground.c, BookGroundOlder).
 */

typedef struct {
    uint64_t   tree;
    TallyPlace at;
    bool       taken;
    char       why[160];
} FacsimileRange;

static FacsimileRange* s_ranges;
static uint32_t        s_nranges;
static uint32_t        s_capranges;

/* Ranges first..last of one tree whose keys reach into each other, and the
 * keys [lo, hi) of k1 they reach together (UINT64_MAX: to the tree's end). */
typedef struct {
    uint64_t tree;
    uint32_t first;
    uint32_t last;
    uint64_t lo;
    uint64_t hi;
} FacsimileGroup;

/* An element of the group's keys as a leaf of the older slot held it,
 * followed by its body. */
typedef struct {
    TallyKey key;
    uint64_t seq;
    uint32_t len;
    uint32_t pad;
} FacsimileHeld;

/* The leaves of an older slot's tree the keys [k1 lo, k1 hi) reach, by
 * place; whether a node they reach reads in no copy; their elements of those
 * keys, kept as read, and counted. */
typedef struct {
    uint64_t    tree;
    uint64_t    lo;
    uint64_t    hi;
    TallyPlace* leaf;
    uint32_t    nleaf;
    uint32_t    capleaf;
    bool        rotten;
    uint32_t    items;
    uint8_t*    held;
    uint64_t    nheld;
    uint64_t    capheld;
} FacsimileLook;

error_t FacsimileRotNoted(uint64_t tree, const TallyPlace* at, const char* why)
{
    if (s_nranges == s_capranges) {
        uint32_t        cap  = s_capranges ? s_capranges * 2u : 4u;
        FacsimileRange* more = kmalloc(sizeof(FacsimileRange) * cap);
        if (!more)
            return ERR_NO_MEMORY;
        if (s_nranges)
            memcpy(more, s_ranges, sizeof(FacsimileRange) * s_nranges);
        kfree(s_ranges);
        s_ranges    = more;
        s_capranges = cap;
    }
    FacsimileRange* r = &s_ranges[s_nranges++];
    r->tree  = tree;
    r->at    = *at;
    r->taken = false;
    ksnprintf(r->why, sizeof(r->why), "%s", why);
    return OK;
}

/* The first keys a range does not reach, by their k1: of FILES, every fid
 * it reaches into — the one its end lies inside too. */
static void RangeKeys(const FacsimileRange* r, uint64_t* lo, uint64_t* hi)
{
    *lo = r->at.has_lo ? r->at.lo.k1 : 0;
    if (!r->at.has_hi)
        *hi = UINT64_MAX;
    else
        *hi = r->tree == TALLY_FILES && r->at.hi.k2 > 0 ? r->at.hi.k1 + 1u : r->at.hi.k1;
}

/* The group that begins at range `first`: the walk noted a tree's ranges one
 * after another in key order, so ranges that reach into each other stand
 * side by side. The index past it. */
static uint32_t GroupFrom(uint32_t first, FacsimileGroup* g)
{
    g->tree  = s_ranges[first].tree;
    g->first = g->last = first;
    RangeKeys(&s_ranges[first], &g->lo, &g->hi);
    for (uint32_t j = first + 1u; j < s_nranges && s_ranges[j].tree == g->tree; j++) {
        uint64_t lo, hi;
        RangeKeys(&s_ranges[j], &lo, &hi);
        if (lo >= g->hi)
            break;
        g->last = j;
        g->hi   = hi > g->hi ? hi : g->hi;
    }
    return g->last + 1u;
}

/* Whether the place's keys meet [l->lo, l->hi) of k1. */
static bool LookMeets(const FacsimileLook* l, const TallyPlace* p)
{
    bool below = !p->has_lo || l->hi == UINT64_MAX || p->lo.k1 < l->hi;
    bool above = !p->has_hi || l->lo < p->hi.k1 || (l->lo == p->hi.k1 && p->hi.k2 > 0);
    return below && above;
}

static error_t LookNode(void* ctx, const TallyPlace* at)
{
    FacsimileLook* l = ctx;
    if (at->level != 0 || !LookMeets(l, at))
        return OK;
    if (l->nleaf == l->capleaf) {
        uint32_t    cap  = l->capleaf ? l->capleaf * 2u : 8u;
        TallyPlace* more = kmalloc(sizeof(TallyPlace) * cap);
        if (!more)
            return ERR_NO_MEMORY;
        if (l->nleaf)
            memcpy(more, l->leaf, sizeof(TallyPlace) * l->nleaf);
        kfree(l->leaf);
        l->leaf    = more;
        l->capleaf = cap;
    }
    l->leaf[l->nleaf++] = *at;
    return OK;
}

static error_t LookRotten(void* ctx, const TallyPlace* at, const char* why)
{
    FacsimileLook* l = ctx;
    (void)why;
    l->rotten |= LookMeets(l, at);
    return OK;
}

/* The room an element takes in the look's list: its body after it, the next
 * one aligned. */
static uint64_t HeldRoom(uint32_t len)
{
    return (sizeof(FacsimileHeld) + (uint64_t)len + 7u) & ~(uint64_t)7u;
}

/* An element of the keys kept as the leaf held it — the take lays it in
 * from here, and reads no leaf again. */
static error_t LookItem(void* ctx, TallyKey key, const uint8_t* body, uint32_t len, uint64_t seq)
{
    FacsimileLook* l = ctx;
    if (key.k1 < l->lo || (l->hi != UINT64_MAX && key.k1 >= l->hi))
        return OK;
    l->items += l->tree != TALLY_FILES || key.k2 == 0;
    uint64_t room = HeldRoom(len);
    if (l->nheld + room > l->capheld) {
        uint64_t cap  = l->capheld ? l->capheld * 2u : 4096u;
        cap           = cap < l->nheld + room ? l->nheld + room : cap;
        uint8_t* more = kmalloc(cap);
        if (!more)
            return ERR_NO_MEMORY;
        if (l->nheld)
            memcpy(more, l->held, l->nheld);
        kfree(l->held);
        l->held    = more;
        l->capheld = cap;
    }
    FacsimileHeld* h = (FacsimileHeld*)(l->held + l->nheld);
    *h = (FacsimileHeld){ .key = key, .seq = seq, .len = len };
    memcpy(h + 1, body, len);
    l->nheld += room;
    return OK;
}

/* The root of `tree` slot `s` names, or NULL: the tree is empty there. */
static const SpineRoot* RootOf(const SpineSlot* s, uint64_t tree)
{
    for (uint32_t i = 0; i < s->roots; i++)
        if (s->root[i].tree == tree)
            return &s->root[i];
    return NULL;
}

/* Rules 1 and 2 of slot `s` for the keys `l` looks at: *holds, with the
 * elements of those keys kept in `l`. OK, or memory's or the medium's
 * refusal. */
static error_t LookHolds(const SpineSlot* s, FacsimileLook* l, bool* holds)
{
    char             why[256];
    const SpineRoot* root = RootOf(s, l->tree);
    TallySink        walk = { .ctx = l, .rotten = LookRotten, .node = LookNode };
    TallySink        leaf = { .ctx = l, .item = LookItem, .rotten = LookRotten };
    error_t          rc   = BookSlotReads(s, why, sizeof(why));
    *holds = false;
    if (rc == ERR_TAGFS_CORRUPTED)
        return OK;
    if (rc == OK && root)
        rc = TallyRead(l->tree, root, false, &walk);
    for (uint32_t i = 0; rc == OK && !l->rotten && i < l->nleaf; i++)
        rc = TallyLeafRead(l->tree, &l->leaf[i], &leaf);
    *holds = rc == OK && !l->rotten;
    return rc;
}

/* Whether the ribbon walked from an older slot reaches stitch `k`. */
static bool RibbonReaches(const RibbonWalk* w, uint64_t k)
{
    for (uint32_t i = 0; i < w->nstitch; i++)
        if (w->stitch[i]->seq == k)
            return true;
    return false;
}

/* The elements the look kept laid into memory, in their order. */
static error_t LookLaid(const FacsimileLook* l)
{
    error_t rc = OK;
    for (uint64_t at = 0; rc == OK && at < l->nheld;) {
        const FacsimileHeld* h = (const FacsimileHeld*)(l->held + at);
        rc = BookFacsimileItem(l->tree, h->key, (const uint8_t*)(h + 1), h->len, h->seq);
        at += HeldRoom(h->len);
    }
    return rc == OK && l->tree == TALLY_FILES ? BookFacsimileClose() : rc;
}

/* Group `g` laid in from slot `s` failed rule 4 for `why`: a FILES group laid
 * out again, the next slot weighed; a TAGS group under glass (above). */
static void FacsimileUnfit(const FacsimileGroup* g, const SpineSlot* s, const char* why, bool* done)
{
    char words[384], hi[24];
    if (g->hi == UINT64_MAX)
        ksnprintf(hi, sizeof(hi), "the end");
    else
        ksnprintf(hi, sizeof(hi), "%llu", (unsigned long long)g->hi);
    if (g->tree == TALLY_FILES) {
        BookFacsimileUndone(g->lo, g->hi);
        for (uint32_t i = g->first; i <= g->last; i++)
            BookRangeBack(&s_ranges[i].at);
        kprintf("[TagFS] slot %u (seq %llu) does not stand in for the rotten FILES keys [%llu, %s): %s — laid out "
                "again\n", s->index, (unsigned long long)s->head.tally_seq, (unsigned long long)g->lo, hi, why);
        return;
    }
    ksnprintf(words, sizeof(words), "the rotten %s keys [%llu, %s) taken from slot %u: %s — what the take coined "
              "is not laid out again, and no Tally is written of a state that is not the one the Tally stood on "
              "holds", TallyTreeName(g->tree), (unsigned long long)g->lo, hi, s->index, why);
    TagFSUnderGlass(words);
    *done = true;
}

/* Group `g` given back by slot `s`, whose look is `l`, its ribbon `w` (NULL:
 * the slot's Tally took in stitch `k` itself) — or rule 4 found against it.
 * *done once the group is settled. */
static error_t FacsimileTake(const FacsimileGroup* g, const SpineSlot* s, FacsimileLook* l, RibbonWalk* w,
                             uint64_t k, bool* done)
{
    char     why[192] = "";
    uint32_t replayed = 0;
    bool     whole    = true;
    error_t  rc;
    for (uint32_t i = g->first; g->tree == TALLY_FILES && i <= g->last; i++)
        BookRangeTaken(&s_ranges[i].at);    /* no record the replay applies is locked by it */
    rc = LookLaid(l);
    if (rc == OK && !(g->tree == TALLY_FILES && BookFacsimileUnfit(why, sizeof(why))) && w) {
        rc = RibbonFacsimile(w, g->tree, l->lo, l->hi, k, &replayed, &whole);
        if (rc == OK && !whole)
            ksnprintf(why, sizeof(why), "its ribbon to stitch %llu passes over a stitch of these keys — refused, "
                      "or its Undone would not hold", (unsigned long long)k);
    }
    if (rc != OK)
        return rc;
    if (why[0]) {
        FacsimileUnfit(g, s, why, done);
        return OK;
    }
    DogEarPut(g->tree, (TallyKey){ g->lo, 0 }, g->hi == UINT64_MAX ? DOG_EAR_END : (TallyKey){ g->hi, 0 });
    for (uint32_t i = g->first; i <= g->last; i++) {
        FacsimileRange* r = &s_ranges[i];
        char            hi[24];
        r->taken = true;
        if (r->at.has_hi)
            ksnprintf(hi, sizeof(hi), "%llu", (unsigned long long)r->at.hi.k1);
        else
            ksnprintf(hi, sizeof(hi), "the end");
        kprintf("[TagFS] the %s range [%llu, %s) rots in every copy (%s): read from slot %u's Tally (seq %llu) and "
                "its ribbon replayed for its keys — %u element(s), %u stitch(es) (Facsimile); the next Tally lays it "
                "anew\n", TallyTreeName(r->tree), (unsigned long long)(r->at.has_lo ? r->at.lo.k1 : 0), hi, r->why,
                s->index, (unsigned long long)s->head.tally_seq, l->items, replayed);
    }
    *done = true;
    return OK;
}

/* Group `g` weighed against slot `s`: given back when the slot holds for it.
 * OK, or memory's or the medium's refusal. */
static error_t FacsimileFrom(const FacsimileGroup* g, const SpineSlot* s, bool* done)
{
    uint64_t      k     = g_book.stood->head.stitch_seq;
    FacsimileLook l     = { .tree = g->tree, .lo = g->lo, .hi = g->hi };
    RibbonWalk*   w     = NULL;
    bool          holds = false;
    error_t       rc    = LookHolds(s, &l, &holds);
    if (rc == OK && holds && s->head.stitch_seq != k) {
        rc    = RibbonCollateOlder(s, &w);
        holds = rc == OK && RibbonReaches(w, k);
    }
    if (rc == OK && holds)
        rc = FacsimileTake(g, s, &l, w, k, done);
    RibbonWalkLetGo(w);
    kfree(l.leaf);
    kfree(l.held);
    return rc;
}

/* Group `g` weighed against every slot before the one stood on, newest
 * first, until one gives it back. */
static error_t FacsimileGroupWeighed(const FacsimileGroup* g)
{
    const SpineSlot* stood = g_book.stood;
    bool             done  = false;
    error_t          rc    = OK;
    for (uint32_t i = 0; rc == OK && !done && i < g_book.nslots; i++) {
        const SpineSlot* s = &g_book.slots[i];
        if (s->seq < stood->seq && s->head.stitch_seq <= stood->head.stitch_seq)
            rc = FacsimileFrom(g, s, &done);
    }
    return rc;
}

/* Whether a FILES range given back reaches into `fid`. */
static bool FacsimileTakes(uint64_t fid)
{
    for (uint32_t i = 0; i < s_nranges; i++) {
        uint64_t lo, hi;
        if (s_ranges[i].tree != TALLY_FILES || !s_ranges[i].taken)
            continue;
        RangeKeys(&s_ranges[i], &lo, &hi);
        if (fid >= lo && fid < hi)
            return true;
    }
    return false;
}

error_t MountFacsimile(const char** what)
{
    static const uint64_t order[] = { TALLY_TAGS, TALLY_FILES };
    error_t               rc      = OK;
    for (uint32_t t = 0; t < 2u; t++)
        for (uint32_t i = 0; rc == OK && i < s_nranges;) {
            FacsimileGroup g;
            uint32_t       next = GroupFrom(i, &g);
            if (g.tree == order[t])
                rc = FacsimileGroupWeighed(&g);
            i = next;
        }
    if (rc == OK)
        rc = BookAsideSettled(FacsimileTakes);
    for (uint32_t i = 0; rc == OK && i < s_nranges; i++) {
        const FacsimileRange* r = &s_ranges[i];
        if (!r->taken)
            rc = r->tree == TALLY_FILES ? BookRangeRotsLaid(&r->at, r->why) : BookTagsRotLaid(r->tree, &r->at, r->why);
    }
    if (rc != OK)
        *what = rc == ERR_NO_MEMORY ? "there is no memory to weigh its rotten ranges against the slots before"
                                    : "its medium would not give up the Tally of a slot before";
    return rc;
}

void FacsimileLetGo(void)
{
    BookAsideLetGo();
    kfree(s_ranges);
    s_ranges  = NULL;
    s_nranges = s_capranges = 0;
}
