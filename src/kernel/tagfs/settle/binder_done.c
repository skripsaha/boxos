#include "binder_internal.h"
#include "../integrity/gloss.h"

/*
 * A Tally's end (binder.c, TALLY_DONE): the nodes it names as they lie and the
 * ground its slot will hold, made at TAKE; the slot named so in memory, the
 * Gloss let go of what its DIGESTS names — or, the slot's write or flush
 * refused, the slot held as naming either Tally. Its own file beside binder.c,
 * which keeps the Tally's steps.
 */

/* A rotten node more for the Tally under way, in the order of tree and key. */
static error_t RotPut(BinderRun* r, uint64_t tree, const TallyPlace* at)
{
    if (!BinderListRoom32((void**)&r->rot, r->nrot, &r->caprot, sizeof(BinderRot)))
        return ERR_NO_MEMORY;
    uint32_t i = r->nrot++;
    while (i > 0 && (r->rot[i - 1u].tree > tree ||
                     (r->rot[i - 1u].tree == tree && at->has_lo &&
                      (!r->rot[i - 1u].at.has_lo || r->rot[i - 1u].at.lo.k1 > at->lo.k1 ||
                       (r->rot[i - 1u].at.lo.k1 == at->lo.k1 && r->rot[i - 1u].at.lo.k2 > at->lo.k2))))) {
        r->rot[i] = r->rot[i - 1u];
        i--;
    }
    r->rot[i] = (BinderRot){ .tree = tree, .at = *at };
    return OK;
}

error_t RotOfDigests(void* ctx, const TallyPlace* at)
{
    return RotPut((BinderRun*)ctx, TALLY_DIGESTS, at);
}

/* The FILES nodes the Tally names as they lie, as the mount found them; the
 * DIGESTS ones are weighed a leaf a turn after the slice — a mount reads a
 * DIGESTS leaf only when a block of it is (§16.4 п.5). */
error_t RotsTaken(BinderRun* r)
{
    error_t rc = OK;
    for (uint32_t i = 0; rc == OK && i < g_binder.nrot; i++)
        if (g_binder.rot[i].tree != TALLY_DIGESTS)
            rc = RotPut(r, g_binder.rot[i].tree, &g_binder.rot[i].at);
    return rc;
}

/* The ground the new slot holds: the two runs, and every rotten node it names
 * as it lies, each copy at its birth — made at TAKE, so nothing is asked of
 * the heap once the slot is on the medium. */
error_t FreshGround(BinderRun* r)
{
    SlotGround* f    = &r->fresh;
    uint32_t    need = 2u;
    for (uint32_t i = 0; i < r->nrot; i++)
        need += r->rot[i].at.ptr.copies;
    memset(f, 0, sizeof(*f));
    if (!(f->ground = kmalloc(sizeof(Strip) * need)))
        return ERR_NO_MEMORY;
    f->capground = need;
    return OK;
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
    f->ground[f->nground++] = (Strip){ .start = w->a, .count = (uint32_t)w->room, .kind = STRIP_DATA, .birth = w->seq };
    f->ground[f->nground++] = (Strip){ .start = w->b, .count = (uint32_t)w->room, .kind = STRIP_DATA, .birth = w->seq };
    for (uint32_t i = 0; i < r->nrot; i++)
        for (uint32_t k = 0; k < r->rot[i].at.ptr.copies; k++)
            f->ground[f->nground++] = (Strip){ .start = TallyPtrBlock(&r->rot[i].at.ptr, k),
                                               .count = (uint32_t)r->rot[i].at.ptr.sheets,
                                               .kind = STRIP_DATA, .birth = r->rot[i].at.ptr.seq };
    BinderSlotSort(f);
}

/* The first DIGESTS range the Tally names as it lies that reaches past `at`,
 * clipped to [at, end): [*lo, *hi) — both `end` when none does. */
static void RotDigestsFrom(const BinderWork* w, uint64_t at, uint64_t end, uint64_t* lo, uint64_t* hi)
{
    *lo = *hi = end;
    for (uint32_t i = 0; i < w->nrot; i++) {
        const BinderRot* r   = &w->rot[i];
        uint64_t         rlo = r->at.has_lo ? r->at.lo.k1 : 0, rhi = r->at.has_hi ? r->at.hi.k1 : UINT64_MAX;
        if (r->tree != TALLY_DIGESTS || rhi <= at || rlo >= *lo)
            continue;
        *lo = rlo > at ? rlo : at;
        *hi = rhi < end ? rhi : end;
    }
}

/*
 * The Gloss let go of every digest the Tally's DIGESTS now names — the ground
 * of the slice, a run at a time under the state lock — but of a block in a
 * DIGESTS range the Tally names as it lies (§16.4 п.5): no element of that
 * range is laid, and the Gloss is all that holds the digest of a block
 * written there since. It keeps those while this mount lasts — said: past it
 * they read unchecked, as the range's other blocks do, until it is healed.
 */
static void GlossCaptured(const BinderWork* w)
{
    TagFSLock* lock = &tagfs_get_state()->lock;
    uint64_t   kept = 0;
    for (uint64_t i = 0; i < w->nground; i++) {
        uint64_t at = w->ground[i].start, end = at + w->ground[i].count;
        while (at < end) {
            uint64_t lo = end, hi = end;
            RotDigestsFrom(w, at, end, &lo, &hi);
            if (lo > at) {
                TagFSLockTake(lock);
                GlossForget(at, lo - at);
                TagFSLockDrop(lock);
            }
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

error_t TallyDone(BinderRun* r)
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
    if (r->captured)
        GlossCaptured(w);
    RibbonTallied(w->seq);
    EndpaperTallyLaid(&w->laid);
    g_binder.useful      -= g_binder.useful >= r->useful_at ? r->useful_at : g_binder.useful;
    g_binder.tally_blocks = w->off;
    g_binder.tallies++;
    kprintf("[TagFS] Tally %llu is written over slot %u: the state after stitch %llu, %llu node(s) in %llu "
            "block(s) a copy; %llu block(s) no slot holds any more rest in Fallow%s\n", (unsigned long long)w->seq,
            r->slot, (unsigned long long)r->k, (unsigned long long)w->nodes, (unsigned long long)w->off,
            (unsigned long long)gone, w->dropped ? "; DIGESTS elements a rotten range covers left out" : "");
    return OK;
}

/* A slot whose write or flush was refused may hold the Tally it held or the
 * new one, whole (C9, C10): held as holding both until a Tally is written over
 * it, and written over first. */
void SlotUnsure(BinderRun* r)
{
    SlotGround* s = &g_binder.slot[r->slot];
    FreshFill(r);
    uint32_t n    = s->nground + r->fresh.nground;
    Strip*   both = kmalloc(sizeof(Strip) * n);
    if (!both) {
        kprintf("[TagFS] no memory to hold slot %u as naming either Tally — the new Tally's ground is held until "
                "the volume is mounted again\n", r->slot);
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
