#include "ribbon.h"
#include "book_mount.h"

/*
 * RibbonQuiresOf — the Quires the ribbon of a slot not stood on runs through
 * (spec_v2 §16.2 п.12, §4.5 п.4): the slot names that ground, and the census
 * must see it, but the slot is read for its ground alone. So its ribbon is
 * walked by its StitchHeads only: from the slot's `at`, one sector a stitch —
 * the first, where its StitchHead lies — the next stitch where the last one
 * ends by its `of` (rule (а)) or at the start of the next_quire, naming the
 * Quire the last one ended in as the one it came from (rule (б)); on through
 * each next_quire a StitchHead names, the walk bounded by the data run's
 * length (meta_pool_mount.c:478-480). Nothing past a stitch's first sector is
 * read, nothing of a stitch is held past its step, and nothing is said.
 *
 * ‼ A HEAD IS NOT A STITCH. What reads of a StitchHead is its seq, the stitch
 * it names before it by seq — and, at the slot's own boundary, by Clasp — and
 * the next_quire it names; whether the rest of the stitch holds, and the Clasp
 * of any stitch after the first, are not read. So a stitch taken here is one
 * pass 1 of a mount on this slot may not take (torn, of another history), and
 * a closed, damaged one whose first sector does not hold ends the walk where
 * pass 1 goes on by the stitch after it (rule (в)). The one leans to claiming
 * a Quire more — a Quire held while a slot names it, never one given while a
 * slot needs it; the other is the price the spec sets on reading heads alone.
 * volume-read walks the same ribbon whole (stitch_ribbon.py Ribbon,
 * scan=False; §16.3 п.13), and the census holds the two to one answer on every
 * landing.
 *
 * ‼ A RETRY IS FOUND WHERE THE WRITER PUTS IT, AND LOOKED FOR WHERE PASS 1
 * LOOKS (§16.4 п.4, rule (б′)). A slot this kernel wrote names a ribbon this
 * kernel sews, and a refused stitch is taken again on the atom past its try in
 * the next_quire. So, as pass 1 does (ribbon_collate.c, RibbonKept,
 * RibbonFurther), wherever a next_quire is named and no head past the stitch
 * rule (а) gives names it — at its end, or at the start of the next_quire it
 * names — the walk looks into the next_quire: the head at its start that goes
 * on from the last stitch, else the first such further in — a unit holding a
 * sector there stepped over to its end, a sector of no unit of a later seq
 * stepped over alone — naming the Quire it came from; and past either, while a
 * later try at its end goes on from the same with a larger seq, that one
 * (§16.3 п.11). Of what rule (а) and the next_quire give, the larger seq is
 * taken. The head read at a stitch's end to know it was kept is the next
 * step's rule (а): read once.
 */

/* Does the head `u` read at the walk's end — or, `at_next`, at the start of its
 * next_quire — go on from the last stitch taken: naming it by seq, by its
 * Clasp where that is known (the slot's boundary), and at the next_quire the
 * Quire it came from? */
static bool RibbonHeadFollows(const RibbonWalk* w, const RibbonUnit* u, bool at_next)
{
    if (u->head.prev != (w->seq ? u->seq - w->seq : 0) || (w->clasp_known && u->head.prev_clasp != w->clasp))
        return false;
    return !at_next || (u->head.from_block == w->quire.block && u->head.from_blocks == w->quire.blocks);
}

/* Whether the head `u`, read at `u`'s place — the walk's end, or, `at_next`,
 * the start of its next_quire — is taken (rules (а), (б)): it was there to
 * read, goes on from the walk and is shaped as a stitch may be. */
static bool RibbonHeadTakes(const RibbonWalk* w, const RibbonUnit* u, bool found, bool at_next)
{
    return found && RibbonHeadFollows(w, u, at_next) &&
           RibbonShape(u->start.off, RibbonPlaceLeft(&u->start), u->hop.blocks * VOLUME_BLOCK_SECTORS, u->of) ==
               RIBBON_SHAPE_KEPT;
}

