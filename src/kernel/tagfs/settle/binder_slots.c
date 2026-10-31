#include "binder_internal.h"
#include "escheat.h"
#include "../mount/book_mount.h"
#include "../ink/fallow.h"
#include "../block_map/block_map_derived.h"
#include "dog_ear.h"
#include "foliation.h"

/*
 * What the slots of the Spine hold (binder.h, FALLOW_BY_SPINE, §3.4, M-7):
 * the Tally each one names — every copy of every node, at its birth — and the
 * ribbon from its `at` on; the chain of the ribbon's Quires in their order;
 * the nodes no copy of which reads, named by every Tally as they lie; what
 * the mount holds for what rotted; and the numbers the medium has shown. The
 * mount lays it all down with the door shut (BinderSlot*, BinderMountStood);
 * the Settle's holder alone changes it after — a Tally's DONE, a Quire given
 * the ribbon — and reads it, so it takes no lock. A core that is not the
 * holder reads none of it: the slots, the chain and the lists are moved and
 * let go by the holder as it goes. What another core asks — whether a Tally
 * is owed (BinderOwed), what the ring lacks (BinderRingShort) — is published
 * by the holder in words of their own: each slot's own ground (`own`, which
 * lives as long as the mount), the due and the reason none may be begun.
 */

BinderBook g_binder;

// ----------------------------------------------------------------------------
// Lists
// ----------------------------------------------------------------------------

/* Room for one more of `size` bytes in *list of *n, *cap: false with no memory. */
bool BinderListRoom(void** list, uint64_t n, uint64_t* cap, size_t size)
{
    if (n < *cap)
        return true;
    uint64_t room = *cap ? *cap * 2u : 16u;
    void*    more = kmalloc(room * size);
    if (!more)
        return false;
    if (n)
        memcpy(more, *list, n * size);
    kfree(*list);
    *list = more;
    *cap  = room;
    return true;
}

bool BinderListRoom32(void** list, uint32_t n, uint32_t* cap, size_t size)
{
    uint64_t c = *cap;
    bool     ok = BinderListRoom(list, n, &c, size);
    *cap = (uint32_t)c;
    return ok;
}

void BinderSlotLetGo(SlotGround* s)
{
    while (s->sealed) {
        SlotSealed* u = s->sealed;
        s->sealed     = u->next;
        kfree(u);
    }
    kfree(s->ground);
    kfree(s->quires);
    kfree(s->extra);
    memset(s, 0, sizeof(*s));
    s->chain_from = UINT64_MAX;
}

// ----------------------------------------------------------------------------
// What the mount read
// ----------------------------------------------------------------------------

/* No book stands from the first line on: the Tally under way ends weighing
 * nothing (binder.c, TallyDue) — a volume let go of has its file map, its
 * registry and its Use Context gone before the book is (tagfs.c,
 * tagfs_clear_the_ground), and what a leaf keeps live is asked of them
 * (binder_live.c). */
void BinderMountReset(void)
{
    g_binder.up = false;
    BinderStopped(ERR_DEVICE_NOT_READY);
    DogEarLetGo();
    FoliationLetGo();
    BinderProofLetGo();
    BinderGleanLetGo();
    EscheatLetGo();
    for (uint32_t i = 0; i < g_binder.ring; i++)
        BinderSlotLetGo(&g_binder.slot[i]);
    kfree(g_binder.slot);
    kfree(g_binder.own);
    kfree(g_binder.chain);
    kfree(g_binder.rot);
    kfree(g_binder.held);
    memset(&g_binder, 0, sizeof(g_binder));
    BirthsLetGo();
    EndpaperForget();
}

error_t BinderSlotsMake(uint32_t ring)
{
    g_binder.slot = kmalloc(sizeof(SlotGround) * (ring ? ring : 1u));
    g_binder.own  = kmalloc(sizeof(uint64_t) * (ring ? ring : 1u));
    if (!g_binder.slot || !g_binder.own) {
        kfree(g_binder.slot);
        kfree(g_binder.own);
        g_binder.slot = NULL;
        g_binder.own  = NULL;
        return ERR_NO_MEMORY;
    }
    memset(g_binder.slot, 0, sizeof(SlotGround) * (ring ? ring : 1u));
    memset(g_binder.own, 0, sizeof(uint64_t) * (ring ? ring : 1u));
    for (uint32_t i = 0; i < ring; i++)
        g_binder.slot[i].chain_from = UINT64_MAX;
    g_binder.ring = ring;
    return OK;
}

error_t BinderSlotHolds(uint32_t index, uint64_t seq, uint64_t tally_seq, uint64_t stitch_seq)
{
    if (index >= g_binder.ring)
        return ERR_INVALID_ARGUMENT;
    SlotGround* s = &g_binder.slot[index];
    s->holds = true;
    s->seq   = seq;
    s->tally = tally_seq;
    s->slice = stitch_seq;
    if (index + 1u > g_binder.nslots)
        g_binder.nslots = index + 1u;
    return OK;
}

