#include "binder_internal.h"
#include "../integrity/gloss.h"
#include "escheat.h"

/*
 * A Tally's end (binder.c, TALLY_DONE): the nodes it names as they lie and the
 * ground its slot will hold, made at TAKE; the slot named so in memory, the
 * Gloss let go of what its DIGESTS names — or, the slot's write or flush
 * refused, the slot held as naming either Tally. Its own file beside binder.c,
 * which keeps the Tally's steps.
 */

/* Whether `a` goes after `b` among the nodes named as they lie: by tree,
 * then by lower key — none first. */
static bool KeptAfter(const BinderKept* a, const BinderKept* b)
{
    if (a->tree != b->tree)
        return a->tree > b->tree;
    return a->at.has_lo && (!b->at.has_lo || BinderKeyLess(a->tree, b->at.lo, a->at.lo));
}

error_t BinderKeptPut(BinderRun* r, const BinderKept* k)
{
    if (!BinderListRoom32((void**)&r->kept, r->nkept, &r->capkept, sizeof(BinderKept)))
        return ERR_NO_MEMORY;
    uint32_t i = r->nkept++;
    while (i > 0 && KeptAfter(&r->kept[i - 1u], k)) {
        r->kept[i] = r->kept[i - 1u];
        i--;
    }
    r->kept[i] = *k;
    return OK;
}

error_t BinderRotOfDigests(void* ctx, const TallyPlace* at)
{
    if (EscheatDigestsLaid(at))
        return OK;     /* laid anew of the data under the act (escheat.c) */
    return BinderKeptPut((BinderRun*)ctx, &(BinderKept){ .tree = TALLY_DIGESTS, .at = *at, .rotten = true });
}

/* The nodes of FILES, TAGS and USE the Tally names as they lie because the
 * mount found no copy of them to read; the DIGESTS ones are weighed a leaf a
 * turn after the slice — a mount reads a DIGESTS leaf only when a block of it
 * is (§16.4 п.5). */
error_t BinderRotsTaken(BinderRun* r)
{
    error_t rc = OK;
    for (uint32_t i = 0; rc == OK && i < g_binder.nrot; i++)
        if (g_binder.rot[i].tree != TALLY_DIGESTS)
            rc = BinderKeptPut(r, &g_binder.rot[i]);
    return rc;
}

/* The ground the new slot holds: the nodes laid, a run a copy, and the
 * Tally-Quires of every node it names as it lies, whole (binder_quires.c) —
 * made at TAKE, so nothing is asked of the heap once the slot is on the
 * medium. */
error_t BinderFreshGround(BinderRun* r)
{
    SlotGround* f = &r->fresh;
    memset(f, 0, sizeof(*f));
    if (!(f->ground = kmalloc(sizeof(Strip) * 2u)))
        return ERR_NO_MEMORY;
    f->capground = 2u;
    return BinderKeptQuires(r);
}

static void FreshFill(BinderRun* r)
{
    SlotGround* f = &r->fresh;
    const BinderWork* w = &r->w;
    f->holds      = true;
    f->seq        = r->slot_seq;
    f->tally      = w->seq;
    f->chain_from = r->chain_at;
    f->nground    = 0;
    if (w->off) {
        f->ground[f->nground++] = (Strip){ .start = w->a, .count = (uint32_t)w->off, .kind = STRIP_DATA,
                                           .birth = w->seq };
        f->ground[f->nground++] = (Strip){ .start = w->b, .count = (uint32_t)w->off, .kind = STRIP_DATA,
                                           .birth = w->seq };
    }
    BinderSlotSort(f);
}

/* The first rotten DIGESTS range the Tally names as it lies that reaches past
 * `at`, clipped to [at, end): [*lo, *hi) — both `end` when none does. The
 * ranges are looked at from `*from` on, the blocks asked about only go up. */
static void RotDigestsFrom(const BinderWork* w, uint32_t* from, uint64_t at, uint64_t end, uint64_t* lo,
                           uint64_t* hi)
{
    *lo = *hi = end;
    for (; *from < w->nkept; (*from)++) {
        const BinderKept* r   = &w->kept[*from];
        uint64_t          rlo = r->at.has_lo ? r->at.lo.k1 : 0, rhi = r->at.has_hi ? r->at.hi.k1 : UINT64_MAX;
        if (r->tree != TALLY_DIGESTS || !r->rotten || r->taken || rhi <= at)
            continue;
        if (rlo < end) {
            *lo = rlo > at ? rlo : at;
            *hi = rhi < end ? rhi : end;
        }
        return;
    }
}

