#include "binder_internal.h"
#include "tagfs_volume.h"

/*
 * ‼ A SLOT HOLDS EVERY TALLY-QUIRE ITS TALLY NAMES A NODE IN. A Tally laid in
 * names the nodes of the Tally before as they lie — each node it keeps, and
 * every node under one — and those lie in Quires the Tallies before it took:
 * the slot that holds it holds each of those Quires whole while it holds
 * (binder_slots.c, SlotsHold), as a mount's census does
 * (mount/ground_census.c, GroundQuiresHeld), so the ring writing over the slot
 * that laid a node never gives back a Quire a Tally still names a node in. Its
 * own nodes are its own runs (binder_done.c, FreshFill).
 *
 * Made at TAKE, once every node it names as it lies is known — the FILES ones
 * the mount found rotten, the ones TipIn keeps and the DIGESTS ones weighed a
 * leaf a turn (binder.c, TallyMeasureBegin) — from the Tally before as the
 * Foliation and the reads' table hold it, so nothing is asked of the heap once
 * the slot is on the medium. A margin a row takes in is laid anew, and its
 * Quire held for nothing until the slot is written over: a leaf's worth.
 */

typedef struct {
    uint64_t* bit;          /* a Quire a node lies in, by its index on the grid */
    uint64_t  nquire;
    uint64_t  q;
} QuireMarks;

static void PlaceMarked(TagFSVolume* vol, QuireMarks* m, const TallyPlace* p)
{
    for (uint32_t k = 0; k < p->ptr.copies; k++) {
        uint64_t start = TallyPtrBlock(vol, &p->ptr, k), end = start + (p->ptr.sheets ? p->ptr.sheets : 1u);
        for (uint64_t at = start / m->q; at < m->nquire && at * m->q < end; at++)
            m->bit[at / 64u] |= 1ull << (at % 64u);
    }
}

/* Whether a node beginning at `p`'s lower key, at or past where kept node
 * `k`'s first key lies, still lies under `k`. */
static bool StillUnder(uint64_t tree, const TallyPlace* p, const BinderKept* k)
{
    return !k->at.has_hi || !p->has_lo || TipInKeyLess(tree, p->lo, k->at.hi);
}

/* The node of a level's run `k`'s first key lies in: the last whose lower key
 * is not past it — the first for none. */
static uint32_t RunAt(uint64_t tree, const FoliationNode* run, uint32_t n, const BinderKept* k)
{
    uint32_t lo = 1, hi = n;
    while (k->at.has_lo && lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (!run[mid].place.has_lo || !TipInKeyLess(tree, k->at.lo, run[mid].place.lo))
            lo = mid + 1u;
        else
            hi = mid;
    }
    return k->at.has_lo ? lo - 1u : 0;
}

/* Every node under kept node `k`, level by level: the inner ones the
 * Foliation's, the leaves the sift's — the Foliation's, or for DIGESTS the
 * reads' table's, a range no copy of whose node reads among them. */
static void UnderMarked(TagFSVolume* vol, QuireMarks* m, const BinderKept* k)
{
    uint64_t   tree = k->tree;
    TallyPlace p;
    bool       rotten = false;
    for (uint64_t level = 1; level < k->at.level; level++) {
        uint32_t             n   = 0;
        const FoliationNode* run = FoliationLevel(vol, tree, level, &n);
        for (uint32_t i = run ? RunAt(tree, run, n, k) : n; i < n && StillUnder(tree, &run[i].place, k); i++)
            PlaceMarked(vol, m, &run[i].place);
    }
    uint32_t n = BinderSiftLeaves(vol, tree);
    for (uint32_t i = k->at.has_lo ? BinderSiftLeafOf(vol, tree, n, k->at.lo) : 0;
         i < n && BinderSiftLeafAt(vol, tree, i, &p, &rotten) && StillUnder(tree, &p, k); i++)
        PlaceMarked(vol, m, &p);
}

/* The marks as runs of whole Quires, in order: `out` (room `room`), how many. */
static uint32_t MarksRun(const QuireMarks* m, Strip* out, uint32_t room)
{
    uint32_t n = 0;
    uint64_t most = UINT32_MAX / m->q;
    for (uint64_t at = 0; at < m->nquire;) {
        if (!(m->bit[at / 64u] & (1ull << (at % 64u)))) {
            at++;
            continue;
        }
        uint64_t from = at;
        while (at < m->nquire && at - from < most && (m->bit[at / 64u] & (1ull << (at % 64u))))
            at++;
        if (out && n < room)
            out[n] = (Strip){ .start = from * m->q, .count = (uint32_t)((at - from) * m->q), .kind = STRIP_DATA };
        n++;
    }
    return n;
}

error_t BinderKeptQuires(TagFSVolume* vol, BinderRun* r)
{
    SlotGround* f = &r->fresh;
    QuireMarks  m;
    m.q      = RibbonCursorNow(vol)->quire_blocks ? RibbonCursorNow(vol)->quire_blocks : 1u;
    m.nquire = (vol->state.block_end + m.q - 1u) / m.q;
    if (r->nkept == 0)
        return OK;
    if (!(m.bit = kmalloc(sizeof(uint64_t) * (m.nquire / 64u + 1u))))
        return ERR_NO_MEMORY;
    memset(m.bit, 0, sizeof(uint64_t) * (m.nquire / 64u + 1u));
    for (uint32_t i = 0; i < r->nkept; i++) {
        PlaceMarked(vol, &m, &r->kept[i].at);
        if (!r->kept[i].rotten && r->kept[i].at.level > 0)
            UnderMarked(vol, &m, &r->kept[i]);
    }
    uint32_t n = MarksRun(&m, NULL, 0);
    if (n && !(f->quires = kmalloc(sizeof(Strip) * n))) {
        kfree(m.bit);
        return ERR_NO_MEMORY;
    }
    f->nquires   = MarksRun(&m, f->quires, n);
    f->capquires = n;
    kfree(m.bit);
    return OK;
}