error_t BinderSlotNode(uint32_t index, uint64_t start, uint64_t count, uint64_t birth)
{
    if (index >= g_binder.ring)
        return ERR_INVALID_ARGUMENT;
    SlotGround* s = &g_binder.slot[index];
    if (!BinderListRoom32((void**)&s->ground, s->nground, &s->capground, sizeof(Strip)))
        return ERR_NO_MEMORY;
    s->ground[s->nground++] = (Strip){ .start = start, .count = (uint32_t)count, .kind = STRIP_DATA, .birth = birth };
    return OK;
}

error_t BinderSlotRibbon(uint32_t index, const RibbonQuire* q)
{
    if (index >= g_binder.ring)
        return ERR_INVALID_ARGUMENT;
    SlotGround* s = &g_binder.slot[index];
    for (uint32_t i = 0; i < s->nextra; i++)
        if (RibbonQuireSame(&s->extra[i], q))
            return OK;
    if (!BinderListRoom32((void**)&s->extra, s->nextra, &s->capextra, sizeof(RibbonQuire)))
        return ERR_NO_MEMORY;
    s->extra[s->nextra++] = *q;
    return OK;
}

error_t BinderRotten(uint64_t tree, const TallyPlace* at)
{
    if (!BinderListRoom32((void**)&g_binder.rot, g_binder.nrot, &g_binder.caprot, sizeof(BinderKept)))
        return ERR_NO_MEMORY;
    uint32_t i = g_binder.nrot++;
    /* In the order of their tree, then of their keys: the walk of each tree
     * meets them in key order, and the trees in theirs. */
    while (i > 0 && (g_binder.rot[i - 1u].tree > tree ||
                     (g_binder.rot[i - 1u].tree == tree && at->has_lo &&
                      (!g_binder.rot[i - 1u].at.has_lo || g_binder.rot[i - 1u].at.lo.k1 > at->lo.k1 ||
                       (g_binder.rot[i - 1u].at.lo.k1 == at->lo.k1 && g_binder.rot[i - 1u].at.lo.k2 > at->lo.k2))))) {
        g_binder.rot[i] = g_binder.rot[i - 1u];
        i--;
    }
    g_binder.rot[i] = (BinderKept){ .tree = tree, .at = *at, .rotten = true };
    return OK;
}

error_t BinderHeldRun(uint64_t start, uint64_t count)
{
    if (!BinderListRoom((void**)&g_binder.held, g_binder.nheld, &g_binder.capheld, sizeof(BinderGround)))
        return ERR_NO_MEMORY;
    g_binder.held[g_binder.nheld++] = (BinderGround){ start, count };
    return OK;
}

/* A Strip sifted down a heap of `n` by first block. */
static void StripSift(Strip* g, uint32_t at, uint32_t n)
{
    for (;;) {
        uint32_t big = at, l = 2u * at + 1u, r = l + 1u;
        if (l < n && g[l].start > g[big].start)
            big = l;
        if (r < n && g[r].start > g[big].start)
            big = r;
        if (big == at)
            return;
        Strip t = g[at];
        g[at]  = g[big];
        g[big] = t;
        at     = big;
    }
}

/* A slot's ground in the order of its first block (a heap sort: a Tally's
 * nodes are counted in thousands), so a node is looked for by halving. */
void BinderSlotSort(SlotGround* s)
{
    for (uint32_t i = s->nground / 2u; i-- > 0;)
        StripSift(s->ground, i, s->nground);
    for (uint32_t end = s->nground; end > 1u; end--) {
        Strip t = s->ground[0];
        s->ground[0]       = s->ground[end - 1u];
        s->ground[end - 1u] = t;
        StripSift(s->ground, 0, end - 1u);
    }
}

uint64_t BinderChainIndex(const RibbonQuire* q)
{
    for (uint64_t i = 0; i < g_binder.nchain; i++)
        if (RibbonQuireSame(&g_binder.chain[i], q))
            return g_binder.chain_base + i;
    return UINT64_MAX;
}

/* The slots not stood on: each Quire of their ribbons that lies on the chain
 * the slot stood on begins their hold of it; the others are their own. */
static void SlotsOnChain(uint32_t stood)
{
    for (uint32_t i = 0; i < g_binder.nslots; i++) {
        SlotGround* s = &g_binder.slot[i];
        uint32_t    kept = 0;
        BinderSlotSort(s);
        if (i == stood) {
            s->nextra     = 0;
            s->chain_from = g_binder.chain_base;
            continue;
        }
        for (uint32_t q = 0; q < s->nextra; q++) {
            uint64_t at = BinderChainIndex(&s->extra[q]);
            if (at == UINT64_MAX)
                s->extra[kept++] = s->extra[q];
            else if (at < s->chain_from)
                s->chain_from = at;
        }
        s->nextra = kept;
    }
}

