#ifndef TAGFS_SETTLE_SEWING_H
#define TAGFS_SETTLE_SEWING_H

#include "settle_internal.h"
#include "../mount/ribbon.h"

/*
 * The Settle cycle of a format-v2 volume: a stitch on its ribbon a cycle
 * (spec_v2 §2.7, §3.2).
 *
 *   sewing.c    the cycle's steps — TAKE, BUILD, FIRST_FLUSH, STITCH,
 *               LAST_FLUSH, DONE — and the stitch they lay: a StitchHead
 *               naming the last stitch by its seq and its Clasp, the records
 *               the cycle carries, a Clasp summing them, PAD to the end of its
 *               last atom
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
 * (UseContext), a record of each file made, written, named, tagged or given up
 * since (Born, Shaped, Labeled, Quitclaim — the Docket's, ink/docket.h), the
 * blocks of fresh data they put in named with their digests and flushed
 * before the stitch (FIRST_FLUSH), or nothing — an empty stitch is the answer
 * to a waiter, the halt's last word, the first FLUSH of a mount (§3.5) and the
 * stitch that closes a Quire early for room (В1). A tail shorter than a block
 * goes as a block on fresh ground: this kernel lays no Slip (§2.8).
 */

/* The bytes a record's tag and length take at the most, a StitchHead's whole
 * record and a Clasp's — what a stream is measured by before it is laid, and
 * what every stitch carries besides its records (RibbonClaim). */
#define STITCH_RECORD_HEAD_MOST  15u
#define STITCH_HEAD_MOST        (STITCH_RECORD_HEAD_MOST + 9u * 10u + 4u)
#define STITCH_CLASP_MOST       (STITCH_RECORD_HEAD_MOST + 2u * 10u + 4u)

/* A Coined's record past the bytes of its key and value, and a UseContext's
 * past those of its word, at the most. */
#define STITCH_COINED_MOST      (STITCH_RECORD_HEAD_MOST + 3u * 10u)
#define STITCH_USE_MOST         (STITCH_RECORD_HEAD_MOST + 10u)

/* The cycle's steps (sewing.c), for settle.c: begun with the door held and the
 * Scribe taken; one step a call — *done when it was the last, and the answer
 * — and its end, settled (OK) or refused. `why` is the cycle's reason. */
void        StitchRunBegin(SettleWhy why);
error_t     StitchRunStep(bool* done);
void        StitchRunEnd(error_t rc);
uint64_t    StitchRunTarget(void);
const char* StitchRunStage(void);

/* A mount's first word for the writer: the book takes writes again
 * (settle.c, SettleMountReset). */
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
    RibbonQuire came;         /* the Quire the ribbon came from: at quire's first sector, or for a retry */
    uint64_t    last;         /* the last stitch sewn on — the slot's when none is past its Tally */
    uint32_t    clasp;        /* and its Clasp */
    uint64_t    tally;        /* the seq of the Tally the stitches are replayed over */
    uint64_t    given;        /* the largest seq this writer gave */
    uint32_t    atom;         /* the medium's atom, in sectors */
    uint64_t    quire_blocks; /* the blocks a Quire is given */
    uint64_t    quires;       /* the Quires the ribbon runs through past the Tally (atomic: said by any core) */
    error_t     taken;        /* how the last ask for a next_quire was answered */
    bool        lied;         /* the medium is shown not to keep what it confirmed: every stitch says so */
    bool        out;          /* the ribbon ran out with no Tally to take it in, said (atomic: any core
                               * that is refused room says it) */
    uint32_t    refused;      /* writes of a stitch the medium refused in a row */
    RibbonQuire ended;        /* the Quire the last stitch sewn ends in — a retry's from_quire */
    uint64_t    ended_from;   /* the seq of the first stitch that ends in it: those before lie in Quires
                               * left, once a stitch begins past it (FallowQuireLeft) */
    RibbonQuire named;        /* the next_quire the mount's reader holds past the last stitch sewn: where
                               * alone it takes a retry up */
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
 *                     with no next_quire, it carries what no claim held room
 *                     for — the person's word: not `promised` — or leaves
 *                     less than an atom after it (§3.6), said; with no
 *                     next_quire a stitch of what was claimed (`promised` —
 *                     freeing records, or none, among it) or a retry goes in
 *                     the rest down to its last atom. ERR_TAGFS_READ_ONLY,
 *                     unsaid, when it is a retry the refusals of the medium
 *                     left no room for (the caller says the book takes no
 *                     write); ERR_NO_MEMORY when the map had no page for the
 *                     next_quire asked
 *   RibbonSewn        the stitch settled: the cursor past it, and what the
 *                     mount's reader holds past it — the Quire it ends in, the
 *                     next_quire it names; the seq every stitch before which
 *                     lies in a Quire the ribbon has now left (FallowQuireLeft):
 *                     a stitch lies in the last of its Quires, and a Quire is
 *                     left once a stitch begun past it settled
 *   RibbonCloseEarly  the Quire the ribbon stands in closed early (В1): the
 *                     next stitch begins at the start of the next_quire the last
 *                     stitch sewn names; false, nothing moved, where that
 *                     stitch names none the cursor stands before
 *   RibbonRetry       the attempt laid at `at`, `of` sectors running on into
 *                     `hop`, refused (M-6, §16.4 п.4): the ribbon goes on in
 *                     the next_quire the last stitch sewn names — at its start,
 *                     or on the atom past the attempt where it lies there —
 *                     the retry naming the Quire that stitch ends in as the
 *                     one it came from; NULL. Or why no reader would take a
 *                     retry up, and none is written: no next_quire is named,
 *                     or the attempts refused fill it to its end. The
 *                     refusals in a row counted either way
 */
