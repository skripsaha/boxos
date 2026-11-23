#include "sewing.h"
#include "kheap.h"
#include "tagfs_volume.h"

/*
 * What this kernel left on the ribbon, kept for the medium's return (sewing.h,
 * BookLeftMark and RibbonTry).
 *
 * The marks are the stitches past the slot the ribbon is replayed over, as the
 * mount's walk found them and as each settled: a medium that came back is held
 * to the last of them where it was sewn — and, shown to lie once, to every one
 * of them, for such a medium may lose a stitch it confirmed as well as the
 * last (M-2): the walk of the whole ribbon from the slot's `at` that the owner
 * asked for a medium that lied, made here of what this kernel itself laid,
 * stitch by stitch, rather than of rules a reader walks again. A stitch the
 * mount's walk took in pieces — closed and damaged before this kernel came —
 * is no mark: what of it reads was judged by that mount, and nothing of it
 * was confirmed to this one.
 *
 * The tries are every attempt laid since the last stitch settled, each with
 * the sectors it was sealed with: whatever of them the medium holds is this
 * kernel's own, known byte for byte (Р-c1 (б)) — the one sure way to tell its
 * own attempt from another writer's stitch of the same seq at the same place
 * (R11). Memory for an attempt is asked for with the attempt itself
 * (RibbonTryMake, by StitchLay), so keeping it asks for nothing more.
 */


/* Room for one more mark: false when the heap refused it. */
static bool RibbonMarkRoom(TagFSVolume* vol)
{
    if (vol->ribbon_left.nmarks < vol->ribbon_left.capmarks)
        return true;
#ifdef CONFIG_VOLUME_TESTS
    if (RibbonStandMarksRefusing())
        return false;
#endif
    uint32_t      cap  = vol->ribbon_left.capmarks ? vol->ribbon_left.capmarks * 2u : 8u;
    BookLeftMark* more = kmalloc(sizeof(BookLeftMark) * cap);
    if (!more)
        return false;
    if (vol->ribbon_left.nmarks)
        memcpy(more, vol->ribbon_left.marks, sizeof(BookLeftMark) * vol->ribbon_left.nmarks);
    kfree(vol->ribbon_left.marks);
    vol->ribbon_left.marks    = more;
    vol->ribbon_left.capmarks = cap;
    return true;
}

/*
 * Mark `m` kept: on a medium not shown to lie the last alone, which is all a
 * return is held to there — so holding it makes good any mark lacking before;
 * on one that lied, past every other. A mark the heap has no room for leaves
 * its stitch lacking, said once until made good: a return before then — the
 * next stitch settling, or on a medium that lied a Tally taking the stitch
 * in — gives the book up, and the volume is read again.
 */
static void RibbonMarkPut(TagFSVolume* vol, const BookLeftMark* m, bool lied)
{
    if (!lied)
        vol->ribbon_left.nmarks = 0;
    if (RibbonMarkRoom(vol)) {
        vol->ribbon_left.marks[vol->ribbon_left.nmarks++] = *m;
        if (!lied)
            vol->ribbon_left.lacking = 0;
        return;
    }
    if (!vol->ribbon_left.lacking)
        kprintf("[TagFS] memory would not keep the place of stitch %llu, which a medium that leaves is held to "
                "when it comes back: a return before %s gives its book up, and the volume is read again\n",
                (unsigned long long)m->seq, lied ? "a Tally takes that stitch in" : "a stitch past it settles");
    vol->ribbon_left.lacking = m->seq;
}

void RibbonLeftMount(TagFSVolume* vol, const RibbonWalk* w, bool lied, uint64_t seen)
{
    RibbonLeftLetGo(vol);
    vol->ribbon_left.past = w->end;
    vol->ribbon_left.seen = seen;
    for (uint32_t i = 0; i < w->nstitch; i++) {
        const RibbonUnit* u = w->stitch[i];
        if (!lied && i + 1u < w->nstitch)
            continue;
        if (!u->sound) {
            vol->ribbon_left.lacking = u->seq;     /* read in pieces: no mark, nothing of it confirmed to this kernel */
            continue;
        }
        BookLeftMark m = { .at = u->start, .hop = u->hop, .of = u->of, .seq = u->seq, .clasp = u->clasp.crc,
                           .clasp_at = u->clasp_at };
        RibbonMarkPut(vol, &m, true);
    }
}

