#include "binder_internal.h"

/*
 * DIGESTS laid in among the nodes of the Tally before (TipIn, spec_v2 §16.5
 * п.16): the leaves it lays anew and offers, the ground it speaks for in them,
 * and the reads' table its DONE turns the reads of a digest to.
 *
 * ‼ THE GROUND OF A LEAF LAID IN IS ITS OLD ELEMENTS, LESS WHAT WENT, WITH WHAT
 * CAME (§4.3 TALLY_TAKE, H-5). A whole Tally's DIGESTS walks every run of
 * every live file — as many as the volume holds. One laid in reads the leaves
 * it lays and offers, a leaf a turn of the Settle's holder (TallyLeafRead):
 * the blocks their elements name are the ground of their range as the Tally
 * before saw it; DogEar holds, a block exactly, the ground the files let go
 * since and the ground they took (dog_ear.h, DogEarTakenGround) — a block let
 * go and taken again in both, its data new. The ground of the ranges laid in
 * is the first less the second with the third, and the digest of each block
 * of it is found as a whole Tally finds it: the Gloss's, else the Tally
 * before's. A leaf that reads in no copy cannot be laid (§16.4 п.5): the tree
 * is sifted again with it named as it lies, and the leaves the new sifting
 * lays and offers are read in their turn.
 *
 * ‼ WHERE THE BINDER CHECKS, THE TWO GROUNDS ARE HELD TO EACH OTHER. A STRICT
 * kernel's walk hands every element of the state to the self-check
 * (binder_check.c), so it walks the ground of every live file; in the ranges
 * a Tally laid in lays and offers, each element of that walk is the next one
 * the ground laid in speaks for, byte for byte, and none of the second is left
 * at the tree's end — or the ground laid in is a lie: a DEFECT. The stand's
 * kernel lays what a release kernel lays only so.
 */

// ----------------------------------------------------------------------------
// The leaves laid and offered, read a turn each
// ----------------------------------------------------------------------------

/* A leaf being read: its runs put in, until an element does not read — the
 * Tally before's reads find nothing of the leaf past it (volume_tally.h,
 * TallyDigestIn), and a whole Tally lays none of its blocks: said, as the
 * reads and a whole Tally say it, and the leaf laid anew without them — and
 * whether no copy of it reads. */
typedef struct {
    BinderRun* r;
    uint32_t   leaf;
    bool       broken;
    bool       rotten;
    bool       refused;
} OldRead;

static error_t OldItem(void* ctx, TallyKey key, const uint8_t* body, uint32_t len, uint64_t seq)
{
    OldRead*       c = ctx;
    BinderRun*     r = c->r;
    uint64_t       n = 0;
    const uint8_t* d = NULL;
    (void)seq;
    if (c->broken)
        return OK;
    if (TallyDigestsGet(body, len, &n, &d) == TALLY_BODY_BROKEN) {
        const TallyPlace* p = &r->digests.leaf[c->leaf].place;
        c->broken = true;
        kprintf("[TagFS] an element of the DIGESTS leaf of blocks from %llu does not read at block %llu: Tally %llu "
                "lays the leaf anew without the digests of the blocks from there to its end, which read unchecked "
                "until now and carry no digest past it\n", (unsigned long long)(p->has_lo ? p->lo.k1 : 0),
                (unsigned long long)key.k1, (unsigned long long)r->w.seq);
        return OK;
    }
    if (n == 0)
        return OK;
    if (!BinderListRoom((void**)&r->old, r->nold, &r->capold, sizeof(BinderOldRun)))
        return ERR_NO_MEMORY;
    r->old[r->nold++] = (BinderOldRun){ key.k1, n, c->leaf };
    return OK;
}

/* No copy of the leaf reads: said, and the Tally names it as it lies — its
 * parent laid anew, what was born in its range left out and said
 * (tip_in.c, Touched; binder_done.c, GlossCaptured). */
static error_t OldRotten(void* ctx, const TallyPlace* at, const char* why)
{
    OldRead* c = ctx;
    c->rotten  = true;
    kprintf("[TagFS] no copy of the DIGESTS leaf of blocks [%llu, %s%llu) reads (%s): Tally %llu names it as it "
            "lies\n", (unsigned long long)(at->has_lo ? at->lo.k1 : 0), at->has_hi ? "" : "the end, ",
            (unsigned long long)(at->has_hi ? at->hi.k1 : tagfs_get_state()->block_end), why,
            (unsigned long long)c->r->w.seq);
    return OK;
}

