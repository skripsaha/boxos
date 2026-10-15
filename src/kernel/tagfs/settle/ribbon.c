#include "sewing.h"
#include "../mount/book_mount.h"
#include "../mount/book_unit.h"
#include "../block_map/block_map_derived.h"

/*
 * Where the ribbon goes on (sewing.h): the writer's cursor past the last
 * stitch sewn on, its next_quire given ahead of need, a refused write taken on
 * at the start of the next_quire the mount's reader looks in, and the rest of
 * a Quire with no next_quire kept.
 *
 * The cursor is laid by the mount, with the door shut, where the walk of the
 * ribbon ended (RibbonCursorMount) and moved by the Settle's holder alone — a
 * stitch settled, a write refused. Nothing else changes it, so it needs no
 * lock: the stand reads it between two cycles, on the runner's core.
 */

static RibbonCursor s_cursor;

/* A Quire's blocks for this volume: the grain it was laid out in (§2.6 — the
 * maker's Quire), a whole number of atoms; the Quire the ribbon stands in
 * when the volume names no grain. */
static uint64_t RibbonQuireBlocks(const RibbonQuire* now, uint32_t atom)
{
    uint64_t grain = tagfs_get_state()->geometry.grain_bytes / TAGFS_BLOCK_SIZE;
    uint64_t atoms = atom / TAGFS_BLOCK_SECTORS ? atom / TAGFS_BLOCK_SECTORS : 1u;
    if (grain == 0)
        grain = now->blocks;
    return (grain + atoms - 1u) / atoms * atoms;
}

error_t RibbonCursorMount(const RibbonWalk* w, const char** glass)
{
    RibbonCursor* c = &s_cursor;
    memset(c, 0, sizeof(*c));
    c->atom = g_book.atom / BOOK_SECTOR_BYTES;
    if (!w->has_end) {
        *glass = "its ribbon ends where the walk cannot say what comes next — no writer goes on from there";
        return ERR_TAGFS_READ_ONLY;
    }
    if (!w->clasp_known) {
        *glass = "the Clasp of its last stitch does not read — no stitch can name it";
        return ERR_TAGFS_READ_ONLY;
    }
    c->quire = w->end.quire;
    c->off   = (w->end.off + c->atom - 1u) / c->atom * c->atom;
    c->next  = RibbonQuireSame(&w->end.quire, &w->quire) ? w->next : (RibbonQuire){ 0, 0 };
    c->came  = RibbonQuireSame(&w->end.quire, &w->quire) ? (RibbonQuire){ 0, 0 } : w->quire;
    if (c->off >= c->quire.blocks * TAGFS_BLOCK_SECTORS) {
        if (!c->next.blocks) {
            *glass = "its ribbon fills its last Quire, and names no next_quire to go on in";
            return ERR_TAGFS_READ_ONLY;
        }
        c->came  = c->quire;
        c->quire = c->next;
        c->next  = (RibbonQuire){ 0, 0 };
        c->off   = 0;
    }
    if (c->off)
        c->came = (RibbonQuire){ 0, 0 };
    /* What the walk holds past its last stitch, as rule (б) reads it
     * (ribbon_collate.c, RibbonSewOne): the Quire it ends in, and the
     * next_quire unless the stitch's end lies in it already. */
    c->ended        = w->quire;
    c->named        = w->next.blocks && !RibbonQuireSame(&w->end.quire, &w->next) ? w->next : (RibbonQuire){ 0, 0 };
    c->last         = w->seq;
    c->clasp        = w->clasp;
    c->tally        = w->slot->head.tally_seq;
    c->given        = BookSeqSeen();
    c->quire_blocks = RibbonQuireBlocks(&c->quire, c->atom);
    c->quires       = w->nquires;
    c->lied         = w->lied;
    c->taken        = OK;
    c->up           = true;
    return OK;
}

void RibbonCursorLetGo(void)
{
    memset(&s_cursor, 0, sizeof(s_cursor));
}

bool RibbonCursorLied(void)
{
    return s_cursor.lied;
}