void BinderMountStood(uint32_t index, uint64_t tally_blocks, uint64_t useful, uint64_t next_fid,
                      uint64_t next_tag, uint64_t next_bookmark)
{
    SlotGround* s     = &g_binder.slot[index < g_binder.ring ? index : 0];
    bool        short_ = g_book.binder_short;
    /* The chain is the ribbon of the slot stood on, in its order; every
     * Quire given from here goes on it (BinderRibbonGiven). One the chain
     * cannot hold would be let go by no Tally, and the order of the rest
     * would be wrong: no Tally is written by this mount then. */
    for (uint32_t q = 0; !short_ && q < s->nextra; q++) {
        if (!BinderListRoom((void**)&g_binder.chain, g_binder.nchain, &g_binder.capchain, sizeof(RibbonQuire)))
            short_ = true;
        else
            g_binder.chain[g_binder.nchain++] = s->extra[q];
    }
    SlotsOnChain(index);
    if (short_)
        BinderNoTalliesFrom("memory would not hold what the Spine's slots hold for the writer of a Tally");
    for (uint32_t i = 0; i < g_binder.ring; i++)
        BinderSlotOwnPublish(i, 0);
    g_binder.tally_blocks  = tally_blocks;
    g_binder.cost_known    = 0;
    g_binder.useful        = useful;
    g_binder.seen_fid      = next_fid;
    g_binder.seen_tag      = next_tag;
    g_binder.seen_bookmark = next_bookmark;
    g_binder.up            = true;
    BinderWeigh();
}

const char* BinderNoTallies(void)
{
    return __atomic_load_n(&g_binder.no_tallies, __ATOMIC_ACQUIRE);
}