/* The stitch whose first sector lies at `place`, its head read, *taken when
 * rules (а), (б) take it. */
static error_t RibbonHeadAt(const RibbonWalk* w, const RibbonPlace* place, bool at_next, RibbonUnit* u,
                            bool* taken)
{
    bool    found = false;
    error_t rc    = RibbonUnitHead(place, at_next ? NULL : &w->next, w->seq, u, &found);
    *taken = rc == OK && RibbonHeadTakes(w, u, found, at_next);
    return rc;
}

/* `u` taken by its head: the walk's last stitch from here, its Quire the
 * walk's when it ended in another, its end the walk's, the next_quire it names
 * weighed. False with no memory. */
static bool RibbonHeadTaken(RibbonWalk* w, const RibbonUnit* u)
{
    RibbonPlace last;
    RibbonQuire next = { u->head.next_block, u->head.next_blocks };
    RibbonPlaceOn(&u->start, u->of - 1u, &u->hop, &last);
    w->seq         = u->seq;
    w->clasp_known = false;
    if (!RibbonQuireSame(&last.quire, &w->quire)) {
        w->quire = last.quire;
        w->next  = (RibbonQuire){ 0, 0 };
        if (!RibbonQuireAdd(w, &w->quire))
            return false;
    }
    w->has_end = RibbonPlaceOn(&u->start, u->of, &u->hop, &w->end);
    return RibbonNamed(w, &next, "a StitchHead");
}

/* The place `of` sectors on from `u`'s start, into its Quire's next. */
static bool RibbonHeadEnd(const RibbonUnit* u, RibbonPlace* end)
{
    return RibbonPlaceOn(&u->start, u->of, &u->hop, end);
}

/* Rule (б′) by heads: the first head in the next_quire that goes on from the
 * last stitch taken — in *u, *found. OK, or the medium's or memory's refusal. */
static error_t RibbonHeadFurther(const RibbonWalk* w, RibbonUnit* u, bool* found)
{
    uint64_t total = w->next.blocks * VOLUME_BLOCK_SECTORS, pos = 0;
    error_t  rc    = OK;
    *found = false;
    while (rc == OK && pos < total && !*found) {
        RibbonPlace    p = { w->next, pos };
        BookSectorHead h;
        RibbonUnit*    whole = NULL;
        uint64_t       j     = 0;
        bool           any   = false;
        if ((rc = RibbonUnitAhead(&p, w->seq, &j, &h, &any, &whole)) != OK || !any)
            break;
        if (whole && whole->head_read && RibbonHeadFollows(w, whole, true)) {
            memset(u, 0, sizeof(*u));
            u->start = whole->start;
            u->hop   = whole->hop;
            u->seq   = whole->seq;
            u->of    = whole->of;
            u->head  = whole->head;
            *found   = true;
        }
        RibbonUnitLetGo(whole);
        pos += j + (h.at < h.of ? (uint64_t)h.of - h.at : 1u);
    }
    return rc;
}

/* Past the stitch `u` found in the next_quire, while a whole head at its end
 * goes on from the walk with a larger seq — a try refused that landed, and the
 * retry after it (§16.3 п.11) — that one in its place; `next` is room for the
 * head read. */
static error_t RibbonHeadOn(const RibbonWalk* w, RibbonUnit* u, RibbonUnit* next)
{
    error_t rc = OK;
    for (RibbonPlace end; rc == OK && RibbonHeadEnd(u, &end);) {
        bool taken = false;
        if ((rc = RibbonHeadAt(w, &end, true, next, &taken)) != OK || !taken || next->seq <= u->seq)
            break;
        *u = *next;
    }
    return rc;
}

/* Whether the head `u` read past the stitch `s` names it — by seq, which the
 * book never gives twice: a writer was told `s` was kept. */
static bool RibbonHeadNames(const RibbonUnit* u, const RibbonUnit* s)
{
    return u->seq > s->seq && u->head.prev == u->seq - s->seq;
}

/* Whether a head past `s` — the stitch rule (а) gives — names it: at its end,
 * read into *ahead (*found when one is there, read through *hop), or at the
 * start of the next_quire it names, read into `v` (ribbon_collate.c,
 * RibbonKept). */