/* The Gloss let go of [at, end) — but of a block a file took since the slice:
 * the block was let go after it and taken again, and the Gloss holds its new
 * digest, which the Tally does not name (fallow.h: a block goes back to the
 * allocator without waiting for a Tally). `born` (`nb`, in order) looked at
 * from `*j` on, the blocks only going up. */
static void GlossForgetBut(TagFSLock* lock, uint64_t at, uint64_t end, const BinderGround* born, uint64_t nb,
                           uint64_t* j)
{
    while (*j < nb && born[*j].start + born[*j].count <= at)
        (*j)++;
    for (uint64_t k = *j; at < end; k++) {
        uint64_t stop = k < nb && born[k].start < end ? born[k].start : end;
        if (stop > at) {
            TagFSLockTake(lock);
            GlossForget(at, stop - at);
            TagFSLockDrop(lock);
        }
        if (k >= nb || born[k].start >= end)
            return;
        at = born[k].start + born[k].count > at ? born[k].start + born[k].count : at;
    }
}

/*
 * The Gloss let go of every digest the Tally's DIGESTS now names — the ground
 * of the slice it laid, a run at a time under the state lock; a Tally laid in
 * lays the ground of the leaves it lays and offers, and names every other
 * block as the Tally before did, whose digest the Gloss let go of then — but
 * of a block in a rotten DIGESTS range the Tally names as it lies (§16.4 п.5):
 * no element of that range is laid, and the Gloss is all that holds the digest
 * of a block written there since. It keeps those while this mount lasts —
 * said: past it they read unchecked, as the range's other blocks do, until it
 * is healed.
 */
static void GlossCaptured(const BinderWork* w)
{
    TagFSLock*          lock = &tagfs_get_state()->lock;
    uint64_t            kept = 0, nb = 0, j = 0;
    uint32_t            from = 0;
    const BinderGround* born = DogEarHeldGround(false, &nb);
    for (uint64_t i = 0; i < w->nground; i++) {
        uint64_t at = w->ground[i].start, end = at + w->ground[i].count;
        while (at < end) {
            uint64_t lo = end, hi = end;
            RotDigestsFrom(w, &from, at, end, &lo, &hi);
            if (lo > at)
                GlossForgetBut(lock, at, lo, born, nb, &j);
            kept += hi - lo;
            at    = hi;
        }
    }
    GlossTrim();
    if (kept)
        kprintf("[TagFS] %llu block(s) Tally %llu speaks for lie in a DIGESTS range it names as it lies, no copy "
                "of which reads: the digests of those written since stay in memory while this mount lasts, and "
                "past it those blocks read unchecked until the range is healed\n", (unsigned long long)kept,
                (unsigned long long)w->seq);
}

/* The blocks one copy of the Tally's whole book takes — every node it names,
 * laid or named as it lies (§16.4 п.7), in Sheets as a mount of it counts them
 * (book_ground.c, BookGroundNodeOf) and volume-read (binder.py, BindSaid): a
 * whole Tally's are the ones it laid; one laid in adds what the Foliation
 * keeps of the Tally before and the DIGESTS leaves the reads' table did, or —
 * the Foliation lost — the whole book before it, an upper bound. */
static uint64_t BookBlocks(const BinderWork* w)
{
    if (!w->tip)
        return w->sheets;
    if (FoliationLost())
        return g_binder.tally_blocks + w->sheets;
    return w->sheets + FoliationBlocksOlder(w->seq) + w->dsheets;
}

error_t BinderTallyDone(BinderRun* r)
{
    BinderWork* w   = &r->w;
    SlotGround  old = g_binder.slot[r->slot];
    FreshFill(r);
    g_binder.slot[r->slot] = r->fresh;
    memset(&r->fresh, 0, sizeof(r->fresh));
    if (r->slot + 1u > g_binder.nslots)
        g_binder.nslots = r->slot + 1u;
    BinderSlotOwnPublish(r->slot, 0);
    uint64_t gone = BinderSlotGoes(&old);
    BinderSlotLetGo(&old);
    if (r->captured && !DogEarHeldLost())
        GlossCaptured(w);
    else if (r->captured)
        kprintf("[TagFS] DogEar lost the ground files took since Tally %llu's slice to a heap that refused it: the "
                "Gloss keeps every digest it holds until the next Tally\n", (unsigned long long)w->seq);
    DogEarTakenLetGo();
    FoliationDraftDone(w->seq, r->captured);
    RibbonTallied(w->seq, r->k);
    EndpaperTallyLaid(&w->laid);
    BinderPairUsed(r);
    g_binder.useful      -= g_binder.useful >= r->useful_at ? r->useful_at : g_binder.useful;
    g_binder.tally_blocks = BookBlocks(w);
    g_binder.cost_known   = 0;
    g_binder.tallies++;
    kprintf("[TagFS] Tally %llu is written over slot %u: the state after stitch %llu, %llu node(s) laid in %llu "
            "block(s) a copy, %llu named as they lie%s; %llu block(s) no slot holds any more rest in Fallow%s\n",
            (unsigned long long)w->seq, r->slot, (unsigned long long)r->k, (unsigned long long)w->nodes,
            (unsigned long long)w->off, (unsigned long long)w->named, w->tip ? " (laid in)" : "",
            (unsigned long long)gone, w->dropped ? "; DIGESTS elements a rotten range covers left out" : "");
    EscheatDone(w->seq);
    BinderGleanAt(r);
    BinderProofAt(r);
    return OK;
}