static void RibbonTriesLetGo(TagFSVolume* vol)
{
    while (vol->ribbon_left.tries) {
        RibbonTry* t = vol->ribbon_left.tries;
        vol->ribbon_left.tries = t->next;
        kfree(t);
    }
}

void RibbonLeftSewn(TagFSVolume* vol, const BookLeftMark* m, bool lied)
{
    if (!RibbonPlaceOn(&m->at, m->of, &m->hop, &vol->ribbon_left.past)) {
        RibbonQuire last = m->of <= RibbonPlaceLeft(&m->at) ? m->at.quire : m->hop;
        vol->ribbon_left.past = (RibbonPlace){ .quire = last, .off = last.blocks * TAGFS_BLOCK_SECTORS };
    }
    RibbonMarkPut(vol, m, lied);
    RibbonTriesLetGo(vol);
}

void RibbonLeftTallied(TagFSVolume* vol, uint64_t taken)
{
    uint32_t keep = 0;
    for (uint32_t i = 0; i < vol->ribbon_left.nmarks; i++)
        if (vol->ribbon_left.marks[i].seq > taken)
            vol->ribbon_left.marks[keep++] = vol->ribbon_left.marks[i];
    vol->ribbon_left.nmarks = keep;
    if (vol->ribbon_left.lacking <= taken)
        vol->ribbon_left.lacking = 0;      /* the stitch lacking its mark lies behind the slot now, which holds it */
}

void RibbonLeftLetGo(TagFSVolume* vol)
{
    RibbonTriesLetGo(vol);
    kfree(vol->ribbon_left.marks);
    vol->ribbon_left.marks    = NULL;
    vol->ribbon_left.nmarks   = 0;
    vol->ribbon_left.capmarks = 0;
    vol->ribbon_left.lacking  = 0;
    vol->ribbon_left.seen     = 0;
    vol->ribbon_left.past     = (RibbonPlace){ 0 };
}

const BookLeftMark* RibbonLeftMarks(TagFSVolume* vol, uint32_t* n, uint64_t* lacking, RibbonPlace* past, uint64_t* seen)
{
    *n       = vol->ribbon_left.nmarks;
    *lacking = vol->ribbon_left.lacking;
    *past    = vol->ribbon_left.past;
    *seen    = vol->ribbon_left.seen;
    return vol->ribbon_left.marks;
}

RibbonTry* RibbonTryMake(uint32_t of)
{
    RibbonTry* t = kmalloc(sizeof(RibbonTry) + (uint64_t)of * BOOK_SECTOR_BYTES);
    if (t)
        memset(t, 0, sizeof(RibbonTry));
    return t;
}

void RibbonTryLaid(TagFSVolume* vol, RibbonTry* t)
{
    t->next = vol->ribbon_left.tries;
    vol->ribbon_left.tries = t;
}

bool RibbonTryOwns(TagFSVolume* vol, uint64_t vlba, const uint8_t* sector, bool* newest)
{
    for (const RibbonTry* t = vol->ribbon_left.tries; t; t = t->next) {
        uint64_t left  = RibbonPlaceLeft(&t->at);
        uint64_t first = RibbonPlaceVlba(&t->at);
        uint64_t k     = UINT64_MAX;
        if (vlba >= first && vlba - first < left && vlba - first < t->of)
            k = vlba - first;
        else if (t->of > left && t->hop.blocks && vlba >= t->hop.block * TAGFS_BLOCK_SECTORS &&
                 vlba - t->hop.block * TAGFS_BLOCK_SECTORS < t->of - left)
            k = left + (vlba - t->hop.block * TAGFS_BLOCK_SECTORS);
        if (k != UINT64_MAX && memcmp(sector, t->sectors + k * BOOK_SECTOR_BYTES, BOOK_SECTOR_BYTES) == 0) {
            *newest = t == vol->ribbon_left.tries;
            return true;
        }
    }
    return false;
}

const char* RibbonTryPast(TagFSVolume* vol)
{
    RibbonTry* t = vol->ribbon_left.tries;
    if (!t || t->past)
        return NULL;
    const char* nowhere = RibbonRetry(vol, &t->at, &t->hop, t->of, true);
    t->past = nowhere == NULL;
    return nowhere;
}
