#ifndef TAGFS_SETTLE_SEWING_H
#define TAGFS_SETTLE_SEWING_H

#include "settle_internal.h"
#include "../mount/ribbon.h"
#include "../ink/docket.h"
#include "read_pace.h"
#include "use_context.h"

/*
 * The Settle cycle: a stitch on the book's ribbon a cycle.
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
 * settle.c begins a cycle and runs its steps through StitchRunStep.
 * Everything the stitch carries is read back by the mount's
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
void        StitchRunBegin(TagFSVolume* vol, SettleWhy why);
error_t     StitchRunStep(TagFSVolume* vol, bool* done);
void        StitchRunEnd(TagFSVolume* vol, error_t rc);
uint64_t    StitchRunTarget(TagFSVolume* vol);
const char* StitchRunStage(TagFSVolume* vol);

/* A mount's first word for the writer: the book takes writes again
 * (settle.c, SettleMountReset). */
void        StitchMountReset(TagFSVolume* vol);

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
    bool        out;          /* the ribbon ran out under a stitch, said until a next_quire is given again
                               * (atomic: the Settle's holder says it, a mount clears it) */
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
error_t  RibbonCursorMount(TagFSVolume* vol, const RibbonWalk* w, const char** glass);
void     RibbonCursorLetGo(TagFSVolume* vol);

/* Whether the volume's medium was shown to lie, as the next stitch will say. */
bool     RibbonCursorLied(TagFSVolume* vol);

/* The mount's ask for a next_quire, with the door shut, past the free it says
 * (book_mount.c): one given where the cursor wants it and the map has one, a
 * refusal left to the first cycle's ask (RibbonNextGiven) with the cursor as
 * the mount laid it. */
void     RibbonNextGivenAtMount(TagFSVolume* vol);

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
 *                     for — the person's word of text: not `promised`; an
 *                     empty one gives up as a delete does — or leaves
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
const RibbonCursor* RibbonCursorNow(TagFSVolume* vol);
uint64_t RibbonStitchOf(TagFSVolume* vol, uint64_t len);
uint64_t RibbonSeqNext(TagFSVolume* vol);
void     RibbonNextGiven(TagFSVolume* vol);
/* At a Tally's DONE that took in stitch `slice`, with no next_quire given: the
 * take again — the Tally may have lifted what kept the ribbon from ground
 * (fallow.c, NodeBarred) or let Quires go — and, none had, the next Tally of
 * the ring's turn asked, which lets the next slot's Quires go: until the ring
 * turned whole, as many Tallies in a row as it has slots taking in the same
 * stitch — past that a Tally gives nothing more until a stitch settles. */
void     RibbonNextAgain(TagFSVolume* vol, uint64_t slice);
error_t  RibbonRoom(TagFSVolume* vol, uint64_t of, bool promised, RibbonPlace* at, RibbonQuire* hop);
uint64_t RibbonSewn(TagFSVolume* vol, const RibbonPlace* at, const RibbonQuire* hop, uint32_t of, uint64_t seq, uint32_t clasp,
                    uint32_t clasp_at, uint64_t flags);
const char* RibbonRetry(TagFSVolume* vol, const RibbonPlace* at, const RibbonQuire* hop, uint32_t of, bool cut);
bool     RibbonCloseEarly(TagFSVolume* vol);

/* A Tally of seq `tally` was written over the slot of the least seq
 * (binder_done.c), taking in every stitch up to `taken`: every stitch from
 * here is replayed over it. By the Settle's holder. */
void     RibbonTallied(TagFSVolume* vol, uint64_t tally, uint64_t taken);

/*
 * ‼ WHAT THIS KERNEL LEFT ON THE RIBBON IS KEPT FOR THE MEDIUM'S RETURN
 * (ribbon_left.c). A medium that leaves and comes back is held to it
 * (mount/perambulation.c) before its book is taken up:
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

const BookLeftMark* RibbonLeftMarks(TagFSVolume* vol, uint32_t* n, uint64_t* lacking, RibbonPlace* past, uint64_t* seen);
RibbonTry*          RibbonTryMake(uint32_t of);
void                RibbonTryLaid(TagFSVolume* vol, RibbonTry* t);
bool                RibbonTryOwns(TagFSVolume* vol, uint64_t vlba, const uint8_t* sector, bool* newest);
const char*         RibbonTryPast(TagFSVolume* vol);

/* ribbon.c's own calls into ribbon_left.c: the marks a mount's walk leaves
 * (`lied`: every one), with the largest seq it saw (`seen`), one a stitch
 * settling adds — every try behind it let go — those a Tally took in let go,
 * and all of it with the volume. */
