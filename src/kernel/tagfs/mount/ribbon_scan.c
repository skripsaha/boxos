#include "ribbon.h"
#include "book_mount.h"
#include "book_unit.h"

/*
 * RibbonScan — the direct scan past the end of the chain a slot's ribbon was
 * sewn into (ribbon_collate.c, spec_v2 §2.2), and what each branch it finds
 * there is (§3.9, §16.3 пп.10–11): every unit of a seq past the last stitch
 * taken in the rest of the last Quire and all of its next, joined to the one
 * its StitchHead names, each branch said by the census's own codes — word for
 * word volume-read's (tools/volumeread/stitch_ribbon.py Ribbon._Scan,
 * _Branches).
 */

/* The unit of seq `seq` past the end, or NULL. */
static RibbonUnit* RibbonPastOf(const RibbonWalk* w, uint64_t seq)
{
    uint32_t lo = 0, hi = w->npast;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (w->past[mid]->seq == seq)
            return w->past[mid];
        if (w->past[mid]->seq < seq)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return NULL;
}

/* A unit past the end into the walk's list, kept in the order of its seq.
 * False with no memory (the unit let go). */
static bool RibbonPastAdd(RibbonWalk* w, RibbonUnit* u, uint32_t* cap)
{
    if (w->npast == *cap) {
        uint32_t     more = *cap ? *cap * 2u : 8u;
        RibbonUnit** to   = kmalloc(sizeof(RibbonUnit*) * more);
        if (!to) {
            RibbonUnitLetGo(u);
            return false;
        }
        if (w->npast)
            memcpy(to, w->past, sizeof(RibbonUnit*) * w->npast);
        kfree(w->past);
        w->past = to;
        *cap    = more;
    }
    uint32_t at = w->npast;
    while (at > 0 && w->past[at - 1u]->seq > u->seq) {
        w->past[at] = w->past[at - 1u];
        at--;
    }
    w->past[at] = u;
    w->npast++;
    return true;
}

/* Why a unit is no stitch the format allows (§16.3 п.8), in stitch.py's
 * words (Misshapen). */
static void RibbonShapeSaid(const RibbonUnit* u, char* out, size_t room)
{
    uint64_t space = RibbonPlaceLeft(&u->start) + u->hop.blocks * VOLUME_BLOCK_SECTORS;
    if (u->shape == RIBBON_SHAPE_ATOMS)
        ksnprintf(out, room, "%u sector(s) long — no whole number of atoms of %u", u->of, VOLUME_BLOCK_SECTORS);
    else if (u->shape == RIBBON_SHAPE_OFF)
        ksnprintf(out, room, "begun at sector %llu of its Quire, off a block's boundary",
                  (unsigned long long)u->start.off);
    else if (u->shape == RIBBON_SHAPE_LONG)
        ksnprintf(out, room, "%u sectors long, more than its Quire and the next hold (%llu)", u->of,
                  (unsigned long long)space);
    else
        ksnprintf(out, room, "%u sectors long — a stream of more than %u bytes, past what a reader holds a "
                  "stitch in", u->of, RIBBON_STREAM_MOST);
}

/* Every sector of the rest of the Quire the chain ends in and all of its
 * next_quire: each unit of a seq past the chain's that begins there, read
 * whole — the first sector of a seq that lies at or past its own `at` from
 * the end names where it begins. */
static error_t RibbonScanUnits(RibbonWalk* w, uint32_t* cap)
{
    uint64_t room = RibbonPlaceLeft(&w->end) + w->next.blocks * VOLUME_BLOCK_SECTORS;
    for (uint64_t i = 0; i < room;) {
        RibbonPlace p;
        BookUnit    run;
        if (!RibbonPlaceOn(&w->end, i, &w->next, &p))
            break;
        uint64_t n  = RibbonPlaceLeft(&p) < 256u ? RibbonPlaceLeft(&p) : 256u;
        error_t  rc = BookUnitRead(RibbonPlaceVlba(&p), (uint32_t)n, 0, 0, &run);
        for (uint32_t k = 0; rc == OK && k < n; k++) {
            BookSectorHead h;
            RibbonPlace    at;
            RibbonUnit*    u = NULL;
            memcpy(&h, run.sectors + (uint64_t)k * BOOK_SECTOR_BYTES, sizeof(h));
            if (run.state[k] != BOOK_SECTOR_HOLDS || h.kind != BOOK_UNIT_STITCH || h.seq <= w->seq ||
                i + k < h.at || RibbonPastOf(w, h.seq))
                continue;
            RibbonPlaceOn(&w->end, i + k - h.at, &w->next, &at);
            rc = RibbonUnitRead(&at, h.of, h.seq, &w->next, &u);
            if (rc == OK && !RibbonPastAdd(w, u, cap))
                rc = ERR_NO_MEMORY;
        }
        BookUnitLetGo(&run);
        if (rc != OK)
            return rc;
        i += n;
    }
    return OK;
}