/* A copy of a leaf that did not hold: the next Tally lays its range again —
 * this one may name it as it lies, an offer the row did not take. */
static void OldMended(void* ctx, uint64_t tree, const TallyPlace* at, uint32_t failed)
{
    (void)ctx;
    (void)failed;
    DogEarMended(tree, at);
}

/* Whether sifted leaf `i` is laid or offered, reads as far as is known, and
 * is not read yet. */
static bool ReadWanted(const BinderRun* r, uint32_t i)
{
    const BinderSift* s = &r->digests;
    return (s->lf[i] == TIP_IN_LAID || s->lf[i] == TIP_IN_MARGIN) && !s->leaf[i].rotten && r->dread[i] == 0;
}

error_t BinderDigestsReadNext(BinderRun* r, bool* done)
{
    BinderSift* s = &r->digests;
    *done = false;
    if (!r->dread && s->n) {
        if (!(r->dread = kmalloc(s->n)))
            return ERR_NO_MEMORY;
        memset(r->dread, 0, s->n);
    }
    while (r->dread_at < s->n && !ReadWanted(r, r->dread_at))
        r->dread_at++;
    if (r->dread_at == s->n) {
        *done = true;
        return OK;
    }
    uint32_t  i    = r->dread_at;
    OldRead   c    = { .r = r, .leaf = i };
    TallySink sink = { .ctx = &c, .item = OldItem, .rotten = OldRotten, .mend = OldMended };
    error_t   rc   = TallyLeafRead(TALLY_DIGESTS, &s->leaf[i].place, &sink);
    if (rc != OK)
        return rc;
    r->dread[i] = c.rotten ? 2u : 1u;
    if (!c.rotten)
        return OK;
    s->leaf[i].rotten = true;
    r->dread_at       = 0;
    return BinderSiftAgain(s);
}

// ----------------------------------------------------------------------------
// The ground laid in
// ----------------------------------------------------------------------------

static error_t RunPut(BinderGround** list, uint64_t* n, uint64_t* cap, uint64_t start, uint64_t end)
{
    if (start >= end)
        return OK;
    if (*n && (*list)[*n - 1u].start + (*list)[*n - 1u].count == start) {
        (*list)[*n - 1u].count += end - start;
        return OK;
    }
    if (!BinderListRoom((void**)list, *n, cap, sizeof(BinderGround)))
        return ERR_NO_MEMORY;
    (*list)[(*n)++] = (BinderGround){ start, end - start };
    return OK;
}

static uint64_t RunEnd(const BinderGround* g)
{
    return g->start + g->count;
}

/* The ranges of blocks the leaves laid and offered cover — every block when
 * the Tally before has no DIGESTS — in order, those that meet joined. */
static error_t RangesLaid(BinderRun* r, BinderGround** out, uint64_t* n)
{
    const BinderSift* s   = &r->digests;
    uint64_t          cap = 0;
    error_t           rc  = OK;
    *n = 0;
    if (s->n == 0 && s->ninner == 0)
        return RunPut(out, n, &cap, 0, UINT64_MAX);
    for (uint32_t i = 0; rc == OK && i < s->n; i++) {
        const TallyPlace* p = &s->leaf[i].place;
        if (s->lf[i] == TIP_IN_LAID || s->lf[i] == TIP_IN_MARGIN)
            rc = RunPut(out, n, &cap, s->at[i] == 0 || !p->has_lo ? 0 : p->lo.k1, p->has_hi ? p->hi.k1 : UINT64_MAX);
    }
    return rc;
}

/* The runs the elements of the leaves laid and offered name, less the ground
 * let go since, into `out` — in order, joined. */