void     RibbonLeftMount(TagFSVolume* vol, const RibbonWalk* w, bool lied, uint64_t seen);
void     RibbonLeftSewn(TagFSVolume* vol, const BookLeftMark* m, bool lied);
void     RibbonLeftTallied(TagFSVolume* vol, uint64_t taken);
void     RibbonLeftLetGo(TagFSVolume* vol);

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
bool     StitchHoldStill(TagFSVolume* vol, uint64_t* written);
void     StitchStillLetGo(TagFSVolume* vol, uint64_t written);

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
 *   RibbonClaimSay    with no lock, as a change's claim was answered: the
 *                     answer it stands by — short of room while a Tally gives
 *                     the ribbon its next_quire, ERR_WOULD_BLOCK on that Tally
 *                     (binder.h, BinderRibbonWait); refused, ERR_TAGFS_NO_SPACE
 *                     said as a change refused room on the ribbon, once until
 *                     a change is given room on it
 *   RibbonFlightBegins under the state lock, at TAKE: the stitch in flight takes
 *                     `sectors` at the most — the room a claim is judged by is
 *                     the room past it, and none past a Quire it may run into
 *                     with no next_quire given for it yet
 *   RibbonFlightEnds  with no lock, once the cycle ended either way
 *   RibbonRoomOf      the room a cursor `c` leaves past a stitch in flight of
 *                     `flight` sectors: the sectors a stitch carrying anything
 *                     may take (*any), and one of freeing records (*frees)
 */
error_t  RibbonClaim(TagFSVolume* vol, uint64_t bytes, bool frees);
error_t  RibbonReserve(TagFSVolume* vol, uint64_t bytes);
void     RibbonReserveLetGo(TagFSVolume* vol, uint64_t bytes);
error_t  RibbonClaimHeld(TagFSVolume* vol, uint64_t held, uint64_t bytes);
uint64_t RibbonClaimTaken(TagFSVolume* vol);
void     RibbonClaimBack(TagFSVolume* vol, uint64_t bytes);
error_t  RibbonClaimSay(TagFSVolume* vol, error_t rc);
void     RibbonFlightBegins(TagFSVolume* vol, uint64_t sectors);
void     RibbonFlightEnds(TagFSVolume* vol);
void     RibbonRoomOf(TagFSVolume* vol, const RibbonCursor* c, uint64_t flight, uint64_t* any, uint64_t* frees);

#ifdef CONFIG_VOLUME_TESTS
/* The cursor as it stands, for the stand between two cycles. */
void     RibbonCursorStand(TagFSVolume* vol, RibbonCursor* out);
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
void     RibbonClaimsStand(TagFSVolume* vol, uint64_t* claimed, uint64_t* reserved);
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
uint64_t TallyCostBlocks(TagFSVolume* vol);
uint64_t TallyKeptBlocks(TagFSVolume* vol);
#endif
uint64_t EndpaperKeeps(TagFSVolume* vol, uint64_t quire_blocks);

/* The free blocks a write of a file's data must leave (§3.6, §16.2 п.6): the
 * Tally's two copies, what the ring lacks of it, and a Quire of ribbon, by the
 * last measure and what data was handed since — measured afresh when
 * `afresh`. With no TagFS lock held; 0 before the ribbon's cursor is up. */
uint64_t EndpaperDataKeeps(TagFSVolume* vol, bool afresh);

typedef enum {
    STITCH_STEP_TAKE = 0,
    STITCH_STEP_BUILD,
    STITCH_STEP_FIRST_FLUSH,
    STITCH_STEP_STITCH,
    STITCH_STEP_LAST_FLUSH,
    STITCH_STEP_DONE,
} StitchStep;

typedef struct {
    SettleWhy   why;
    StitchStep  step;
    uint64_t    target;       /* every Ink up to here */
    uint64_t    cycle;
    uint64_t    flags;        /* StitchHead's */
    uint64_t    made_ns;
    uint32_t    coined_lo;    /* the tags laid down as Coineds: [lo, hi) */
    uint32_t    coined_hi;
    bool        retires;      /* a tag the book has was retired by an Ink up to the target: a Coined RETIRED is owed */
    bool        use_owed;     /* a word the person said is owed the medium */
    UseText*    use;          /* that word, held; NULL says none */
    uint64_t    use_ticket;
    bool        use_out;      /* the word had no room on the ribbon: the stitch is laid without it */
    bool        use_forgot;   /* ... and forgets the word the book held: an empty UseContext in its place */
    bool        name_owed;    /* a name a person gave the volume is owed the medium (volume/volume_name.c) */
    UseText*    name;         /* that name, held; NULL takes the name away */
    uint64_t    name_ticket;
    bool        name_out;     /* the name had no room on the ribbon: the stitch is laid without it, the old kept */
    DocketTaken jots;         /* the files' changes up to the target, by fid */
    uint64_t    claimed;      /* the ribbon's bytes their Inks claimed, taken (RibbonClaimTaken) */
    uint64_t    flight;       /* the sectors TAKE counted it in flight at the most (StitchFlight) */
    uint64_t    data_need;    /* the flush that keeps every block the Inks up to the target wrote */
    uint64_t    d_us;         /* the toll's measures, taken at S0: the last cycle's D, */
    ReadPaceSeen pace;        /* and the pace the medium's reads were seen to keep */
    bool        toll;         /* the stitch pays the flush of its data before its atoms */
    uint64_t    fresh;        /* the blocks of fresh data the stitch names (Clasp.fresh) */
    uint64_t    flush_us;     /* what the flush of its data took the medium: no part of D (§16.5 п.1) */
    uint64_t    seq;          /* this attempt's */
    uint32_t    clasp;
    uint32_t    clasp_at;     /* where its Clasp lies in its stream: its crc is of every byte before */
    RibbonTry*  laid;         /* the attempt, kept past its refusal or its cut (ribbon_left.c) */
    uint8_t*    sectors;      /* the attempt sealed: `of` sectors, laid's */
    uint32_t    of;
    uint32_t    useful;       /* the bytes of its records, StitchHead to Clasp (§4.2) */
    RibbonPlace at;           /* where it lies, and the Quire it runs on into */
    RibbonQuire hop;
    uint32_t    written;      /* its sectors on the medium so far */
    const char* stage;
} StitchSnap;