const RibbonCursor* RibbonCursorNow(void)
{
    return &s_cursor;
}

#ifdef CONFIG_VOLUME_TESTS
void RibbonCursorStand(RibbonCursor* out)
{
    *out = s_cursor;
}
#endif

uint64_t RibbonSeqNext(void)
{
    uint64_t seen = BookSeqSeen();
    s_cursor.given = (s_cursor.given > seen ? s_cursor.given : seen) + 1u;
    BookSeqGiven(s_cursor.given);
    return s_cursor.given;
}

/*
 * ‼ THE NEXT QUIRE IS GIVEN AHEAD OF NEED (§3.6): the first stitch of each
 * Quire of the ribbon asks for the one after it, and every stitch after it
 * asks again while none was given — so that a Quire's end, an early close or
 * a refused write always has somewhere to go on, named in every StitchHead
 * from then on (M-5). It is a whole Quire on an atom of the data run, looked
 * for past the Quire the ribbon stands in, and never one the ground a whole
 * Tally needs would go to (EndpaperKeeps): with no Tally written by this
 * kernel, a Quire of the ribbon never comes back, and the reserve is what is
 * left to the writer of one.
 */
void RibbonNextGiven(void)
{
    RibbonCursor* c = &s_cursor;
    uint64_t      start = 0;
    if (!c->up || c->next.blocks)
        return;
    c->taken = BlockMapTakeQuire(c->quire_blocks, c->atom / TAGFS_BLOCK_SECTORS ? c->atom / TAGFS_BLOCK_SECTORS : 1u,
                                 EndpaperKeeps(c->quire_blocks), g_book.frame.data_block,
                                 c->quire.block + c->quire.blocks, &start);
    if (c->taken == OK)
        c->next = (RibbonQuire){ start, c->quire_blocks };
}

/* The ribbon run out, said once a mount: no next_quire, and the stitch the
 * cycle carries is not one the rest of the Quire is kept for. */
static void RibbonOutSaid(void)
{
    RibbonCursor* c = &s_cursor;
    if (c->out)
        return;
    c->out = true;
    kprintf("[TagFS] the ribbon has run out with no Tally to take it in: %llu Quire(s) taken for it, %llu "
            "block(s) free and %llu kept for a whole Tally — this kernel writes no Tally, so no Quire of the "
            "ribbon comes back; names and memory are refused from here (ERR_TAGFS_NO_SPACE), and the rest of its "
            "Quire is kept for a stitch that takes nothing\n", (unsigned long long)c->quires,
            (unsigned long long)tagfs_get_state()->ledger.free_blocks,
            (unsigned long long)EndpaperKeeps(c->quire_blocks));
}

/*
 * ‼ A STITCH THAT RUNS ON INTO ITS NEXT_QUIRE LEAVES TWO ATOMS OF IT (§2.7,
 * §3.6). Past it the ribbon stands in that Quire with no next_quire given,
 * where the rest is kept for a stitch that takes nothing — the halt's, a
 * waiter's — and one atom more for the stitch that gives the next_quire once
 * ground comes back. One that filled the Quire whole would end where no reader
 * can say what comes next (RibbonPlaceOn: the next_quire it would hop to is
 * not named yet), and the mount would stand the volume under glass with every
 * stitch sewn after it unread.
 */
error_t RibbonRoom(uint64_t of, bool empty, RibbonPlace* at, RibbonQuire* hop)
{
    RibbonCursor* c    = &s_cursor;
    uint64_t      left = c->quire.blocks * TAGFS_BLOCK_SECTORS - c->off;
    at->quire = c->quire;
    at->off   = c->off;
    *hop      = c->next;
    if (c->next.blocks) {
        uint64_t most = left + c->next.blocks * TAGFS_BLOCK_SECTORS - 2u * c->atom;
        if (of <= left || of <= most)
            return OK;
        kprintf("[TagFS] a stitch of %llu sector(s) runs on past the rest of its Quire into the next one and "
                "would leave less of it than the two atoms kept for a stitch that takes nothing and the one "
                "that gives its next_quire (%llu sector(s) at the most) — it is not begun (§2.7, §3.6): what it "
                "carries is refused, ERR_TAGFS_NO_SPACE\n", (unsigned long long)of, (unsigned long long)most);
        return ERR_TAGFS_NO_SPACE;
    }
    if (c->taken == ERR_NO_MEMORY)
        return ERR_NO_MEMORY;
    /* A retry (M-6) carries what the cycle already set out to write, at the
     * first sector of a Quire it went on into: taken while an atom is left
     * past it, so a refusal there is the medium's, said as one (sewing.c). */
    if ((empty || c->refused) && of + c->atom <= left)
        return OK;
    RibbonOutSaid();
    return ERR_TAGFS_NO_SPACE;
}