static error_t OldKept(BinderRun* r, BinderGround** out, uint64_t* n, uint64_t* cap)
{
    const BinderSift*   s    = &r->digests;
    uint64_t            ng   = 0, m = 0, mcap = 0, j = 0;
    const BinderGround* gone = DogEarTakenGround(true, &ng);
    BinderGround*       old  = NULL;
    error_t             rc   = OK;
    for (uint64_t k = 0; rc == OK && k < r->nold; k++)
        if (s->lf[r->old[k].leaf] == TIP_IN_LAID || s->lf[r->old[k].leaf] == TIP_IN_MARGIN)
            rc = RunPut(&old, &m, &mcap, r->old[k].start, r->old[k].start + r->old[k].count);
    m = rc == OK ? BinderGroundJoined(old, m) : 0;
    for (uint64_t i = 0; rc == OK && i < m; i++) {
        uint64_t at = old[i].start;
        while (j < ng && RunEnd(&gone[j]) <= at)
            j++;
        for (uint64_t g = j; rc == OK && g < ng && gone[g].start < RunEnd(&old[i]); g++) {
            rc = RunPut(out, n, cap, at, gone[g].start > at ? gone[g].start : at);
            at = RunEnd(&gone[g]) > at ? RunEnd(&gone[g]) : at;
        }
        if (rc == OK && at < RunEnd(&old[i]))
            rc = RunPut(out, n, cap, at, RunEnd(&old[i]));
    }
    kfree(old);
    return rc;
}

/* The ground the files took since in ranges `rg` (`nr`, in order), into `out`. */
static error_t BornIn(const BinderGround* rg, uint64_t nr, BinderGround** out, uint64_t* n, uint64_t* cap)
{
    uint64_t            nb = 0, j = 0;
    const BinderGround* born = DogEarTakenGround(false, &nb);
    error_t             rc   = OK;
    for (uint64_t i = 0; rc == OK && i < nb; i++) {
        while (j < nr && RunEnd(&rg[j]) <= born[i].start)
            j++;
        for (uint64_t k = j; rc == OK && k < nr && rg[k].start < RunEnd(&born[i]); k++)
            rc = RunPut(out, n, cap, born[i].start > rg[k].start ? born[i].start : rg[k].start,
                        RunEnd(&born[i]) < RunEnd(&rg[k]) ? RunEnd(&born[i]) : RunEnd(&rg[k]));
    }
    return rc;
}

/* The ranges the ground the files took is looked for in: the ones laid and
 * offered, and — `rotten` — every rotten range the Tally names as it lies: a
 * block of it a file took is left out of DIGESTS, said, as a whole Tally
 * leaves it, its digest the Gloss's to keep (binder_done.c, GlossCaptured).
 * In order, joined. */
static error_t BornRanges(const BinderRun* r, const BinderGround* laid, uint64_t nl, bool rotten, BinderGround** out,
                          uint64_t* n)
{
    uint64_t cap = 0;
    *n = 0;
    for (uint64_t i = 0; i < nl; i++)
        if (RunPut(out, n, &cap, laid[i].start, RunEnd(&laid[i])) != OK)
            return ERR_NO_MEMORY;
    for (uint32_t i = 0; rotten && i < r->nkept; i++) {
        const BinderKept* k = &r->kept[i];
        if (k->tree == TALLY_DIGESTS && k->rotten && !k->taken &&
            RunPut(out, n, &cap, k->at.has_lo ? k->at.lo.k1 : 0, k->at.has_hi ? k->at.hi.k1 : UINT64_MAX) != OK)
            return ERR_NO_MEMORY;
    }
    *n = BinderGroundJoined(*out, *n);
    return OK;
}

/* The ground laid in: its runs and the ranges it lays and offers — where the
 * Binder checks, held apart from the walk's ground of every live file. */
static error_t GroundLaidIn(BinderRun* r)
{
    BinderWork*   w = &r->w;
    BinderGround *laid = NULL, *born = NULL, *g = NULL;
    uint64_t      nl = 0, nb = 0, n = 0, cap = 0;
    error_t       rc = RangesLaid(r, &laid, &nl);
    /* ‼ HELD TO THE WALK, THE GROUND LAID IN IS THE LAID RANGES' ALONE. The
     * walk's ground leaves out every element a rotten range the Tally names as
     * it lies covers (binder_items.c, `dropped`), a block a file took there
     * since among them; the self-check holds this ground to that walk's in the
     * ranges laid and offered (BinderDigestsHeld), and a born block of a
     * rotten range left in it is met by no element of the walk — a DEFECT of
     * the check, not of the Tally. The release kernel's ground keeps those
     * blocks: they are the Gloss's to keep (GlossCaptured). */
    if (rc == OK)
        rc = BornRanges(r, laid, nl, !BINDER_CHECKS, &born, &nb);
    if (rc == OK)
        rc = OldKept(r, &g, &n, &cap);
    if (rc == OK)
        rc = BornIn(born, nb, &g, &n, &cap);
    kfree(born);
    if (rc != OK) {
        kfree(laid);
        kfree(g);
        return rc;
    }
    n = BinderGroundJoined(g, n);
#if BINDER_CHECKS
    w->tground  = g;
    w->ntground = n;
    w->dranges  = laid;
    w->ndranges = nl;
    if (!(w->items.tip_buf = kmalloc(BINDER_DIGEST_ITEM_BYTES)))
        return ERR_NO_MEMORY;
#else
    kfree(laid);
    w->ground  = g;
    w->nground = n;
#endif
    return OK;
}