/* Slot `s`'s Tally-Quires joined with `f`'s, in order, a Quire once: false
 * with no memory. */
static bool QuiresJoined(SlotGround* s, const SlotGround* f)
{
    uint32_t n = s->nquires + f->nquires, k = 0;
    Strip*   both = n ? kmalloc(sizeof(Strip) * n) : NULL;
    if (n && !both)
        return false;
    for (uint32_t i = 0, j = 0; i < s->nquires || j < f->nquires;) {
        bool     mine = j == f->nquires || (i < s->nquires && s->quires[i].start <= f->quires[j].start);
        Strip    x    = mine ? s->quires[i++] : f->quires[j++];
        uint64_t last = k ? both[k - 1u].start + both[k - 1u].count : 0;
        if (k && last >= x.start + x.count)
            continue;
        if (k && last >= x.start && last - both[k - 1u].start + (x.start + x.count - last) <= UINT32_MAX) {
            both[k - 1u].count = (uint32_t)(x.start + x.count - both[k - 1u].start);
            continue;
        }
        if (k && last > x.start) {
            x.count -= (uint32_t)(last - x.start);
            x.start  = last;
        }
        both[k++] = x;
    }
    kfree(s->quires);
    s->quires    = both;
    s->nquires   = k;
    s->capquires = n;
    return true;
}

/* A slot whose write or flush was refused may hold the Tally it held or the
 * new one, whole (C9, C10): held as holding both until a Tally is written over
 * it, and written over first — the new one's unit as sealed kept with it, each
 * one of every write refused while it is so. With no memory to hold it so, no
 * slot holds the Quires the new Tally names a node in as it lies — so no Tally
 * is written by this mount again, and no Quire is given back until the volume
 * is mounted again. */
void BinderSlotUnsure(BinderRun* r)
{
    SlotGround* s = &g_binder.slot[r->slot];
    FreshFill(r);
    BinderPairUsed(r);
    uint32_t n    = s->nground + r->fresh.nground;
    Strip*   both = kmalloc(sizeof(Strip) * n);
    if (!both || !QuiresJoined(s, &r->fresh)) {
        kfree(both);
        kprintf("[TagFS] no memory to hold slot %u as naming either Tally — the new Tally's ground is held until "
                "the volume is mounted again\n", r->slot);
        BinderNoTalliesFrom("memory would not hold a slot that may name either of two Tallies");
        return;
    }
    if (s->nground)
        memcpy(both, s->ground, sizeof(Strip) * s->nground);
    memcpy(both + s->nground, r->fresh.ground, sizeof(Strip) * r->fresh.nground);
    kfree(s->ground);
    s->ground    = both;
    s->nground   = n;
    s->capground = n;
    BinderSlotSort(s);
    s->unsure = true;
    s->tally2 = r->w.seq;
    if (r->sealed) {
        r->sealed->next = s->sealed;
        s->sealed       = r->sealed;
        r->sealed       = NULL;
    }
    if (!s->holds) {
        s->holds = true;
        s->tally = r->w.seq;
    }
    if (r->chain_at < s->chain_from)
        s->chain_from = r->chain_at;
    if (r->slot + 1u > g_binder.nslots)
        g_binder.nslots = r->slot + 1u;
    BinderSlotOwnPublish(r->slot, 0);
}

bool BinderSlotSealed(uint32_t slot, const uint8_t* unit)
{
    if (slot >= g_binder.ring || !g_binder.slot[slot].unsure)
        return false;
    for (const SlotSealed* u = g_binder.slot[slot].sealed; u; u = u->next)
        if (memcmp(u->unit, unit, sizeof(u->unit)) == 0)
            return true;
    return false;
}
