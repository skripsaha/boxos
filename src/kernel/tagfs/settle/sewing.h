#ifndef TAGFS_SETTLE_SEWING_H
#define TAGFS_SETTLE_SEWING_H

#include "settle_internal.h"
#include "../mount/ribbon.h"

/*
 * The Settle cycle of a format-v2 volume: a stitch on its ribbon a cycle
 * (spec_v2 §2.7, §3.2).
 *
 *   sewing.c    the cycle's steps — TAKE, BUILD, STITCH, LAST_FLUSH, DONE —
 *               and the stitch they lay: a StitchHead naming the last stitch
 *               by its seq and its Clasp, the records the cycle carries, a
 *               Clasp summing them, PAD to the end of its last atom
 *   ribbon.c    where the ribbon goes on: the cursor past its last stitch,
 *               the next_quire given ahead of need, the Quire a refused write
 *               goes on in where the mount's reader takes it up (M-6), and
 *               the rest of a Quire with no next_quire kept for a stitch
 *               that takes nothing (§3.6)
 *   endpaper.c  the reserve of ground the book keeps: a Quire of ribbon and
 *               room for a whole Tally in two copies, by the mirror's upper
 *               bound of what a Tally costs (§16.2 п.6)
 *
 * Format v1's cycle is settle.c's and stays what it was; settle.c fixes the
 * format of a cycle as it begins and runs this one's steps through
 * StitchRunStep. Everything the stitch carries is read back by the mount's
 * reader (ribbon.h) and by volume-read's (stitch.py, bookmount.py): one codec,
 * volume_ribbon.h.
 *
 * What a stitch carries here: the tags given ids since the last stitch that
 * settled (Coined), the volume's Use Context when the person's word changed
 * (UseContext), or nothing — an empty stitch is the answer to a waiter, the
 * halt's last word and the first FLUSH of a mount (§3.5). A change to a file
 * this kernel does not yet write into a book (ink.h).
 */

/* The cycle's steps (sewing.c), for settle.c: begun with the door held and the
 * Scribe taken; one step a call — *done when it was the last, and the answer
 * — and its end, settled (OK) or refused. `why` is the cycle's reason. */
void        StitchRunBegin(SettleWhy why);
error_t     StitchRunStep(bool* done);
void        StitchRunEnd(error_t rc);
uint64_t    StitchRunTarget(void);
const char* StitchRunStage(void);

/* A mount's first word for the writer: the book takes writes again, and the
 * refusal of a change to a file is said anew (settle.c, SettleMountReset). */
void        StitchMountReset(void);

/*
 * Where the ribbon goes on (ribbon.c), as the writer keeps it between its
 * cycles. Changed by the Settle's holder alone — at a cycle's DONE, or at a
 * retry — and by the mount with the door shut; read by them, and by the stand
 * between two cycles.
 */
typedef struct {
    bool        up;           /* the cursor is the mounted volume's */
    RibbonQuire quire;        /* the Quire the next stitch begins in */
    uint64_t    off;          /* the sector of `quire` it begins at — on an atom */
    RibbonQuire next;         /* the next_quire given for `quire`; blocks 0 until one is */
    RibbonQuire came;         /* the Quire the ribbon came from, while it stands at quire's first sector */
    uint64_t    last;         /* the last stitch sewn on — the slot's when none is past its Tally */
    uint32_t    clasp;        /* and its Clasp */
    uint64_t    tally;        /* the seq of the Tally the stitches are replayed over */
    uint64_t    given;        /* the largest seq this writer gave */
    uint32_t    atom;         /* the medium's atom, in sectors */
    uint64_t    quire_blocks; /* the blocks a Quire is given */
    uint64_t    quires;       /* the Quires the ribbon runs through past the Tally */
    error_t     taken;        /* how the last ask for a next_quire was answered */
    bool        lied;         /* the medium is shown not to keep what it confirmed: every stitch says so */
    bool        out;          /* the ribbon ran out with no Tally to take it in, said */
    uint32_t    refused;      /* writes of a stitch the medium refused in a row */
    RibbonQuire ended;        /* the Quire the last stitch sewn ends in — a retry's from_quire */
    RibbonQuire named;        /* the next_quire the mount's reader holds past the last stitch sewn, when it
                               * is not the Quire that stitch ends in: where alone it takes a retry up */
#ifdef CONFIG_VOLUME_TESTS
    RibbonPlace sewn;         /* where the last stitch this mount sewed begins, ... */
    RibbonQuire sewn_hop;     /* ... the Quire it ran on into, ... */
    uint32_t    sewn_of;      /* ... its sectors and its flags — for the stand */
    uint64_t    sewn_flags;
#endif
} RibbonCursor;