const RibbonCursor* RibbonCursorNow(void);
uint64_t RibbonSeqNext(void);
void     RibbonNextGiven(void);
error_t  RibbonRoom(uint64_t of, bool promised, RibbonPlace* at, RibbonQuire* hop);
uint64_t RibbonSewn(const RibbonPlace* at, const RibbonQuire* hop, uint32_t of, uint64_t seq, uint32_t clasp,
                    uint64_t flags);
const char* RibbonRetry(const RibbonPlace* at, const RibbonQuire* hop, uint32_t of);
bool     RibbonCloseEarly(void);

/*
 * ‼ A CHANGE OF A BOOK CLAIMS ITS ROOM ON THE RIBBON BEFORE IT IS MADE (§3.6,
 * §16.2 п.8, P21). A stitch carries every change inked before its TAKE, and is
 * not begun where the rest of its Quire and the next_quire, less what is kept,
 * do not hold it; so the bytes no stitch has taken — the claims, and the
 * Coineds of every tag given an id since the last stitch that settled — are
 * held to the room the next stitch will find: past the stitch in flight, at
 * the most it can take (RibbonFlightBegins). A change that room does not hold
 * is refused with nothing done — where room can still come, to wait for the
 * cycle's end.
 *
 *   RibbonClaim       under the state lock, by Ink: `bytes` of the next stitch
 *                     — the change's record at its most — claimed: OK; or,
 *                     nothing claimed, ERR_WOULD_BLOCK while room can come and
 *                     ERR_TAGFS_NO_SPACE when none can. `frees` for a change
 *                     that only gives ground up (a Quitclaim), which the rest
 *                     of a Quire with no next_quire is kept for. With `bytes`
 *                     0, whether a record could be claimed now, nothing claimed
 *   RibbonReserve     under the state lock, before a change is made: `bytes`
 *                     held as a claim is, so two changes do not both take the
 *                     room one has — a tag's Coined before the tag is given an
 *                     id (once it holds the id the registry counts the Coined,
 *                     and RibbonReserveLetGo lets the hold go), and a write's
 *                     record at its most before a byte of its data moves
 *   RibbonClaimHeld   under the state lock, by the Ink of a change that held
 *                     `held` bytes (RibbonReserve): the hold let go and `bytes`
 *                     claimed in its place — judged only for what is past it,
 *                     so a change refused before its data is never refused
 *                     after it for what it held (P21); refused as RibbonClaim,
 *                     the hold kept
 *   RibbonClaimTaken  under the state lock, at TAKE: every byte claimed so far,
 *                     taken by the stitch — every claim is an Ink's up to the
 *                     target, and the stitch carries its record
 *   RibbonClaimBack   under the state lock: claims given back to the next
 *                     stitch — a cycle refused gives back what it took
 *   RibbonClaimSay    with no lock: a claim refused ERR_TAGFS_NO_SPACE said,
 *                     once a mount, as the ribbon run out
 *   RibbonFlightBegins under the state lock, at TAKE: the stitch in flight takes
 *                     `sectors` at the most — the room a claim is judged by is
 *                     the room past it, and none past a Quire it may run into
 *                     with no next_quire given for it yet
 *   RibbonFlightEnds  with no lock, once the cycle ended either way
 *   RibbonRoomOf      the room a cursor `c` leaves past a stitch in flight of
 *                     `flight` sectors: the sectors a stitch carrying anything
 *                     may take (*any), and one of freeing records (*frees)
 */
error_t  RibbonClaim(uint64_t bytes, bool frees);
error_t  RibbonReserve(uint64_t bytes);
void     RibbonReserveLetGo(uint64_t bytes);
error_t  RibbonClaimHeld(uint64_t held, uint64_t bytes);
uint64_t RibbonClaimTaken(void);
void     RibbonClaimBack(uint64_t bytes);
void     RibbonClaimSay(error_t rc);
void     RibbonFlightBegins(uint64_t sectors);
void     RibbonFlightEnds(void);
void     RibbonRoomOf(const RibbonCursor* c, uint64_t flight, uint64_t* any, uint64_t* frees);

#ifdef CONFIG_VOLUME_TESTS
/* The cursor as it stands, for the stand between two cycles. */
void     RibbonCursorStand(RibbonCursor* out);
/* The bytes claimed by changes no stitch has taken, and those held for changes
 * in the making (RibbonReserve), as they stand — for the stand. */
void     RibbonClaimsStand(uint64_t* claimed, uint64_t* reserved);
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

/* The free blocks a write of a file's data must leave (§3.6, §16.2 п.6): the
 * Tally's two copies and a Quire of ribbon, by the last measure and what data
 * was handed since — measured afresh when `afresh`. With no TagFS lock held;
 * 0 before the ribbon's cursor is up. */
uint64_t EndpaperDataKeeps(bool afresh);

#endif /* TAGFS_SETTLE_SEWING_H */