void BinderNoTalliesFrom(const char* why)
{
    const char* none = NULL;
    (void)__atomic_compare_exchange_n(&g_binder.no_tallies, &none, why, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

// ----------------------------------------------------------------------------
// What the ring lacks (BinderRingShort)
// ----------------------------------------------------------------------------

static uint64_t SlotOwn(uint32_t index, uint64_t q);

/* A slot held unsure over one that held nothing (BinderSlotUnsure: its Tally
 * and the one that may stand in it the same) may hold the new Tally or none,
 * and a mount whose write there tore stands on the newest slot before it: its
 * slice is the bar's too, and it spends no step of the lag. */
uint64_t BinderBarSlice(const SlotGround* slots, uint32_t n, uint64_t lag, uint64_t floor)
{
    uint64_t slice = floor, below = UINT64_MAX;
    for (uint64_t k = 0; k < lag;) {
        const SlotGround* newest = NULL;
        for (uint32_t i = 0; i < n; i++) {
            const SlotGround* s = &slots[i];
            if (s->holds && s->tally < below && (!newest || s->tally > newest->tally))
                newest = s;
        }
        if (!newest)
            break;
        below = newest->tally;
        slice = newest->slice < slice ? newest->slice : slice;
        if (!newest->unsure || newest->tally != newest->tally2)
            k++;
    }
    return slice;
}

void BinderBarPublish(uint64_t floor)
{
    uint32_t n     = g_binder.nslots < g_binder.ring ? g_binder.nslots : g_binder.ring;
    uint64_t slice = BinderBarSlice(g_binder.slot, n, FallowLag(), floor);
    TagFSLockTake(&tagfs_get_state()->lock);
    FallowNode* spent = FallowBarLift(slice);
    TagFSLockDrop(&tagfs_get_state()->lock);
    FallowNodesFree(spent);
}

/* Whether a Tally over the slots but the newest gives ground back: a Quire of
 * the chain but the one the ribbon stands in and its next_quire, or a slot
 * whose Tally took in an older stitch than the newest's — its own nodes and
 * the Tally-Quires they hold whole go once a Tally is written over it. */
void BinderTurnGivesPublish(void)
{
    const RibbonCursor* c      = RibbonCursorNow();
    uint64_t            newest = 0;
    bool                gives  = false;
    for (uint64_t i = 0; c->up && i < g_binder.nchain && !gives; i++)
        gives = !RibbonQuireSame(&g_binder.chain[i], &c->quire) && !RibbonQuireSame(&g_binder.chain[i], &c->next);
    for (uint32_t i = 0; i < g_binder.nslots && i < g_binder.ring; i++)
        if (g_binder.slot[i].holds && g_binder.slot[i].slice > newest)
            newest = g_binder.slot[i].slice;
    for (uint32_t i = 0; i < g_binder.nslots && i < g_binder.ring && !gives; i++)
        gives = g_binder.slot[i].holds && g_binder.slot[i].slice < newest;
    __atomic_store_n(&g_binder.turn_gives, gives ? 1u : 0u, __ATOMIC_RELEASE);
}

/* Slot `index`'s own as it stands: the whole Quires a Tally over it gives
 * back, what the Tally before it let go while it rests, and what a Tally
 * under way over it took. */
static void SlotOwnSaid(uint32_t index)
{
    const SlotGround* s = &g_binder.slot[index];
    uint64_t          q = RibbonCursorNow()->quire_blocks ? RibbonCursorNow()->quire_blocks : 1u;
    __atomic_store_n(&g_binder.own[index], SlotOwn(index, q) + s->rest + s->under, __ATOMIC_RELEASE);
}

void BinderSlotOwnPublish(uint32_t index, uint64_t more)
{
    if (index >= g_binder.ring)
        return;
    g_binder.slot[index].under = more;
    SlotOwnSaid(index);
    BinderGroundHeldPublish();
}

void BinderSlotsOwnPublish(void)
{
    for (uint32_t i = 0; i < g_binder.ring; i++)
        SlotOwnSaid(i);
    BinderGroundHeldPublish();
}

/*
 * ‼ THE GROUND A TALLY LET GO STAYS ITS SLOT'S UNTIL IT IS FREE (§16.6 п.8). A
 * Tally's DONE lets the slot's old ground go to Fallow for the lag's stitches
 * (BinderSlotGoes) and the slot holds only the new Tally's: counted then, the
 * reserve rose by the old ground at once, while the free did not until Fallow
 * gave it back — the ribbon's Quire a full volume keeps it, the write's
 * reserve with it, gone for the lag. So the slot counts what it let go as its
 * own (`rest`) until Fallow gives it back, and lets the count go in the same
 * hold of the state lock the give is made in (sewing.c): not at the TAKE of
 * the stitch whose settling may give it — Fallow gives nothing while a Tally
 * is under way (BinderHoldsGround), and Tallies one after another at a full
 * volume's wall held it there while the count was gone — nor after, when a
 * measure taken between would count it twice. A rest a DONE finds still
 * resting is added to, never put over: its ground is no freer for it.
 */
void BinderRestRipens(uint64_t resting_from)
{
    for (uint32_t i = 0; i < g_binder.ring; i++) {
        SlotGround* s = &g_binder.slot[i];
        if (s->rest && s->rest_until < resting_from) {
            __atomic_sub_fetch(&g_binder.own[i], s->rest, __ATOMIC_ACQ_REL);
            s->rest = 0;
        }
    }
}

/* What each slot but `index` lacks of `tally` blocks, its own ground counted
 * in whole Quires of `quire` — the ground a Tally takes is (binder.c) — so the
 * reserve stays whole Quires. */
uint64_t BinderRingShortBut(uint64_t tally, uint64_t quire, uint32_t index)
{
    uint64_t lacks = 0;
    for (uint32_t i = 0; quire && i < g_binder.ring; i++) {
        uint64_t own = __atomic_load_n(&g_binder.own[i], __ATOMIC_ACQUIRE) / quire * quire;
        if (i != index && own < tally)
            lacks += tally - own;
    }
    return lacks;
}

uint64_t BinderRingShort(uint64_t tally, uint64_t quire)
{
    return BinderRingShortBut(tally, quire, UINT32_MAX);
}

#ifdef CONFIG_VOLUME_TESTS
#define BINDER_STAND_GIVEN_MOST  256u
static RibbonQuire s_stand_given[BINDER_STAND_GIVEN_MOST];
static uint32_t    s_stand_given_n;

void BinderStandGivenBegin(void)
{
    s_stand_given_n = 0;
}

uint32_t BinderStandGiven(RibbonQuire* out, uint32_t room)
{
    for (uint32_t i = 0; i < s_stand_given_n && i < room; i++)
        out[i] = s_stand_given[i];
    return s_stand_given_n;
}

uint32_t BinderStandGround(Strip* out, uint32_t room)
{
    uint32_t n = 0;
    uint64_t from = UINT64_MAX;
    for (uint32_t i = 0; i < g_binder.nslots; i++) {
        const SlotGround* s = &g_binder.slot[i];
        if (!s->holds)
            continue;
        for (uint32_t k = 0; k < s->nground; k++, n++)
            if (n < room)
                out[n] = s->ground[k];
        for (uint32_t k = 0; k < s->nquires; k++, n++)
            if (n < room)
                out[n] = s->quires[k];
        for (uint32_t k = 0; k < s->nextra; k++, n++)
            if (n < room)
                out[n] = (Strip){ .start = s->extra[k].block, .count = (uint32_t)s->extra[k].blocks, .kind = STRIP_DATA };
        from = s->chain_from < from ? s->chain_from : from;
    }
    for (uint64_t at = from; from != UINT64_MAX && at < g_binder.chain_base + g_binder.nchain; at++, n++)
        if (n < room && at >= g_binder.chain_base)
            out[n] = (Strip){ .start = g_binder.chain[at - g_binder.chain_base].block,
                              .count = (uint32_t)g_binder.chain[at - g_binder.chain_base].blocks, .kind = STRIP_DATA };
    /* And what of the pair of Tally-Quires the next Tally goes on in no node
     * was laid in yet: the book's, no slot's. */
    if (g_binder.pair_room > g_binder.pair_used) {
        uint64_t used = g_binder.pair_used, left = g_binder.pair_room - used;
        for (uint32_t k = 0; k < 2u; k++, n++)
            if (n < room)
                out[n] = (Strip){ .start = (k ? g_binder.pair_b : g_binder.pair_a) + used, .count = (uint32_t)left,
                                  .kind = STRIP_DATA };
    }
    return n;
}
#endif

void BinderRibbonGiven(const RibbonQuire* q)
{
#ifdef CONFIG_VOLUME_TESTS
    if (s_stand_given_n < BINDER_STAND_GIVEN_MOST)
        s_stand_given[s_stand_given_n++] = *q;
#endif
    if (!g_binder.up || BinderChainIndex(q) != UINT64_MAX)
        return;
    if (!BinderListRoom((void**)&g_binder.chain, g_binder.nchain, &g_binder.capchain, sizeof(RibbonQuire))) {
        /* A Quire the chain cannot hold is never let go by a Tally: held for good. */
        kprintf("[TagFS] no memory to note the ribbon's Quire %llu+%llu for the Tallies — it is held until the "
                "volume is mounted again\n", (unsigned long long)q->block, (unsigned long long)q->blocks);
        return;
    }
    g_binder.chain[g_binder.nchain++] = *q;
    BinderGroundHeldPublish();
}

// ----------------------------------------------------------------------------
// What the slots hold (FALLOW_BY_SPINE)
// ----------------------------------------------------------------------------

/* Whether runs `g` (`n`, in order, apart from one another) take any block
 * of [start, end). */
static bool RunsName(const Strip* g, uint32_t n, uint64_t start, uint64_t end)
{
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (g[mid].start + g[mid].count <= start)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo < n && g[lo].start < end;
}

/* Whether slot `s` names any block of [start, end): its nodes, or a
 * Tally-Quire its Tally names a node in as it lies. */
static bool SlotNames(const SlotGround* s, uint64_t start, uint64_t end)
{
    return RunsName(s->ground, s->nground, start, end) || RunsName(s->quires, s->nquires, start, end);
}

/*
 * ‼ A TALLY-QUIRE IS HELD WHOLE WHILE ONE NODE IN IT LIVES (spec_v2 §16.5
 * п.17, Р-d3 (б)). The Binder takes its Tally-Quires whole on the grid of the
 * ribbon's Quire (binder.c TallyRunsTake), and every slot that holds counts
 * the nodes of its Tally — or of either, unsure — in each by its ground and
 * by the Quires its Tally names a node in as it lies (binder_quires.c): the
 * Quire goes back whole once no such count is left, the nodes there that no
 * Tally names any more and the tail no Tally reached with it (§4.8), resting
 * in Fallow for the lag (§3.4) — the same Quire a mount's census holds whole
 * (mount/ground_census.c GroundQuiresHeld). What of the current pair a Tally
 * may still be laid in stays the pair's.
 */
static bool SlotsHold(uint64_t start, uint64_t end)
{
    for (uint32_t i = 0; i < g_binder.nslots; i++)
        if (g_binder.slot[i].holds && SlotNames(&g_binder.slot[i], start, end))
            return true;
    return false;
}

/* Whether the current pair may still lay a Tally in [start, end): past the
 * Quires its Tallies filled. */
static bool PairHolds(uint64_t start, uint64_t end, uint64_t q)
{
    if (!g_binder.pair_room)
        return false;
    uint64_t used = g_binder.pair_used / q * q;
    return (start < g_binder.pair_a + g_binder.pair_room && g_binder.pair_a + used < end) ||
           (start < g_binder.pair_b + g_binder.pair_room && g_binder.pair_b + used < end);
}

static bool QuireHeld(uint64_t start, uint64_t end, uint64_t q)
{
    return SlotsHold(start, end) || PairHolds(start, end, q);
}

/* The end of the Quire of the ribbon `q` (`n` of them) that `block` lies in,
 * or `block`. */
static uint64_t RibbonQuirePast(const RibbonQuire* q, uint64_t n, uint64_t block)
{
    for (uint64_t i = 0; i < n; i++)
        if (block >= q[i].block && block - q[i].block < q[i].blocks)
            return q[i].block + q[i].blocks;
    return block;
}

uint64_t BinderOwnPast(uint64_t block)
{
    uint64_t q    = RibbonCursorNow()->quire_blocks;
    uint64_t cell = q ? block / q * q : 0;
    if (!g_binder.up)
        return block;
    if (q && QuireHeld(cell, cell + q, q))
        return cell + q;
    uint64_t past = RibbonQuirePast(g_binder.chain, g_binder.nchain, block);
    for (uint32_t i = 0; past == block && i < g_binder.nslots; i++)
        if (g_binder.slot[i].holds)
            past = RibbonQuirePast(g_binder.slot[i].extra, g_binder.slot[i].nextra, block);
    return past;
}

/* Where a slot goes in the ring's turn: one that holds nothing — or may hold
 * either of two Tallies — first, then by seq. */
static uint64_t SlotTurn(const SlotGround* s)
{
    return s->holds && !s->unsure ? s->seq : 0;
}

uint32_t BinderSlotNext(void)
{
    uint32_t best  = 0;
    uint64_t least = UINT64_MAX;
    for (uint32_t i = 0; i < g_binder.ring; i++) {
        if (SlotTurn(&g_binder.slot[i]) < least) {
            least = SlotTurn(&g_binder.slot[i]);
            best  = i;
        }
    }
    return best;
}

/* Whether [start, end) goes back with slot `index`: no slot that holds and
 * goes as late or later names it, nor — the slot the next Tally is written
 * over — may the current pair still lay a Tally in it. Any other slot goes a
 * Tally later at the least, and by then the pair has moved on — its Quire the
 * slot's to give back (BinderPairLetGo) — or the Tally went on in that Quire
 * and names it, the last to go. */
static bool QuireGoesWith(uint32_t index, uint64_t start, uint64_t end, uint64_t q)
{
    uint64_t turn = SlotTurn(&g_binder.slot[index]);
    for (uint32_t i = 0; i < g_binder.nslots; i++) {
        const SlotGround* s = &g_binder.slot[i];
        if (i != index && s->holds && SlotTurn(s) >= turn && SlotNames(s, start, end))
            return false;
    }
    return index != BinderSlotNext() || !PairHolds(start, end, q);
}

/*
 * ‼ A SLOT'S OWN GROUND IS WHAT A TALLY OVER IT GIVES BACK, IN WHOLE QUIRES
 * (§16.4 п.7, §16.6 п.8). A Tally takes its pair in whole Quires (binder.c
 * TallyRunsTake) and gives a slot's ground back the same (QuiresGo): counted
 * as its nodes' blocks born at its Tally and rounded down to a Quire, each
 * slot was short by up to a Quire a copy of what it gives back, and the
 * reserve kept that for every slot of the ring; and a node a later Tally names
 * as it lies was the slot's that bore it, so the DONE over that slot took it
 * off the count of every slot while no Tally gave it back. So a Quire of the
 * ribbon's grid any node the slot names lies in — or a Tally-Quire it names
 * one in as it lies — is its own, whole, when it goes back with it: no slot
 * that goes as late in the ring's turn or later names one in it, so every
 * Quire the slots name is one slot's, the last of them to go, as SlotGoes
 * gives it back; and the Quires the current pair may still lay a Tally in are
 * the slot's whose Tally lies there, but for the slot the next Tally is
 * written over (QuireGoesWith). Counted again for every slot at each DONE
 * (BinderSlotsOwnPublish). With the Quire the ribbon's cursor does not know
 * yet, a block a Quire — its nodes.
 */
static uint64_t SlotOwn(uint32_t index, uint64_t q)
{
    const SlotGround* s    = &g_binder.slot[index];
    uint64_t          own  = 0;
    uint64_t          past = 0;
    for (uint32_t i = 0, j = 0; s->holds && (i < s->nground || j < s->nquires);) {
        bool         node = j == s->nquires || (i < s->nground && s->ground[i].start <= s->quires[j].start);
        const Strip* g    = node ? &s->ground[i++] : &s->quires[j++];
        for (uint64_t at = g->start / q * q; at < g->start + g->count; at += q) {
            if (at < past)
                continue;
            past = at + q;
            if (QuireGoesWith(index, at, at + q, q))
                own += q;
        }
    }
    return own;
}

/* Whether a Quire off the chain is another slot's too. */
static bool ExtraHeld(const RibbonQuire* q)
{
    for (uint32_t i = 0; i < g_binder.nslots; i++)
        for (uint32_t k = 0; g_binder.slot[i].holds && k < g_binder.slot[i].nextra; k++)
            if (RibbonQuireSame(&g_binder.slot[i].extra[k], q))
                return true;
    return false;
}

/* The least place on the chain a slot's ribbon begins at. */
static uint64_t ChainHeldFrom(void)
{
    uint64_t from = UINT64_MAX;
    for (uint32_t i = 0; i < g_binder.nslots; i++)
        if (g_binder.slot[i].holds && g_binder.slot[i].chain_from < from)
            from = g_binder.slot[i].chain_from;
    return from;
}

/* Whether [start, end) is a slot's own: one that holds names it and it goes
 * back with that slot (QuireGoesWith). */
static bool QuireOwned(uint64_t start, uint64_t end, uint64_t q)
{
    for (uint32_t i = 0; i < g_binder.nslots && i < g_binder.ring; i++)
        if (g_binder.slot[i].holds && SlotNames(&g_binder.slot[i], start, end) && QuireGoesWith(i, start, end, q))
            return true;
    return false;
}

/* The Quires of the ribbon the slots hold: the chain from the least place a
 * slot's ribbon begins at — all of it while none holds — and those off it,
 * each once. */
static uint64_t RibbonHeld(void)
{
    uint64_t held = 0, from = ChainHeldFrom(), end = g_binder.chain_base + g_binder.nchain;
    for (uint64_t at = from > g_binder.chain_base && from != UINT64_MAX ? from : g_binder.chain_base; at < end; at++)
        held += g_binder.chain[at - g_binder.chain_base].blocks;
    for (uint32_t i = 0; i < g_binder.nslots; i++) {
        const SlotGround* s = &g_binder.slot[i];
        for (uint32_t k = 0; s->holds && k < s->nextra; k++) {
            bool before = false;
            for (uint32_t j = 0; j < i && !before; j++)
                for (uint32_t e = 0; g_binder.slot[j].holds && e < g_binder.slot[j].nextra && !before; e++)
                    before = RibbonQuireSame(&g_binder.slot[j].extra[e], &s->extra[k]);
            held += before ? 0 : s->extra[k].blocks;
        }
    }
    return held;
}

/*
 * ‼ WHAT THE BOOK HOLDS ITSELF IS NAMED, NOT WHAT IS LEFT (§16.6 п.8). A write
 * refused at a full volume's wall says it beside what the book keeps, and a
 * refusal is weighed against another by both (logcheck fullvolume, roomback):
 * left over — the data run but the free, the records' blocks and the staked —
 * it held every block no owner names, and a leak of the book's ground, or a
 * hole a record counts, was in the number that excused it. So it is the
 * Quires the Binder names: every Quire of the grid a slot that holds names,
 * each the own of the last slot to go of those that name it (SlotOwn — less
 * what a slot counts its own past its ground: the rest Fallow holds for it,
 * the pair a Tally under way took, `under`); the Quires of the current pair
 * no slot counts its own — those a Tally may still be laid in, the whole of a
 * pair a Tally under way took — and the Quires of the ribbon the slots hold.
 * Published by the Settle's holder wherever one of them moves, read from any
 * core (BinderGroundHeld).
 */
void BinderGroundHeldPublish(void)
{
    uint64_t q    = RibbonCursorNow()->quire_blocks ? RibbonCursorNow()->quire_blocks : 1u;
    uint64_t held = RibbonHeld();
    for (uint32_t i = 0; i < g_binder.ring; i++) {
        uint64_t own  = __atomic_load_n(&g_binder.own[i], __ATOMIC_ACQUIRE);
        uint64_t past = g_binder.slot[i].rest + g_binder.slot[i].under;
        held += own > past ? own - past : 0;
    }
    for (uint32_t k = 0; g_binder.pair_room && k < 2u; k++) {
        uint64_t base = k ? g_binder.pair_b : g_binder.pair_a;
        for (uint64_t at = base / q * q; at < base + g_binder.pair_room; at += q)
            held += QuireOwned(at, at + q, q) ? 0 : q;
    }
    __atomic_store_n(&g_binder.ground_held, held, __ATOMIC_RELEASE);
}

uint64_t BinderGroundHeld(void)
{
    return __atomic_load_n(&g_binder.ground_held, __ATOMIC_ACQUIRE);
}

/* A run more into the list of what goes. */
static bool GoesPut(Strip** list, uint64_t* n, uint64_t* cap, uint64_t start, uint64_t count)
{
    if (!BinderListRoom((void**)list, *n, cap, sizeof(Strip)))
        return false;
    (*list)[(*n)++] = (Strip){ .start = start, .count = (uint32_t)count, .kind = STRIP_DATA };
    return true;
}

/* The Tally-Quires slot `old` named a node in that no slot holds now — its
 * nodes and its Quires walked as one list by first block, each Quire once —
 * into the list of what goes. False with no memory. */
static bool QuiresGo(const SlotGround* old, uint64_t q, Strip** goes, uint64_t* ngoes, uint64_t* cap)
{
    uint64_t past = 0;
    for (uint32_t i = 0, j = 0; i < old->nground || j < old->nquires;) {
        bool         node = j == old->nquires || (i < old->nground && old->ground[i].start <= old->quires[j].start);
        const Strip* s    = node ? &old->ground[i++] : &old->quires[j++];
        uint64_t     end  = s->start + s->count;
        for (uint64_t at = s->start / q * q; at < end; at += q) {
            if (at < past || QuireHeld(at, at + q, q))
                continue;
            past = at + q;
            if (!GoesPut(goes, ngoes, cap, at, q))
                return false;
        }
    }
    return true;
}

/*
 * What the slot `old` held that no slot holds now — the Tally-Quires its
 * nodes lie in no slot that holds names a node in (QuireHeld), whole, the
 * Quires of its ribbon off the chain no other slot's, and the chain from where its ribbon
 * began to where the earliest slot's now does (L-12: every slot names its
 * `at` later on the ribbon than their end) — as runs, for Fallow. The chain
 * before every slot's ribbon let go of in memory. False with no memory.
 */
static bool SlotGoes(const SlotGround* old, Strip** goes, uint64_t* n)
{
    uint64_t cap = 0, held = ChainHeldFrom();
    *goes = NULL;
    *n    = 0;
    uint64_t q = RibbonCursorNow()->quire_blocks ? RibbonCursorNow()->quire_blocks : 1u;
    if (!QuiresGo(old, q, goes, n, &cap))
        return false;
    for (uint32_t i = 0; i < old->nextra; i++)
        if (!ExtraHeld(&old->extra[i]) && !GoesPut(goes, n, &cap, old->extra[i].block, old->extra[i].blocks))
            return false;
    for (uint64_t at = old->chain_from; at != UINT64_MAX && at < held && at < g_binder.chain_base + g_binder.nchain;
         at++) {
        const RibbonQuire* q = &g_binder.chain[at - g_binder.chain_base];
        if (at >= g_binder.chain_base && !GoesPut(goes, n, &cap, q->block, q->blocks))
            return false;
    }
    if (held != UINT64_MAX && held > g_binder.chain_base) {
        uint64_t drop = held - g_binder.chain_base;
        drop = drop < g_binder.nchain ? drop : g_binder.nchain;
        memmove(g_binder.chain, g_binder.chain + drop, sizeof(RibbonQuire) * (g_binder.nchain - drop));
        g_binder.nchain     -= drop;
        g_binder.chain_base += drop;
    }
    return true;
}

/* The runs `goes` (`n`) resting in Fallow for the lag's stitches
 * (FALLOW_BY_SPINE): their blocks. With no memory for its node, held until the
 * volume is mounted again — said. */
static uint64_t GoesRest(Strip* goes, uint64_t n)
{
    uint64_t    blocks = 0;
    FallowNode* node   = n && n <= UINT32_MAX ? FallowNodeMake(goes, (uint32_t)n, FALLOW_BY_SPINE, 0) : NULL;
    for (uint64_t i = 0; i < n; i++)
        blocks += goes[i].count;
    if (n && !node) {
        kprintf("[TagFS] no memory to rest the %llu block(s) the slots no longer hold — they are held until the "
                "volume is mounted again\n", (unsigned long long)blocks);
        return 0;
    }
    if (!node)
        return 0;
    TagFSLockTake(&tagfs_get_state()->lock);
    FallowSpineAdd(node, SettledCycles());
    TagFSLockDrop(&tagfs_get_state()->lock);
    return blocks;
}

/*
 * The current pair of Tally-Quires let go for a new one (binder.c
 * TallyRunsTake), each of its two runs: the Quires no node was laid in back to
 * the map at once — no slot ever named them; the one the last Tally ended
 * inside, when no slot that holds names a node in it any more, resting in
 * Fallow as a slot's ground does; every other stays with the slots that name its nodes,
 * and goes back whole with the last of them (SlotGoes). Before the pair is
 * moved: it reads the pair it lets go.
 */
void BinderPairLetGo(uint64_t q)
{
    if (!g_binder.pair_room || !q)
        return;
    uint64_t used = (g_binder.pair_used + q - 1u) / q * q, last = g_binder.pair_used / q * q;
    Strip    goes[2];
    uint64_t n = 0;
    for (uint32_t k = 0; k < 2u; k++) {
        uint64_t base = k ? g_binder.pair_b : g_binder.pair_a;
        if (used < g_binder.pair_room)
            (void)BlockMapDerivedGiveAs(GROUND_TALLY, 0, base + used, g_binder.pair_room - used);
        /* The Quire the last Tally ended inside, which SlotGoes left the pair
         * (QuireHeld) — one it filled was no longer the pair's to keep. */
        if (last < used && !SlotsHold(base + last, base + used))
            goes[n++] = (Strip){ .start = base + last, .count = (uint32_t)q, .kind = STRIP_DATA };
    }
    (void)GoesRest(goes, n);
}

/* What the slot `old` held that no slot holds now, resting in Fallow for the
 * lag's stitches (FALLOW_BY_SPINE): its blocks. With no memory for its node,
 * held until the volume is mounted again — said. */
uint64_t BinderSlotGoes(const SlotGround* old)
{
    Strip*   goes = NULL;
    uint64_t n    = 0;
    if (!SlotGoes(old, &goes, &n)) {
        kfree(goes);
        kprintf("[TagFS] no memory to list the ground slot %llu's Tally held — what no slot holds now is held "
                "until the volume is mounted again\n", (unsigned long long)old->seq);
        return 0;
    }
    uint64_t blocks = GoesRest(goes, n);
    kfree(goes);
    return blocks;
}
