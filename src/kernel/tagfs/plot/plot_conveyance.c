#include "plot_internal.h"

/*
 * What a Plot conveys (plot.h, PlotConveyance): the difference between the
 * runs of the record it was loaded from and the runs it leaves, as splices of
 * that record — the one change an Ink makes of a file's runs, and the one a
 * Shaped record of the book says was made.
 *
 * ‼ THE DIFFERENCE IS TAKEN, NOT KEPT. A Plot works on a flat copy of its
 * window of the runs and changes it in place — a trim, an append, a gather, a
 * run put where another was — and never knew where its changes lie in the
 * record. The Ink finds them once it holds the record the Plot was loaded
 * from: one walk of both in the file's order, O(runs).
 */

static bool StripSame(const Strip *a, const Strip *b)
{
    return a->start == b->start && a->count == b->count && a->kind == b->kind &&
           a->birth == b->birth;
}

/*
 * The splices that make the record's Strips into `runs`, into `out` (NULL:
 * only counted): how many. Both are walked in the file's order, block by
 * block: where both lists are at the same block and their Strips are the
 * same Strip, nothing changed; anywhere else a splice begins, and it goes on,
 * the list behind moving first, until both meet at one block again with the
 * same Strip next — or both end. A Strip is the same only whole and in the
 * same place: two Strips that meet are not one, as the record keeps them
 * (file_record.h). A file's blocks never move along it, so every splice but a
 * last one puts in as many blocks as it takes out, and its `at` is the same
 * in the file before and after.
 */
/* The next of the record's Strips the walk gives, while the window took it
 * in: *taken counts its blocks against `took`. */
static bool ConveyanceNext(StripWalk *w, Strip *o, uint64_t *taken, uint64_t took)
{
    if (*taken >= took || !StripWalkNext(w, o))
        return false;
    *taken += o->count;
    return true;
}

static uint64_t ConveyanceSplices(const FileRecord *cur, uint64_t from, uint64_t took, const Strip *runs,
                                  uint32_t n, BookSplice *out)
{
    StripWalk w;
    Strip     o;
    uint64_t  po = from, pn = from, count = 0, taken = 0, begins = 0;
    uint32_t  j = 0;
    bool      has = StripWalkAt(cur, from, &w, &begins) && ConveyanceNext(&w, &o, &taken, took);

    while (has || j < n) {
        if (has && j < n && po == pn && StripSame(&o, &runs[j])) {
            po += o.count;
            pn += runs[j].count;
            j++;
            has = ConveyanceNext(&w, &o, &taken, took);
            continue;
        }
        uint64_t at = pn, from = po;
        uint32_t first = j;
        for (;;) {
            bool old_behind = has && (j >= n || po <= pn);
            bool new_behind = j < n && (!has || pn <= po);
            if (old_behind) {
                po += o.count;
                has = ConveyanceNext(&w, &o, &taken, took);
            }
            if (new_behind)
                pn += runs[j++].count;
            if ((!has && j >= n) || (po == pn && has && j < n && StripSame(&o, &runs[j])))
                break;
        }
        if (out)
            out[count] = (BookSplice){ .at = at, .remove = po - from, .ninsert = j - first,
                                       .insert = j > first ? &runs[first] : NULL };
        count++;
    }
    return count;
}

/* A Plot conveys its window, from block window_at, of the window_was blocks
 * of the record it took in. */
error_t PlotConveyance(const Plot *p, const FileRecord *cur, BookSplice **out, uint64_t *count)
{
    uint64_t from = p->window_at;
    uint64_t took = p->window_was;
    *out   = NULL;
    *count = ConveyanceSplices(cur, from, took, p->runs, p->run_count, NULL);
    if (*count == 0)
        return OK;
    *out = kmalloc(sizeof(BookSplice) * *count);
    if (!*out) {
        *count = 0;
        return ERR_NO_MEMORY;
    }
    ConveyanceSplices(cur, from, took, p->runs, p->run_count, *out);
    return OK;
}