error_t BinderDigestsSifted(BinderRun* r)
{
    error_t rc = BinderSiftTaken(r, &r->digests);
    if (rc == OK)
        rc = GroundLaidIn(r);
    BinderDigestsLetGo(r);
    return rc;
}

void BinderDigestsLetGo(BinderRun* r)
{
    BinderSiftLetGo(&r->digests);
    kfree(r->dread);
    kfree(r->old);
    r->dread    = NULL;
    r->dread_at = 0;
    r->old      = NULL;
    r->nold     = 0;
    r->capold   = 0;
}

// ----------------------------------------------------------------------------
// The leaves of the Tally before under the nodes named as they lie
// ----------------------------------------------------------------------------

/* The places of the reads' table [*from, *to) a DIGESTS node named as it lies
 * holds — itself alone when it is a leaf, or reads in no copy. */
static void KeptLeaves(const BinderKept* k, uint32_t n, uint32_t* from, uint32_t* to)
{
    if (k->rotten || k->at.level == 0) {
        *from = 0;
        *to   = 1;
        return;
    }
    *from = k->at.has_lo && n ? BinderSiftLeafOf(TALLY_DIGESTS, n, k->at.lo) : 0;
    *to   = k->at.has_hi ? BinderSiftLeafFrom(TALLY_DIGESTS, *from + 1u < n ? *from + 1u : n, n, k->at.hi) : n;
}

/* The `i`-th place a node named as it lies gives the reads' table: false for
 * a place past the table's end, which KeptLeaves never gives. */
static bool KeptLeaf(const BinderKept* k, uint32_t i, TallyPlace* p, bool* rotten)
{
    if (k->rotten || k->at.level == 0) {
        *p      = k->at;
        *rotten = k->rotten;
    } else if (!BinderSiftLeafAt(TALLY_DIGESTS, i, p, rotten)) {
        return false;
    }
    if (!p->has_lo)
        p->lo = (TallyKey){ 0, 0 };
    return true;
}

/* The next DIGESTS node named as it lies from `*r` on, or NULL. */
static const BinderKept* KeptNext(const BinderWork* w, uint32_t* r)
{
    for (; *r < w->nkept; (*r)++)
        if (w->kept[*r].tree == TALLY_DIGESTS && !w->kept[*r].taken)
            return &w->kept[*r];
    return NULL;
}

/*
 * ‼ THE READS TURN TO EVERY LEAF THE TALLY NAMES (§16.4 п.5, §16.5 п.9). A
 * Tally laid in names the leaves of the Tally before under each node it names
 * as it lies: the reads' table it turns them to holds those as they lay —
 * their places, a range no copy of whose node reads too — among the leaves it
 * laid, in the order of their keys.
 */
