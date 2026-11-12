#include "binder_internal.h"
#include "../glean/glean.h"
#include "../dedup/dedup.h"

/*
 * Glean of the Tally-Quires (spec_v2 §4.8, §16.5 пп.17–18) at a Tally's DONE.
 *
 * A Tally laid in names every node nothing changed as it lies, wherever it
 * was laid, and a Tally-Quire is held whole while one node in it lives
 * (binder_quires.c): left to itself, a logger's book would hold a Quire for
 * its TAGS root, another for a leaf of FILES no line touches, Tally after
 * Tally, the pair moving on each time it fills. So when the Tally-Quires the
 * newest slot names a node in come to GLEAN_SLACK times the blocks its live
 * nodes take — and the current pair, by copy — the emptiest of them outside
 * the pair has its nodes laid again by the next Tally: a leaf's keys go into
 * DogEar, as a copy that does not hold puts them (§2.5), an inner node is
 * marked to be laid again in the Foliation; a Tally is demanded. The Quire then
 * goes back whole once no slot names a node in it (binder_slots.c, SlotGoes).
 *
 * ‼ A QUIRE WAITING FOR THE RING IS NOT COUNTED. One whose nodes the newest Tally no longer names, held only by an older slot,
 * goes back as the ring writes over that slot — an event, not an estimate:
 * Glean counts the newest slot's Quires alone, and never chooses such a one
 * again.
 *
 * ‼ A QUIRE A ROTTEN NODE LIES IN NEVER EMPTIES: no Tally lays that node again
 * until an act lets its range go (escheat.h), so it is never chosen — as §4.8
 * leaves a Quire a bookmark pins.
 *
 * The Settle's holder's, with no lock held: it reads only memory.
 */

/* The Quire the last Glean chose: chosen again, its nodes did not all leave it
 * with the Tally it demanded — they are marked again, and no Tally more is
 * demanded for it: the next one TALLY_SLACK owes takes them, so a Quire that
 * will not empty never turns the Tallies over and over. The Settle's
 * holder's. */
static uint64_t s_glean_last = UINT64_MAX;

/* A Quire the newest slot names a node in outside the pair, and the blocks
 * of the live nodes in it — UINT64_MAX for one a rotten node lies in. */
typedef struct {
    uint64_t start;
    uint64_t live;
} GleanQuire;

typedef struct {
    GleanQuire* q;
    uint32_t    n;
    uint64_t    grid;
} GleanSet;

/* Whether [at, at + grid) lies in the current pair. */
static bool GleanInPair(uint64_t at, uint64_t grid)
{
    return (at + grid > g_binder.pair_a && at < g_binder.pair_a + g_binder.pair_room) ||
           (at + grid > g_binder.pair_b && at < g_binder.pair_b + g_binder.pair_room);
}

/* The Quires slot `s` names a node in outside the pair — its own nodes and
 * the Quires of those it names as they lie, walked as one list by first block,
 * each Quire once — into `out` (room `room`): how many there are. */
static uint32_t GleanQuiresOf(const SlotGround* s, uint64_t grid, GleanQuire* out, uint32_t room)
{
    uint64_t past = 0;
    uint32_t n    = 0;
    for (uint32_t i = 0, j = 0; i < s->nground || j < s->nquires;) {
        bool         node = j == s->nquires || (i < s->nground && s->ground[i].start <= s->quires[j].start);
        const Strip* r    = node ? &s->ground[i++] : &s->quires[j++];
        for (uint64_t at = r->start / grid * grid; at < r->start + r->count; at += grid) {
            if (at < past || GleanInPair(at, grid))
                continue;
            past = at + grid;
            if (out && n < room)
                out[n] = (GleanQuire){ .start = at, .live = 0 };
            n++;
        }
    }
    return n;
}

/* The Quire of the set block `b` lies in, or NULL. */
static GleanQuire* GleanQuireOf(GleanSet* set, uint64_t b)
{
    uint64_t at = b / set->grid * set->grid;
    uint32_t lo = 0, hi = set->n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (set->q[mid].start < at)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo < set->n && set->q[lo].start == at ? &set->q[lo] : NULL;
}

/* Whether the mount found no copy of node `p` of `tree` to read. */
static bool GleanRotten(uint64_t tree, const TallyPlace* p)
{
    for (uint32_t i = 0; i < g_binder.nrot; i++)
        if (g_binder.rot[i].tree == tree && g_binder.rot[i].at.level == p->level && p->ptr.copies &&
            g_binder.rot[i].at.ptr.copies && g_binder.rot[i].at.ptr.block[0] == p->ptr.block[0])
            return true;
    return false;
}

/* Node `p` of `tree` counted into the Quires its copies lie in — or, `mark`,
 * marked to be laid again when one lies in `mark`. */