/* The cursor laid where the mount's walk ended (book_mount.c, the ribbon
 * replayed): OK; or the reason the writer could not go on, for the glass —
 * a ribbon whose end the walk does not know, or whose last Clasp did not read.
 * Let go of with the volume. */
error_t  RibbonCursorMount(const RibbonWalk* w, const char** glass);
void     RibbonCursorLetGo(void);

/* Whether the volume's medium was shown to lie, as the next stitch will say. */
bool     RibbonCursorLied(void);

/*
 * The cursor's moves, for the cycle's steps (sewing.c), by the Settle's
 * holder with no TagFS lock held:
 *
 *   RibbonCursorNow   the cursor as it stands, to read
 *   RibbonSeqNext     a seq for the stitch about to be built — past every one
 *                     the book showed and this writer gave — folded into what
 *                     the mount counts seen (BookSeqSeen)
 *   RibbonNextGiven   a next_quire for the Quire the ribbon stands in, when it
 *                     has none: taken from the map, the Tally's ground left
 *   RibbonRoom        where a stitch of `of` sectors goes — the place, and the
 *                     Quire it may run on into — OK; ERR_TAGFS_NO_SPACE when
 *                     the rest and the next_quire, less the two atoms of it a
 *                     stitch that runs on into it leaves, do not hold it, or,
 *                     with no next_quire, it takes anything — but a retry's —
 *                     or leaves less than an atom after it (§3.6), said;
 *                     ERR_NO_MEMORY when the map had no page for the
 *                     next_quire asked
 *   RibbonSewn        the stitch settled: the cursor past it, and what the
 *                     mount's reader holds past it — the Quire it ends in, the
 *                     next_quire it names
 *   RibbonRetry       a write of the stitch refused (M-6): the ribbon goes on
 *                     at the start of the next_quire the last stitch sewn
 *                     names — NULL; or why no reader would take a retry up
 *                     there, and none is written: none is named past the Quire
 *                     the refused attempt began in, it began there, or it ran
 *                     on into it (`ran_on`). The refusals in a row counted
 *                     either way
 */
const RibbonCursor* RibbonCursorNow(void);
uint64_t RibbonSeqNext(void);
void     RibbonNextGiven(void);
error_t  RibbonRoom(uint64_t of, bool empty, RibbonPlace* at, RibbonQuire* hop);
void     RibbonSewn(const RibbonPlace* at, const RibbonQuire* hop, uint32_t of, uint64_t seq, uint32_t clasp,
                    uint64_t flags);
const char* RibbonRetry(bool ran_on);

#ifdef CONFIG_VOLUME_TESTS
/* The cursor as it stands, for the stand between two cycles. */
void     RibbonCursorStand(RibbonCursor* out);
#endif

/*
 * The book's reserve (endpaper.c). TallyCostBlocks: the blocks one copy of a
 * whole Tally of the volume as memory holds it takes — an upper bound by the
 * rules every writer of a Tally keeps (§16.2 п.24, §16.3 пп.1, 5), never less
 * than the Tally a writer lays of it. EndpaperKeeps: the ground the ribbon
 * leaves free for that Tally, both copies, in whole Quires of `quire_blocks`.
 * With no TagFS lock held.
 */
uint64_t TallyCostBlocks(void);
uint64_t EndpaperKeeps(uint64_t quire_blocks);

#endif /* TAGFS_SETTLE_SEWING_H */