/* The stitch settled at `at`: the cursor past it — a stitch that ends on its
 * Quire's last sector goes on at the next Quire's first, RibbonRoom gave it
 * one — and what the mount's reader holds past it, as RibbonTake does
 * (ribbon_collate.c): the Quire its last sector lies in, the next_quire forgot
 * when that is a Quire the walk had not ended in, and the one it names taken
 * unless it is the Quire it ends in. */
void RibbonSewn(const RibbonPlace* at, const RibbonQuire* hop, uint32_t of, uint64_t seq, uint32_t clasp,
                uint64_t flags)
{
    RibbonCursor* c    = &s_cursor;
    uint64_t      left = at->quire.blocks * TAGFS_BLOCK_SECTORS - at->off;
    RibbonQuire   last = of <= left ? at->quire : *hop;
    c->last    = seq;
    c->clasp   = clasp;
    c->refused = 0;
#ifdef CONFIG_VOLUME_TESTS
    c->sewn       = *at;
    c->sewn_hop   = *hop;
    c->sewn_of    = of;
    c->sewn_flags = flags;
#else
    (void)flags;
#endif
    if (!RibbonQuireSame(&last, &c->ended)) {
        c->ended = last;
        c->named = (RibbonQuire){ 0, 0 };
    }
    if (hop->blocks && !RibbonQuireSame(hop, &c->ended))
        c->named = *hop;
    if (of < left) {
        c->off = at->off + of;
    } else {
        c->came  = at->quire;
        c->quire = *hop;
        c->next  = (RibbonQuire){ 0, 0 };
        c->off   = of - left;
        c->quires++;
    }
    if (c->off)
        c->came = (RibbonQuire){ 0, 0 };
}

/*
 * ‼ A RETRY GOES WHERE THE MOUNT'S READER TAKES IT UP, OR NOWHERE (M-6, §4.5
 * п.5 (б)). Past the last stitch it took, the reader looks in two places: the
 * first atom after it — where the refused attempt lies, which is never written
 * over — and the start of the next_quire that stitch names, when it is not the
 * Quire the stitch ends in (ribbon_collate.c, RibbonSewOne; stitch.py alike).
 * A retry anywhere else is a stitch the waiters were told is kept and the next
 * mount does not read. So the retry goes to that next_quire's start, coming
 * from the Quire the last stitch ends in — and not at all when no such Quire
 * is named, when the attempt refused began there itself (a retry refused, or
 * the first stitch of a Quire the ribbon came into), or when it ran on into it
 * and may lie there in part. §3.2's retry in the Quire after the next is not
 * one rule (б) reads: it waits on a reader that does.
 */
const char* RibbonRetry(bool ran_on)
{
    RibbonCursor* c = &s_cursor;
    c->refused++;
    if (!c->named.blocks)
        return "the last stitch sewn names no next_quire past the Quire it ends in";
    if (RibbonQuireSame(&c->named, &c->quire))
        return "the attempt began in the next_quire the last stitch sewn names";
    if (ran_on)
        return "the attempt ran on into the next_quire the last stitch sewn names, and may lie there in part";
    c->came  = c->ended;
    c->quire = c->named;
    c->next  = (RibbonQuire){ 0, 0 };
    c->off   = 0;
    c->quires++;
    return NULL;
}
