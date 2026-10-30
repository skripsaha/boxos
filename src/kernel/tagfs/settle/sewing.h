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
 * stitch that closes a Quire early for room (В1). A write that lies wholly in
 * a file's last block and leaves it short of full rides in the stitch as a
 * Slip — the change since the last stitch, one span (§2.8, §16.5 п.13).
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
    uint32_t    atom;         /* the ribbon's atom, in sectors: a flash page on a removable medium, the
                               * medium's atom on any other (§16.6 п.2) — the Quires taken, the room */
    uint64_t    quire_blocks; /* the blocks a Quire is given */
    uint64_t    quires;       /* the Quires the ribbon runs through past the Tally (atomic: said by any core) */
    error_t     taken;        /* how the last ask for a next_quire was answered */
    bool        lied;         /* the medium is shown not to keep what it confirmed: every stitch says so */
    bool        out;          /* the ribbon ran out with no Tally to take it in, said (atomic: any core
                               * that is refused room says it) */
    uint32_t    refused;      /* attempts of a stitch given up in a row: a retry is under way, ... */
    uint32_t    cut;          /* ... `cut` of them cut short by the medium leaving, the rest it refused */
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
 *   RibbonStitchOf    the sectors a stitch of a `len`-byte stream takes where
 *                     the cursor stands: whole atoms of the medium, on to the
 *                     end of an atom of the ribbon (volume_ribbon.h,
 *                     RibbonStitchEnds)
 *   RibbonNextGiven   a next_quire for the Quire the ribbon stands in, when it
 *                     has none or the one named is off an atom of the ribbon:
 *                     taken from the map, the Tally's ground left
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
 *                     attempts given up in a row counted either way — `cut`
 *                     one cut short by the medium leaving, which no refusal
 *                     of the medium's is said as (ribbon_left.c)
 */
const RibbonCursor* RibbonCursorNow(void);
uint64_t RibbonStitchOf(uint64_t len);
uint64_t RibbonSeqNext(void);
void     RibbonNextGiven(void);
error_t  RibbonRoom(uint64_t of, bool promised, RibbonPlace* at, RibbonQuire* hop);
uint64_t RibbonSewn(const RibbonPlace* at, const RibbonQuire* hop, uint32_t of, uint64_t seq, uint32_t clasp,
                    uint32_t clasp_at, uint64_t flags);
const char* RibbonRetry(const RibbonPlace* at, const RibbonQuire* hop, uint32_t of, bool cut);
bool     RibbonCloseEarly(void);

/* A Tally of seq `tally` was written over the slot of the least seq
 * (binder_done.c), taking in every stitch up to `taken`: every stitch from
 * here is replayed over it. By the Settle's holder. */
void     RibbonTallied(uint64_t tally, uint64_t taken);

/*
 * ‼ WHAT THIS KERNEL LEFT ON THE RIBBON IS KEPT FOR THE MEDIUM'S RETURN (spec_v2
 * §3.8, §16.5 пп.11-12; ribbon_left.c). A medium that leaves and comes back is
 * held to it (mount/perambulation.c) before its book is taken up:
 *
 *   BookLeftMark   a stitch this kernel left on the ribbon past the slot it is
 *                  replayed over: where it lies, its seq, its Clasp and where
 *                  that lies in its stream — the crc of every byte before it.
 *                  Every one of them on a medium shown to lie, the last alone
 *                  on any other (Р-c2 (б)): from the mount's walk, then from
 *                  each stitch that settles; those a Tally took in let go
 *   RibbonTry      an attempt of a stitch laid since the last one settled, its
 *                  sectors as sealed: one the medium refused (M-6), one a
 *                  departure cut short. A return finds each sector of it byte
 *                  for byte or holds the medium changed; the stitch that
 *                  settles next lies past every one, and lets them go
 *
 * Both the Settle's holder's, changed with no TagFS lock held — read by the
 * judge of a return while it holds the Settle still (StitchHoldStill) — and
 * let go with the volume.
 *
 *   RibbonLeftMarks  the marks, oldest first: *n of them; *lacking the seq of
 *                    the newest stitch a return must be held to whose mark
 *                    this kernel lacks — memory refused it, or the mount read
 *                    that stitch in pieces — 0 for none: the next stitch to
 *                    settle makes it good on a medium not shown to lie, a
 *                    Tally taking the stitch in on one that lied; *past where
 *                    the ribbon goes on past the last stitch sewn — or past
 *                    the slot's Tally, none sewn — as a mount's reader finds
 *                    its end: where any other writer's next stitch would
 *                    begin; and *seen the largest seq the mount saw on the
 *                    medium — every writer that mounts it gives seqs past it
 *                    (§4.5 п.8), so a sector there of a seq no larger is
 *                    what the mount found, never another's since
 *   RibbonTryMake    an attempt of `of` sectors, its room asked of the heap
 *                    with no lock held: NULL when there is none
 *   RibbonTryLaid    the attempt the writer laid at `at` — its fields filled,
 *                    some of its sectors maybe on the medium — kept, taken
 *   RibbonTryOwns    whether `sector`, read at `vlba`, is a sector of an
 *                    attempt kept, byte for byte as sealed: *newest whether of
 *                    the newest
 *   RibbonTryPast    the ribbon taken past the newest attempt as a retry of it
 *                    is (RibbonRetry), once: NULL, or why no reader would take
 *                    the next stitch up there
 */