/* What one volume holds of sewing.c (TagFSVolume, volume/tagfs_volume.h). */
typedef struct {
    StitchSnap stitch;
    /* What the flush of its data took the medium in the last cycle that settled —
     * the cycle whose busy time the cadence keeps as D (cadence.c, cadence.d_us): the
     * Settle's holder's alone, written as that cycle ends and read as the next
     * one's S0 takes the toll's D. */
    uint64_t settled_flush_us;
    /* No Quire took the write: the book takes none until the volume is mounted
     * again. */
    volatile uint8_t book_refused;
    /* The core putting the verdict's tags on, + 1; 0 while none is. */
    volatile uint32_t marking;
} SewingState;

/* What one volume holds of ribbon_left.c (TagFSVolume, volume/tagfs_volume.h). */
typedef struct {
    BookLeftMark* marks;
    uint32_t nmarks;
    uint32_t capmarks;
    uint64_t lacking;  /* the newest stitch to be held to whose mark this kernel lacks; 0 none */
    uint64_t seen;  /* the largest seq the mount saw on the medium (BookSeqSeen) */
    RibbonTry* tries;  /* newest first */
    RibbonPlace past;  /* where the ribbon goes on past the last stitch sewn */
} RibbonLeftState;

/* What one volume holds of ribbon.c (TagFSVolume, volume/tagfs_volume.h). */
typedef struct {
    RibbonCursor cursor;
    /*
     * What a change of a book may claim of the ribbon (RibbonClaim), published by
     * the Settle's holder each time the cursor or the stitch in flight moves, and
     * by the mount, and read under the state lock by Ink: the bytes of records the
     * next stitch can carry past the one in flight, and of a stitch of freeing
     * records alone; whether the ribbon may still be given a next_quire, and the
     * free count when the last ask for one was refused. A move that shrinks the
     * room publishes it before what it took is let go, so a claim is judged
     * against too little room, never too much.
     */
    volatile uint64_t room_any;
    volatile uint64_t room_frees;
    volatile uint8_t room_grows;
    volatile uint64_t refused_free;
    /* The ribbon stands with no next_quire, its last ask refused past the reserve
     * — or answered with one found only on ground a Tally a mount may stand on
     * keeps from it (fallow.c, NodeBarred): published with the room. A change
     * short of room on the ribbon waited on a Tally for it, and no change that
     * does not free has been given room since (RibbonClaimSay, RibbonRoomFor):
     * the ribbon's shortfall is the volume's, not a change's, so any change given
     * room past it ends it. A change refused room on the ribbon, said until one
     * is given room (RibbonRefusedSaid). Any core, no lock; let go at mount. */
    volatile uint8_t room_refused;
    volatile uint8_t room_barred;
    volatile uint8_t room_waited;
    volatile uint8_t refused_said;
    /* The slice of the last Tally's DONE that asked the next_quire again, and how
     * many in a row took that same stitch in (RibbonNextAgain). By the Settle's
     * holder. */
    uint64_t again_slice;
    uint32_t again_turns;
    /* The bytes claimed by changes no stitch has taken, and those held for changes
     * in the making — a tag about to be given an id, a write's record while its
     * data moves (RibbonReserve): under the state lock. */
    uint64_t claimed;
    uint64_t reserved;
    /* The sectors the stitch in flight takes at the most; 0 with none. Set under
     * the state lock at TAKE, cleared by the Settle's holder at the cycle's end. */
    volatile uint64_t flight;
    /* A stitch with no word aboard that the rest and the next_quire do not hold,
     * said once a mount. */
    volatile uint8_t long_said;
    /* A stitch that ends inside a flash page, said once a mount (RibbonPlaced);
     * the last Quire begun off a page gone on into, said once, its block + 1
     * (RibbonEnteredSaid). */
    volatile uint8_t inside_said;
    volatile uint64_t entered_said;
} RibbonState;

#endif /* TAGFS_SETTLE_SEWING_H */