/* The units past the end no stitch is shaped like: said, and out of the
 * list — the ribbon is not taken past one, and its records are not read. */
static void RibbonScanShapes(RibbonWalk* w)
{
    uint32_t kept = 0;
    char     why[96];
    for (uint32_t i = 0; i < w->npast; i++) {
        RibbonUnit* u = w->past[i];
        if (u->shape == RIBBON_SHAPE_KEPT) {
            w->past[kept++] = u;
            continue;
        }
        RibbonShapeSaid(u, why, sizeof(why));
        RibbonSay(w, RIBBON_CODE_RECORD_SHAPE, "a unit of the ribbon at sector %llu, seq %llu, is %s: no stitch is "
                  "so shaped (§1.5, §2.2) — the ribbon is not taken past it, and its records are not read",
                  (unsigned long long)RibbonPlaceVlba(&u->start), (unsigned long long)u->seq, why);
        RibbonUnitLetGo(u);
    }
    w->npast = kept;
}

/* What the chain holds of stitch `seq` — one sewn on, or the one the Tally
 * took in: true, its Clasp in *clasp and whether it reads in *known. */
static bool RibbonSewn(const RibbonWalk* w, uint64_t seq, uint32_t* clasp, bool* known)
{
    if (seq == w->slot->head.stitch_seq) {
        *clasp = w->slot->head.stitch_clasp;
        *known = true;
        return true;
    }
    for (uint32_t i = 0; i < w->nstitch; i++) {
        if (w->stitch[i]->seq == seq) {
            *clasp = w->stitch[i]->clasp.crc;
            *known = w->stitch[i]->clasp_read;
            return true;
        }
    }
    return false;
}

/* Whether a member of a branch names `seq` past the chain's end that is not
 * on the medium whole — what settled and is lost. */
static bool RibbonLostNamed(const RibbonWalk* w, uint64_t seq)
{
    RibbonUnit* there = RibbonPastOf(w, seq);
    return seq > w->seq && (!there || !there->sound);
}

/* The first member of a branch, in the order of its seq, that names what
 * settled and is lost: NULL when none does. */
static RibbonUnit* RibbonLostNamer(const RibbonWalk* w, RibbonUnit* const* m, uint32_t nm)
{
    for (uint32_t i = 0; i < nm; i++) {
        uint64_t x = 0;
        if (RibbonUnitNames(m[i], &x) && RibbonLostNamed(w, x))
            return m[i];
    }
    return NULL;
}

/* A branch some unit of which (`namer`, the first) names one that settled —
 * for it began only after that one's flush — and is not on the medium whole
 * (H-3); and when its root, whole, names one the medium does not hold at all,
 * the lie is proven (FAST'13, H-2). Both lie: MEDIUM_LIED. */
static void RibbonLost(RibbonWalk* w, const RibbonUnit* r, const RibbonUnit* namer, const char* seqs)
{
    uint64_t named = 0, x = 0;
    if (r->sound && RibbonUnitNames(r, &named) && named > w->seq && !RibbonPastOf(w, named)) {
        RibbonSay(w, RIBBON_CODE_DEAD_BRANCH, "the lie is proven: stitch seq %llu lies past the end of the ribbon "
                  "and names seq %llu, which the medium does not hold — a flush that lied, the cycles after it "
                  "written over a hole (FAST'13, H-2); seq %s are not applied — MEDIUM_LIED",
                  (unsigned long long)r->seq, (unsigned long long)named, seqs);
        return;
    }
    RibbonUnitNames(namer, &x);
    RibbonSay(w, RIBBON_CODE_DEAD_BRANCH, "what settled is lost: seq %s past the end of the ribbon — stitch seq %llu "
              "names %llu, begun only after %llu had settled, and %llu %s (H-3) — MEDIUM_LIED", seqs,
              (unsigned long long)namer->seq, (unsigned long long)x, (unsigned long long)x, (unsigned long long)x,
              RibbonPastOf(w, x) ? "does not read whole" : "is not on the medium");
}