static error_t RibbonHeadKept(const RibbonWalk* w, const RibbonUnit* s, RibbonUnit* ahead, bool* found,
                              RibbonQuire* hop, bool* kept, RibbonUnit* v)
{
    RibbonQuire own = { s->head.next_block, s->head.next_blocks };
    RibbonPlace end, last;
    error_t     rc  = OK;
    *kept  = false;
    *found = false;
    *hop   = (RibbonQuire){ 0, 0 };
    if (!RibbonHeadEnd(s, &end))
        return OK;
    (void)RibbonPlaceOn(&s->start, s->of - 1u, &s->hop, &last);
    *hop  = RibbonQuireLies(&own) ? own : RibbonQuireSame(&last.quire, &w->quire) ? w->next : *hop;
    rc    = RibbonUnitHead(&end, hop->blocks ? hop : NULL, s->seq, ahead, found);
    *kept = rc == OK && *found && RibbonHeadNames(ahead, s);
    if (rc == OK && !*kept && hop->blocks && !RibbonQuireSame(&end.quire, hop)) {
        RibbonPlace head  = { *hop, 0 };
        bool        there = false;
        rc    = RibbonUnitHead(&head, NULL, s->seq, v, &there);
        *kept = rc == OK && there && RibbonHeadNames(v, s);
    }
    return rc;
}

/* Rules (б) and (б′) by heads: the head at the next_quire's start that goes on
 * from the walk, else the first further in — and the latest try past it; `v`
 * is room for a head read past it. */
static error_t RibbonHeadOnward(const RibbonWalk* w, RibbonUnit* u, bool* found, RibbonUnit* v)
{
    RibbonPlace head = { w->next, 0 };
    error_t     rc   = OK;
    *found = false;
    if (!RibbonQuireSame(&w->end.quire, &w->next))
        rc = RibbonHeadAt(w, &head, true, u, found);
    if (rc == OK && !*found)
        rc = RibbonHeadFurther(w, u, found);
    return rc == OK && *found ? RibbonHeadOn(w, u, v) : rc;
}

/* The heads a step of the walk weighs — from the heap: four would not fit a
 * frame. */
typedef struct {
    RibbonUnit here;
    RibbonUnit there;
    RibbonUnit ahead;
    RibbonUnit spare;
} RibbonHeads;

error_t RibbonQuiresOf(const SpineSlot* slot, RibbonWalk** out)
{
    RibbonWalk*  w  = NULL;
    RibbonHeads* h  = kmalloc(sizeof(RibbonHeads));
    error_t      rc = h ? RibbonWalkBegun(slot, false, &w) : ERR_NO_MEMORY;
    bool         cached = false, ahead_found = false;
    *out = w;
    for (uint64_t hops = 0; rc == OK && w->has_end && hops <= g_book.data_end; hops++) {
        RibbonQuire hop = { 0, 0 };
        bool        a = false, b = false, kept = false;
        if (cached) {
            h->here = h->ahead;
            a       = RibbonHeadTakes(w, &h->here, ahead_found, false);
        } else {
            rc = RibbonHeadAt(w, &w->end, false, &h->here, &a);
        }
        cached = false;
        if (rc == OK && a && w->next.blocks)
            rc = RibbonHeadKept(w, &h->here, &h->ahead, &ahead_found, &hop, &kept, &h->spare);
        if (rc == OK && w->next.blocks && !kept)
            rc = RibbonHeadOnward(w, &h->there, &b, &h->spare);
        if (rc != OK || (!a && !b))
            break;
        /* Both go on from the last: the larger seq is the retry, and the one
         * the ribbon goes on by (§16.3 п.11). */
        bool by_here = a && (!b || h->here.seq >= h->there.seq);
        if (!RibbonHeadTaken(w, by_here ? &h->here : &h->there))
            rc = ERR_NO_MEMORY;
        cached = by_here && hop.blocks && RibbonQuireSame(&hop, &w->next);
    }
    kfree(h);
    return rc;
}