error_t BinderDigestsTipTurn(const BinderWork* w)
{
    uint32_t    nt = TallyDigestLeaves(), n = w->ndleaves, from = 0, to = 0, r = 0, k = 0, at = 0;
    const BinderKept* kept;
    for (uint32_t i = 0; (kept = KeptNext(w, &i)) != NULL; i++) {
        KeptLeaves(kept, nt, &from, &to);
        n += to - from;
    }
    TallyPlace* places = kmalloc(sizeof(TallyPlace) * (n ? n : 1u));
    bool*       rotten = kmalloc(sizeof(bool) * (n ? n : 1u));
    error_t     rc     = places && rotten && !w->dleaves_lost ? OK : ERR_NO_MEMORY;
    while (rc == OK && at < n) {
        kept = KeptNext(w, &r);
        if (kept && (k == w->ndleaves || (kept->at.has_lo ? kept->at.lo.k1 : 0) < w->dleaves[k].key.k1)) {
            KeptLeaves(kept, nt, &from, &to);
            for (uint32_t i = from; rc == OK && i < to && at < n; i++, at++)
                if (!KeptLeaf(kept, i, &places[at], &rotten[at])) {
                    kprintf("[TagFS] DEFECT: the reads' DIGESTS table has no place %u under a node Tally %llu "
                            "names as it lies — the reads stay with the Tally before it and the Gloss\n", i,
                            (unsigned long long)w->seq);
                    rc = ERR_TAGFS_CORRUPTED;
                }
            r++;
            continue;
        }
        const BinderKid* kid = &w->dleaves[k++];
        memset(&places[at], 0, sizeof(TallyPlace));
        places[at].ptr   = (TallyPtr){ .seq = w->seq, .sheets = kid->sheets, .copies = 2u, .more = 0,
                                       .block = { kid->block[0], kid->block[1] } };
        places[at].count = kid->count;
        places[at].lo    = kid->key;
        rotten[at++]     = false;
    }
    if (rc == OK) {
        TipInLeavesClosed(places, n);
        rc = TallyDigestsTurn(places, rotten, n);
    }
    kfree(places);
    kfree(rotten);
    return rc;
}

void BinderDigestsKeptLaid(BinderWork* w)
{
    uint32_t          nt = TallyDigestLeaves(), from = 0, to = 0;
    const BinderKept* kept;
    TallyPlace        p;
    bool              rotten = false;
    for (uint32_t r = 0; (kept = KeptNext(w, &r)) != NULL; r++) {
        if (kept->rotten) {
            w->dsheets += kept->at.level == 0 ? kept->at.ptr.sheets : 0;
            continue;
        }
        KeptLeaves(kept, nt, &from, &to);
        for (uint32_t i = from; i < to && KeptLeaf(kept, i, &p, &rotten); i++) {
            w->dsheets += p.level == 0 ? p.ptr.sheets : 0;
            if (p.level == 0 && !rotten)
                EndpaperLaidNode(&w->laid, TALLY_DIGESTS, 0, p.ptr.sheets, w->grid, p.lo.k1);
            else if (p.level > 0)
                EndpaperLaidNode(&w->laid, TALLY_DIGESTS, p.level, 0, w->grid, p.lo.k1);
        }
    }
}

#if BINDER_CHECKS
error_t BinderDigestsHeld(BinderWork* w, const BinderItem* it, bool end)
{
    BinderItems* p   = &w->items;
    BinderItem   tip = { 0 };
    bool         got = false;
    if (!end) {
        while (p->tip_range < w->ndranges && RunEnd(&w->dranges[p->tip_range]) <= it->key.k1)
            p->tip_range++;
        if (p->tip_range == w->ndranges || w->dranges[p->tip_range].start > it->key.k1)
            return OK;
    }
    error_t rc = BinderDigestsElement(w, w->tground, w->ntground, &p->tip_at, p->tip_buf, &tip, &got);
    if (rc != OK || (end && !got))
        return rc;
    if (!end && got && tip.key.k1 == it->key.k1 && tip.len == it->len && memcmp(tip.body, it->body, it->len) == 0)
        return OK;
    char state[32] = "none", laid[32] = "none";
    if (!end)
        ksnprintf(state, sizeof(state), "block %llu", (unsigned long long)it->key.k1);
    if (got)
        ksnprintf(laid, sizeof(laid), "block %llu", (unsigned long long)tip.key.k1);
#if CONFIG_STRICT
    panic("[TagFS] DEFECT: DIGESTS laid in — the state's next element at %s, the ground laid in at %s (Tally %llu)",
          state, laid, (unsigned long long)w->seq);
#endif
    kprintf("[TagFS] DEFECT: DIGESTS laid in — where the state's next element lies at %s, the ground laid in "
            "speaks for one at %s, or another: the ground the files took and let go since is not what DogEar says, "
            "and Tally %llu is not written\n", state, laid, (unsigned long long)w->seq);
    return ERR_TAGFS_CORRUPTED;
}
#endif