static void GleanNode(GleanSet* set, const GleanQuire* mark, uint64_t tree, const TallyPlace* p, bool rotten)
{
    bool in = false;
    for (uint32_t k = 0; k < p->ptr.copies; k++) {
        GleanQuire* q = GleanQuireOf(set, TallyPtrBlock(&p->ptr, k));
        if (!q)
            continue;
        in |= q == mark;
        if (!mark && q->live != UINT64_MAX)
            q->live = rotten ? UINT64_MAX : q->live + p->ptr.sheets;
    }
    if (!mark || !in)
        return;
    if (p->level == 0)
        DogEarMended(tree, p);
    else
        FoliationRelay(tree, p);
}

/* Every node of the Tally the next is laid against — the Foliation's, and
 * the leaves of DIGESTS out of the reads' table — handed to GleanNode. */
static void GleanWalk(GleanSet* set, const GleanQuire* mark)
{
    static const uint64_t trees[] = { TALLY_FILES, TALLY_TAGS, TALLY_USE, TALLY_DIGESTS };
    TallyPlace            p;
    bool                  rotten = false;
    for (uint32_t t = 0; t < sizeof(trees) / sizeof(trees[0]); t++) {
        uint32_t             nl = 0, ni = 0;
        const FoliationNode* l  = trees[t] == TALLY_DIGESTS ? NULL : FoliationLeaves(trees[t], &nl);
        const FoliationNode* in = FoliationInner(trees[t], &ni);
        for (uint32_t i = 0; i < nl; i++)
            GleanNode(set, mark, trees[t], &l[i].place, GleanRotten(trees[t], &l[i].place));
        for (uint32_t i = 0; i < ni; i++)
            GleanNode(set, mark, trees[t], &in[i].place, GleanRotten(trees[t], &in[i].place));
    }
    for (uint32_t i = 0; TallyDigestLeafAt(i, &p, &rotten); i++)
        if (p.level == 0)
            GleanNode(set, mark, TALLY_DIGESTS, &p, rotten);
}

/* The emptiest Quire of the set no rotten node lies in, or NULL. */
static const GleanQuire* GleanChosen(const GleanSet* set)
{
    const GleanQuire* best = NULL;
    for (uint32_t i = 0; i < set->n; i++)
        if (set->q[i].live != UINT64_MAX && (!best || set->q[i].live < best->live))
            best = &set->q[i];
    return best;
}

void BinderGleanAt(const BinderRun* r)
{
    const SlotGround* s    = &g_binder.slot[r->slot];
    GleanSet          set  = { NULL, 0, RibbonCursorNow()->quire_blocks ? RibbonCursorNow()->quire_blocks : 1u };
    uint64_t          live = 2u * g_binder.tally_blocks, pair = 2u * g_binder.pair_room;
    if (FoliationLost() || !g_binder.pair_room)
        return;
    set.n = GleanQuiresOf(s, set.grid, NULL, 0);
    if (!set.n || (uint64_t)set.n * set.grid + pair < GLEAN_SLACK * live + pair)
        return;
    if (!(set.q = kmalloc(sizeof(GleanQuire) * set.n))) {
        kprintf("[TagFS] Glean: no memory to weigh the %u Tally-Quire(s) Tally %llu names a node in — none is "
                "gleaned this time\n", set.n, (unsigned long long)r->w.seq);
        return;
    }
    set.n = GleanQuiresOf(s, set.grid, set.q, set.n);
    GleanWalk(&set, NULL);
    const GleanQuire* q = GleanChosen(&set);
    if (q && q->start == s_glean_last) {
        GleanWalk(&set, q);
    } else if (q) {
        kprintf("[TagFS] Glean: Tally %llu names a node in %llu block(s) of Tally-Quires past the pair, at least "
                "%u times the %llu its live nodes take — the Quire at block %llu, %llu of its %llu "
                "block(s) live, is laid again by the next Tally\n", (unsigned long long)r->w.seq,
                (unsigned long long)(set.n * set.grid), (unsigned)GLEAN_SLACK, (unsigned long long)live,
                (unsigned long long)q->start,
                (unsigned long long)q->live, (unsigned long long)set.grid);
        GleanWalk(&set, q);
        BinderDemanded();
    }
    s_glean_last = q ? q->start : UINT64_MAX;
    kfree(set.q);
}

/*
 * Dedup, Glean's step after each Tally (dedup/dedup.h; spec_v2 §8.2, §16.7
 * п.9): the ground the Tally took in — DogEar's, the blocks written since the
 * Tally before — handed to Dedup's pass, which finds the copies among them and
 * lays each on its original. Asked by the DONE before the Gloss lets go of
 * what the Tally's DIGESTS now names (binder_done.c, GlossCaptured): those
 * blocks' digests are the Gloss's until then, and the pass reads no leaf the
 * Tally just laid to learn them.
 */
void BinderGleanDedup(const BinderRun* r)
{
    DedupGleanAt(r->w.seq, r->w.sheets);
}

void BinderGleanLetGo(void)
{
    s_glean_last = UINT64_MAX;
}
