#include "binder.h"
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

void BinderMountReset(void)
{
    BinderStopped(ERR_DEVICE_NOT_READY);
    DogEarLetGo();
    FoliationLetGo();
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

error_t BinderSlotHolds(uint32_t index, uint64_t seq, uint64_t tally_seq)
{
    if (index >= g_binder.ring)
        return ERR_INVALID_ARGUMENT;
    SlotGround* s = &g_binder.slot[index];
    s->holds = true;
    s->seq   = seq;
    s->tally = tally_seq;
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

/* A slot's own Tallies' ground: its nodes born at the Tally it names, or at
 * the one it may name instead — what a Tally written over it gives back. A
 * node named as it lies is every Tally's (QuireHeld), and goes back with
 * none. */
static uint64_t SlotOwn(const SlotGround* s)
{
    uint64_t own = 0;
    for (uint32_t i = 0; s->holds && i < s->nground; i++)
        if (s->ground[i].birth == s->tally || (s->tally2 && s->ground[i].birth == s->tally2))
            own += s->ground[i].count;
    return own;
}

void BinderSlotOwnPublish(uint32_t index, uint64_t more)
{
    if (index < g_binder.ring)
        __atomic_store_n(&g_binder.own[index], SlotOwn(&g_binder.slot[index]) + more, __ATOMIC_RELEASE);
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

static bool QuireHeld(uint64_t start, uint64_t end, uint64_t q)
{
    if (SlotsHold(start, end))
        return true;
    if (!g_binder.pair_room)
        return false;
    uint64_t used = g_binder.pair_used / q * q;
    return (start < g_binder.pair_a + g_binder.pair_room && g_binder.pair_a + used < end) ||
           (start < g_binder.pair_b + g_binder.pair_room && g_binder.pair_b + used < end);
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