typedef struct {
    RibbonPlace at;
    RibbonQuire hop;
    uint32_t    of;
    uint64_t    seq;
    uint32_t    clasp;
    uint32_t    clasp_at;
} BookLeftMark;

typedef struct RibbonTry RibbonTry;
struct RibbonTry {
    RibbonTry*  next;
    RibbonPlace at;
    RibbonQuire hop;
    uint32_t    of;
    uint64_t    seq;
    bool        past;
    uint8_t     sectors[];
};

const BookLeftMark* RibbonLeftMarks(uint32_t* n, uint64_t* lacking, RibbonPlace* past, uint64_t* seen);
RibbonTry*          RibbonTryMake(uint32_t of);
void                RibbonTryLaid(RibbonTry* t);
bool                RibbonTryOwns(uint64_t vlba, const uint8_t* sector, bool* newest);
const char*         RibbonTryPast(void);

/* ribbon.c's own calls into ribbon_left.c: the marks a mount's walk leaves
 * (`lied`: every one), with the largest seq it saw (`seen`), one a stitch
 * settling adds — every try behind it let go — those a Tally took in let go,
 * and all of it with the volume. */
void     RibbonLeftMount(const RibbonWalk* w, bool lied, uint64_t seen);
void     RibbonLeftSewn(const BookLeftMark* m, bool lied);
void     RibbonLeftTallied(uint64_t taken);
void     RibbonLeftLetGo(void);

/*
 * The Settle held still for the judge of a medium that came back (mount/
 * perambulation.c): its Scribe taken as a pass that answers no asker — given
 * back with the cover it found, so every asker takes the next pass itself — a
 * cycle another core holds waited for, and one this core holds between two of
 * its steps run out here: with the medium away, every step it has left is
 * refused at once (tagfs_volume_write). Then no cycle and no Tally's step
 * stands half-done while the cursor, the Docket and the tries are read. True
 * and *written for StitchStillLetGo; false when this core holds the Scribe
 * beneath the caller in the middle of a step, which nothing here can end.
 */
bool     StitchHoldStill(uint64_t* written);
void     StitchStillLetGo(uint64_t written);

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
/* The next stitch's medium leaves (TagFSStandMediumLeaves) once `sectors` of
 * it are on the medium — UINT32_MAX: the whole of it, before its last flush —
 * so the cycle is refused there, as a hand pulling the medium out refuses it.
 * Once. For the stand. */
void     StitchStandLeaves(uint32_t sectors);
/* The next room asked of the heap for the marks of what this kernel left on
 * the ribbon (ribbon_left.c) refused, as a heap that has none refuses it:
 * armed, and taken by the one it refuses. Once. For the stand. */
void     RibbonStandMarksRefused(void);
bool     RibbonStandMarksRefusing(void);
/* The next ask of the map for a next_quire refused, as a map with no Quire
 * past the Tallies' ground refuses it: armed, and taken by the ask it refuses;
 * false takes it down unspent. Whether it stood armed. Once. For the stand. */
bool     RibbonStandNextRefused(bool armed);
/* The bytes claimed by changes no stitch has taken, and those held for changes
 * in the making (RibbonReserve), as they stand — for the stand. */
void     RibbonClaimsStand(uint64_t* claimed, uint64_t* reserved);
#endif

/*
 * The book's reserve (endpaper.c). EndpaperKeeps: the ground the ribbon
 * leaves free for the Tallies — the next Tally, both copies, in whole Quires
 * of `quire_blocks`: each tree measured from above — an upper bound by the
 * rules every writer of a Tally keeps (§16.2 п.24, §16.3 пп.1, 5) — or, FILES,
 * TAGS and DIGESTS, by the last Tally laid and the growth since where that is
 * known and less (endpaper_fact.h, §16.4 п.7); and what the ring's slots lack
 * of one (binder.h, BinderRingShort): the Tallies that fill the ring give
 * nothing back. With no TagFS lock held. For the stand: the blocks one copy
 * of the next Tally takes by that measure taken now (TallyCostBlocks), and by
 * the one a write of data keeps with the data handed since (TallyKeptBlocks,
 * UINT64_MAX when none is kept).
 */
#ifdef CONFIG_VOLUME_TESTS
uint64_t TallyCostBlocks(void);
uint64_t TallyKeptBlocks(void);
#endif
uint64_t EndpaperKeeps(uint64_t quire_blocks);

/* The free blocks a write of a file's data must leave (§3.6, §16.2 п.6): the
 * Tally's two copies, what the ring lacks of it, and a Quire of ribbon, by the
 * last measure and what data was handed since — measured afresh when
 * `afresh`. With no TagFS lock held; 0 before the ribbon's cursor is up. */
uint64_t EndpaperDataKeeps(bool afresh);

#endif /* TAGFS_SETTLE_SEWING_H */