/* The stitch right after the end, torn, and nothing past it naming it: a cut
 * while it was written — or, every sector of it there and some sum or its
 * Clasp not holding, a cut or rot, said loud (M-4). */
static void RibbonTornSaid(RibbonWalk* w, const RibbonUnit* t)
{
    uint32_t held = 0;
    for (uint32_t k = 0; k < t->of; k++)
        held += t->valid[k] ? 1u : 0u;
    if (RibbonUnitWhole(t))
        RibbonSay(w, RIBBON_CODE_RIBBON_TORN, "the last cycle does not read whole: stitch seq %llu has all %u "
                  "sectors and its stream or its Clasp does not hold — a cut or rot (M-4)",
                  (unsigned long long)t->seq, t->of);
    else
        RibbonSay(w, RIBBON_CODE_RIBBON_TORN, "the ribbon ends on stitch %llu; seq %llu past it is torn — %u of "
                  "its %u sectors hold, a cut while it was written; its numbers are not given again",
                  (unsigned long long)w->seq, (unsigned long long)t->seq, held, t->of);
}

/* A whole root past the end, naming a stitch of the chain by that stitch's
 * own Clasp: the end itself from where the ribbon does not go on, or one
 * before it — the ribbon forks (§16.3 п.10). Settled, not taken, no lie. */
static void RibbonNamesChain(RibbonWalk* w, const RibbonUnit* r, uint64_t n, const char* seqs)
{
    if (n == w->seq) {
        RibbonSay(w, RIBBON_CODE_DEAD_BRANCH, "stitch seq %llu names the end of the ribbon, seq %llu, by its Clasp, "
                  "and lies where the ribbon does not go on — not the first atom after it, nor the start of its "
                  "next_quire naming its Quire (§4.5, step 5): seq %s, settled, are not taken",
                  (unsigned long long)r->seq, (unsigned long long)w->seq, seqs);
        return;
    }
    uint64_t after = n;
    for (uint32_t i = 0; i < w->nstitch; i++) {
        if (w->stitch[i]->seq > n) {
            after = w->stitch[i]->seq;
            break;
        }
    }
    RibbonSay(w, RIBBON_CODE_DEAD_BRANCH, "the ribbon forks at stitch %llu: seq %llu names it by its Clasp, as seq "
              "%llu of the ribbon does, and lies past the ribbon's end — seq %s, settled, are not taken (§16.3, "
              "amendment 10)",
              (unsigned long long)n, (unsigned long long)r->seq, (unsigned long long)after, seqs);
}

/* A root naming a stitch of the chain by a Clasp not its own, a seq the
 * chain does not hold at or before its end, or — by a prev past its own seq —
 * no stitch at all: a dead branch of another history (H-2) — said, not sewn
 * on, no lie. */
static void RibbonStranger(RibbonWalk* w, const RibbonUnit* r, uint64_t n, bool sewn, uint32_t clasp,
                           const char* seqs)
{
    char how[64];
    if (r->head.prev > r->seq) {
        RibbonSay(w, RIBBON_CODE_CLASP_STRANGER, "seq %s past the end of the ribbon: a dead branch of another "
                  "history — stitch seq %llu names, by a prev of %llu past its own seq, no stitch this ribbon "
                  "does hold; it is not sewn on", seqs, (unsigned long long)r->seq,
                  (unsigned long long)r->head.prev);
        return;
    }
    if (sewn)
        ksnprintf(how, sizeof(how), "a Clasp that is not its own (0x%08X, not 0x%08X)", r->head.prev_clasp, clasp);
    else
        ksnprintf(how, sizeof(how), "which this ribbon does not hold");
    RibbonSay(w, RIBBON_CODE_CLASP_STRANGER, "seq %s past the end of the ribbon: a dead branch of another history — "
              "stitch seq %llu names stitch %llu with %s; it is not sewn on", seqs, (unsigned long long)r->seq,
              (unsigned long long)n, how);
}

