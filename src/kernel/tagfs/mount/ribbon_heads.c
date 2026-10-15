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
 * volume-read walks the same ribbon whole (stitch.py Ribbon, scan=False;
 * §16.3 п.13), and the census holds the two to one answer on every landing.
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

/* The stitch whose first sector lies at `place`, its head read, taken when it
 * goes on from the walk (rules (а), (б)) and is shaped as a stitch may be. */
static error_t RibbonHeadAt(const RibbonWalk* w, const RibbonPlace* place, bool at_next, RibbonUnit* u,
                            bool* taken)
{
    bool    found = false;
    error_t rc    = RibbonUnitHead(place, at_next ? NULL : &w->next, w->seq, u, &found);
    *taken = rc == OK && found && RibbonHeadFollows(w, u, at_next) &&
             RibbonShape(u->start.off, RibbonPlaceLeft(&u->start), u->hop.blocks * VOLUME_BLOCK_SECTORS, u->of) ==
                 RIBBON_SHAPE_KEPT;
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

error_t RibbonQuiresOf(const SpineSlot* slot, RibbonWalk** out)
{
    RibbonWalk* w  = NULL;
    error_t     rc = RibbonWalkBegun(slot, false, &w);
    *out = w;
    for (uint64_t hops = 0; rc == OK && w->has_end && hops <= g_book.data_end; hops++) {
        RibbonUnit  here, there;
        RibbonPlace head  = { w->next, 0 };
        bool        a     = false, b = false;
        rc = RibbonHeadAt(w, &w->end, false, &here, &a);
        if (rc == OK && w->next.blocks && !RibbonQuireSame(&w->end.quire, &w->next))
            rc = RibbonHeadAt(w, &head, true, &there, &b);
        if (rc != OK || (!a && !b))
            break;
        /* Both go on from the last: the larger seq is the retry, and the one
         * the ribbon goes on by (§16.3 п.11). */
        if (!RibbonHeadTaken(w, a && (!b || here.seq >= there.seq) ? &here : &there))
            rc = ERR_NO_MEMORY;
    }
    return rc;
}