/* What one branch past the end is (§3.9's table, §16.3 п.10), said once, by
 * what its first unit `r` names and what its units name of each other. */
static void RibbonBranch(RibbonWalk* w, RibbonUnit* r, RibbonUnit* const* m, uint32_t nm)
{
    char        seqs[160];
    uint64_t    n     = 0;
    uint32_t    clasp = 0;
    bool        known = false, has = RibbonUnitNames(r, &n), tail = w->torn && r->seq == w->torn->seq;
    bool        sewn  = has && RibbonSewn(w, n, &clasp, &known);
    bool        ours  = sewn && (!known || r->head.prev_clasp == clasp);
    RibbonUnit* namer = RibbonLostNamer(w, m, nm);
    bool        below = r->head_read && r->head.prev > r->seq;
    RibbonSeqsSaid(m, nm, seqs, sizeof(seqs));
    if (below || (has && !ours && (sewn || n <= w->seq))) {
        if (tail) {
            RibbonUnitLetGo(w->torn);
            w->torn = NULL;
        }
        RibbonStranger(w, r, n, sewn, clasp, seqs);
    } else if (namer) {
        w->lied = true;
        RibbonLost(w, r, namer, seqs);
    } else if (tail && (!has || n == w->seq)) {
        RibbonTornSaid(w, w->torn);
    } else if (!has) {
        RibbonSay(w, RIBBON_CODE_DEAD_BRANCH, "past the end of the ribbon lie pieces of seq %s whose StitchHead does "
                  "not read: a retry, or nothing known — no loss of what settled is shown", seqs);
    } else if (!r->sound) {
        RibbonSay(w, RIBBON_CODE_DEAD_BRANCH, "past the end of the ribbon lie pieces of seq %s naming stitch %llu of "
                  "it: a retry cut while it was written — no loss of what settled is shown", seqs,
                  (unsigned long long)n);
    } else {
        RibbonNamesChain(w, r, n, seqs);
    }
}

/* The units past the end joined into branches, each to the unit its
 * StitchHead names when that one lies past the end too (RibbonUnitNames names
 * only a seq below the namer's, so the join ends), and each branch judged by
 * its root in the order of its seq. */
static error_t RibbonBranches(RibbonWalk* w)
{
    uint32_t     n       = w->npast ? w->npast : 1u;
    RibbonUnit** root    = kmalloc(sizeof(RibbonUnit*) * n);
    RibbonUnit** members = kmalloc(sizeof(RibbonUnit*) * n);
    if (!root || !members) {
        kfree(root);
        kfree(members);
        return ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < w->npast; i++) {
        RibbonUnit* r = w->past[i];
        uint64_t    named;
        while (RibbonUnitNames(r, &named) && RibbonPastOf(w, named))
            r = RibbonPastOf(w, named);
        root[i] = r;
    }
    for (uint32_t r = 0; r < w->npast; r++) {
        uint32_t nm = 0;
        for (uint32_t i = 0; i < w->npast; i++)
            if (root[i] == w->past[r])
                members[nm++] = w->past[i];
        if (nm)
            RibbonBranch(w, w->past[r], members, nm);
    }
    kfree(root);
    kfree(members);
    return OK;
}

/* The direct scan (§2.2): every unit past the chain's end, the torn tail,
 * and what each branch there is. */
error_t RibbonScan(RibbonWalk* w)
{
    RibbonUnit* tail = NULL;
    uint32_t    cap  = 0;
    if (!w->has_end)
        return OK;
    error_t rc = RibbonScanUnits(w, &cap);
    if (rc != OK)
        return rc;
    RibbonScanShapes(w);
    rc = RibbonUnitFind(&w->end, &w->next, w->seq, &tail);
    if (rc != OK)
        return rc;
    if (tail && !tail->sound && tail->shape == RIBBON_SHAPE_KEPT)
        w->torn = tail;
    else
        RibbonUnitLetGo(tail);
    return RibbonBranches(w);
}
