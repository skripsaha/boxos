#ifndef TAGFS_SETTLE_BINDER_H
#define TAGFS_SETTLE_BINDER_H

#include "settle_internal.h"
#include "sewing.h"
#include "endpaper_fact.h"
#include "../mount/tally_read.h"
#include "../file_map/file_record.h"

/*
 * The Binder — the kernel's writer of a format-v2 volume's Tally (spec_v2
 * §4.1–§4.4, §16.2 пп.4–6, 24, §16.3 пп.1–5, §16.4 п.5): the state of the book
 * after one settled stitch, every tree written whole in two copies, and the
 * Spine slot of the least seq made to name it.
 *
 *   binder.c        the Tally's steps — TALLY_TAKE, TALLY_NODES, TALLY_FLUSH,
 *                   TALLY_SLOT, TALLY_SLOT_FLUSH, TALLY_DONE — when one is
 *                   owed (TALLY_SLACK, SETTLE_DEMAND_TALLY), the slot, and the
 *                   ground the slots stop holding (FALLOW_BY_SPINE: a
 *                   Tally-Quire whole, once no slot names a node in it)
 *   binder_slice.c  the slice a Tally is written of, and the ground DIGESTS
 *                   speaks for
 *   binder_done.c   a Tally's DONE, and a slot whose write was refused held as
 *                   naming either Tally
 *   binder_nodes.c  the nodes: the elements of each tree in key order, packed
 *                   by the one rule every writer of a Tally keeps — the same as
 *                   mkfs's (tools/create_tagfs_bind.c) and volume-read's
 *                   (tools/volumeread/binder.py) — and laid at A and B
 *   binder_items.c  the elements of each tree, as they come out of memory
 *   births.c        the seq that put each run of this mount's writing in, for
 *                   the runs a Tally names (a run in memory is born 0 until a
 *                   mount reads it back)
 *
 * ‼ A WORK OF THE SAME SCRIBE, BETWEEN ITS CYCLES (§4.3). A Tally is written a
 * step a turn of the Settle's holder, the cycles going on between its steps:
 * stitches k+1, k+2… are sewn while the nodes of the state after k are
 * measured and laid, and a waiter's anchor waits for one turn of a Tally at
 * most, never for a Tally. A turn is a node measured or laid, a leaf of the
 * DIGESTS read to know whether it rots, a flush, the slot — and the slice,
 * taken in one turn right after the S6 of the stitch it is the state after, so
 * no stitch settles inside it: memory alone, a batch of files under the state
 * lock at a time, as long as a walk of every live file and its runs takes.
 *
 * ‼ A SLOT IS WRITTEN ONLY OVER A WHOLE TALLY: its nodes, their flush, the
 * slot, its flush (§4.3) — a slot that holds names a Tally that holds. A Tally
 * refused before its slot is nobody's on the medium (C7, C8), and its ground
 * goes back; one refused at its slot or its flush leaves the slot either whole
 * or torn (C9, C10), so the slot is held as naming both what it named before
 * and the new Tally until a Tally is written over it again.
 */

/* The Tally's steps (binder.c). */
typedef enum {
    TALLY_TAKE = 1,
    TALLY_NODES,
    TALLY_FLUSH,
    TALLY_SLOT,
    TALLY_SLOT_FLUSH,
    TALLY_DONE,
} TallyStep;

/*
 * ‼ TALLY_SLACK — A POLICY, NOT A DERIVATION (§4.2): a Tally is owed once the
 * useful bytes of the ribbon past the newest slot's `at` — the records of the
 * stitches, from their StitchHead to their Clasp, not the sectors' heads nor
 * the PAD — reach TALLY_SLACK times what the next Tally costs: the nodes it
 * lays in both copies and its slot, 4 KiB a block — laid in, the leaves the
 * keys changed since meet and their path, never more than the whole book
 * (binder_cost.c, §16.5 п.16). At 1 the metadata a write puts on the medium
 * is at most twice its useful bytes, and the ribbon a mount replays is never
 * longer than a Tally.
 */
#define TALLY_SLACK  1u

/* --- the Settle's holder ------------------------------------------------- */

/* Whether the Tally owes a step: one is under way, or owed — TALLY_SLACK
 * reached or a demand — and may be begun: a stitch of this mount settled, the
 * volume takes writes and is not under glass. From any core, lock-free: it
 * reads only what the Settle's holder publishes (BinderWeigh) and flags kept
 * as atomics, never the slots' or the chain's memory, which the holder moves. */
bool        BinderOwed(void);

/* Whether a Tally is owed and held back by nothing but the Slips the settled
 * state carries (LooseLeaves) — or by nothing at all: no write lays a new
 * Slip meanwhile (plot_slip.c), and the Helm's service lays each tail in its
 * block (helm/engross.h). From any core, lock-free, as BinderOwed. */
bool        BinderEngrossOwed(void);

/* Whether the volume takes no write a Tally could go on with — only read,
 * under glass, refused by its book, its medium gone, or the machine halting:
 * flags kept as atomics, asked from any core. */
bool        BinderNoWrite(void);

/* One step of the Tally, by the cadence's committer: the Settle's Scribe taken
 * as a pass of its own that answers no asker, the step run, the Scribe given
 * back. False when the Scribe was another's. With no TagFS lock held. */
bool        BinderTurn(void);

/* A whole Tally written now by the caller, who may wait for it (the stand):
 * a step a turn of the Scribe, as the cadence would. OK, ERR_BUSY when none
 * may be begun — why said — or the step's refusal. */
error_t     BinderRunWhole(void);

/* The Tally a write short of room waits on (BinderRoomAwaited), written by
 * that write's own wait for room (settle.c, SettleForRoomBook) as
 * BinderRunWhole writes one — but never begun past the tails' Engross, which
 * is the Helm service's: ERR_WOULD_BLOCK, said by nothing, when none may be
 * begun now. */
error_t     BinderRunAwaited(void);

/* The stitch `seq` settled carrying `useful` bytes of records, the highest
 * file id a record of it named `fid_most`, and the births of the runs it put
 * in noted — `births`, ERR_NO_MEMORY when memory would not keep them (and no
 * Tally is written by this mount from there) — at its S6, by the Settle's
 * holder with no lock: the useful bytes counted toward TALLY_SLACK. */
void        BinderStitchSettled(uint64_t seq, uint64_t useful, uint64_t fid_most, error_t births);

/* A Tally asked for: the ribbon could be given no next_quire
 * (SETTLE_DEMAND_TALLY). A write short of room asks cycles, not a Tally — one
 * takes a turn a node, and gives back only what the slot it is written over
 * held. From any context. */
void        BinderDemanded(void);

/* A Quire given the ribbon, in its order (ribbon.c): the ribbon's ground the
 * slots hold from their `at` on. By the Settle's holder, or the mount. */
void        BinderRibbonGiven(const RibbonQuire* q);

/* The volume stopped taking writes — the book refused a stitch, the volume
 * went read-only, its medium left: the Tally under way is ended there, its
 * ground going back as a refused step's does (a slot whose write began held
 * as naming either Tally). By the Settle's holder, with no lock held. */
void        BinderStopped(error_t rc);

/* Whether a Tally is under way: no ground goes back meanwhile (sewing.c holds
 * Fallow's gives) — the slice names blocks by the state it was taken of. */
bool        BinderHoldsGround(void);

/* The end of the book's own ground `block` lies in — a Tally-Quire a slot
 * holds or the pair holds, whole on the grid of the ribbon's Quire, or a
 * Quire of the ribbon, on the chain or a slot's off it — or `block` itself
 * where it is none of these. By the Settle's holder, who alone moves them. */
uint64_t    BinderOwnPast(uint64_t block);

/* --- the mount, with the door shut ------------------------------------- */

/* Everything the Binder keeps let go — the volume let go of, or mounted
 * afresh. */
void        BinderMountReset(void);

/* Room for the Spine's ring of `ring` slots, as its FRAME names them — the
 * ring a volume's maker chose is the one its writer of a Tally keeps.
 * ERR_NO_MEMORY. */
error_t     BinderSlotsMake(uint32_t ring);

/* What the mount read, for the Binder: slot `index` that holds, at `seq`,
 * naming the Tally `tally_seq` — every copy of every node it names (`start`,
 * `count` blocks, written at `birth`) and every Quire of its ribbon, in order.
 * ERR_NO_MEMORY. */
error_t     BinderSlotHolds(uint32_t index, uint64_t seq, uint64_t tally_seq, uint64_t stitch_seq);
error_t     BinderSlotNode(uint32_t index, uint64_t start, uint64_t count, uint64_t birth);
error_t     BinderSlotRibbon(uint32_t index, const RibbonQuire* q);

/* The mount stood on slot `index`: the blocks its Tally takes in one copy, the
 * useful bytes of the stitches it replayed past it, and the numbers the
 * medium has shown — what the Tally's slot never names less than. Why this
 * mount writes no Tally at all, when it is so, said here. */
void        BinderMountStood(uint32_t index, uint64_t tally_blocks, uint64_t useful, uint64_t next_fid,
                             uint64_t next_tag, uint64_t next_bookmark);

/* Why no Tally is written by this mount whatever is owed — memory would not
 * hold what the Tallies need — or NULL. */
const char* BinderNoTallies(void);

/*
 * ‼ THE RING IS FILLED BEFORE IT GIVES ANYTHING BACK (§16.2 п.6, §1.4). A
 * Tally written over a slot gives back the Tally that slot held; one written
 * over a slot that held nothing — two of a fresh volume's three — or a small
 * Tally of the volume as it was gives back nothing, or less than it took. So
 * "a ribbon's step and one Tally are always possible" holds only when the
 * reserve also keeps what the ring's slots lack of a whole Tally: for `tally`
 * blocks of one whole Tally, both copies, in whole Quires of `quire`, the sum
 * over every slot of what its own Tallies' ground — the whole Quires a Tally
 * written over it gives back (binder_slots.c, SlotOwn) — falls short of it
 * (EndpaperKeeps adds it). From any core, lock-free: each slot's own ground is
 * published by the Settle's holder.
 */
uint64_t    BinderRingShort(uint64_t tally, uint64_t quire);

/*
 * ‼ A WRITE SHORT OF ROOM WAITS WHILE A TALLY GIVES GROUND BACK (§3.6: a
 * request that frees nothing waits, ERR_WOULD_BLOCK, while Fallow rests ground
 * or what the slots hold would come back; ERR_TAGFS_NO_SPACE only past that).
 * Two grounds a Tally gives back that nothing else does:
 *   - the reserve's measure (§16.4 п.7): the last Tally's exact size and,
 *     from above, the growth settled since (endpaper_fact.c) — each Quire of
 *     it kept twice, once for the Tally and once for each slot holding less
 *     (BinderRingShort). A GiB written past the last Tally measured 980 blocks
 *     a copy, the two Tallies after it 658 and 610: 7680 kept where 6400 did,
 *     and the GiB written the moment a GiB was deleted came 1280 blocks short
 *     of a volume that had them (fullvolume roomback). The Tally makes the
 *     measure exact (EndpaperGrown);
 *   - what the slots hold that the newest does not — every Quire of the ribbon
 *     but the one it stands in and its next_quire, and a slot's own nodes and
 *     the Tally-Quires they hold whole — given back once a Tally is written
 *     over each (SlotGoes), published by the Settle's holder as it weighs
 *     (BinderWeigh, BinderTurnGivesPublish).
 * BinderRoomWait, by a write refused past the book's fresh measure with
 * Fallow empty: true — the write waits — while fewer Tallies than the ring
 * holds slots ended since a write was last handed ground, and a Tally is under
 * way — its end gives its slot's ground back — or one of the two stands. A
 * Tally under way at every ask spends the turn as one asked would: a Tally
 * begun between two asks (the ribbon's, RibbonNextAgain; the useful bytes')
 * never holds the write past it. A Tally may be begun: it is asked
 * (SETTLE_DEMAND_TALLY), and its end — DONE or refused — counts a turn and
 * asks the cycle that wakes the write (BinderRoomEnded); the ground the slot
 * let go rests in Fallow, whose own wait carries the write from there. None
 * may be begun for a reason the next stitch takes away — the ribbon on its
 * Quire's edge: a cycle asked for room closes a Quire early, and the take
 * after Fallow's wait meets it there — a cycle is asked. False once a whole
 * turn of the ring is spent, none may be begun for any other reason, or the
 * volume takes no write: the write is refused. BinderRoomAwaited: a Tally so
 * asked and not ended — the write's own wait for room writes it (settle.c,
 * SettleForRoomBook; BinderRunAwaited).
 * BinderRoomTaken: a write was handed ground — the next shortfall has its
 * turn of the ring again. BinderRoomMeasured, as a write is refused: how the
 * reserve it met was measured — to the block, nothing grown past the last
 * Tally; or from above, a turn of the ring waited for, or no Tally waited for
 * and *why none may be (NULL: none was asked). From any core, no TagFS lock
 * held.
 */
bool        BinderRoomWait(void);
const char* BinderRoomMeasured(const char** why);
bool        BinderRoomAwaited(void);
void        BinderRoomTaken(void);
void        BinderRoomEnded(void);
void        BinderTurnGivesPublish(void);

/* The ground the book holds itself, in blocks: every Quire of the grid a slot
 * that holds names, the current pair's no slot counts its own, and the Quires
 * of the ribbon the slots hold — named, each once (binder_slots.c). Published
 * by the Settle's holder wherever one of them moves; read from any core, no
 * TagFS lock held: said beside what the book keeps when a write is refused at
 * a full volume's wall (allotment.c). */
void        BinderGroundHeldPublish(void);
uint64_t    BinderGroundHeld(void);

/*
 * The stitch the oldest Tally a mount may stand on took in, given to Fallow,
 * which keeps from the ribbon the ground a record past it gave back
 * (fallow.c, NodeBarred): the newest slot that holds a Tally — the FallowLag()
 * newest once the medium lied, as many as a mount may fall back past a slot a
 * lying medium lost — each by the older Tally it may hold while its write is
 * unsure, and `floor`, the slot a mount stood on when it is not the newest
 * (UINT64_MAX for none). A slot held unsure over one that held nothing may
 * hold the new Tally or none: a mount whose write there tore stands on the
 * newest slot before it, so it spends no step of the lag — its slice is taken
 * and the next newest asked too. By the Settle's holder, or the mount, with no
 * TagFS lock held: at the mount and at each Tally's DONE.
 */
void        BinderBarPublish(uint64_t floor);

/* A node of the Tally stood on no copy of which holds — kept to be named by
 * every Tally after as it lies (§16.4 п.5): its tree, place, and level.
 * ERR_NO_MEMORY. */
error_t     BinderRotten(uint64_t tree, const TallyPlace* at);

/* --- births.c ------------------------------------------------------------ */

/*
 * ‼ A RUN IS BORN AT THE STITCH THAT PUT IT IN, AND MEMORY DOES NOT SAY WHICH
 * (§2.9 birth_delta). A run a mount reads carries its birth; one this mount's
 * writes put in is born 0 in memory — the Ink that made it knows no seq — and
 * stays 0: its Parcel is shared by every record that names it, and a copy an
 * Ink takes of it while the stitch is in flight would keep the 0 it found.
 * So the stitch that settles names the ground it put in here, by its first
 * block, and a Tally asks it for the birth of each run born 0. The ground goes
 * out of it when it goes back to the allocator (Fallow) — a run of it a write
 * cut in two stays one entry, named by both halves.
 *
 *   BirthsNoted   the DATA Strips `strips` put in by stitch `seq`: ERR_NO_MEMORY
 *   BirthsOf      the seq that put in the ground of `block`, 0 when none did
 *   BirthsForget  the ground [start, start + count) gone back: what of it was
 *                 named goes out, an entry cut short at either end — under
 *                 the state lock, nothing asked of the heap or given it
 *   BirthsTrim    the room BirthsForget emptied given the heap, with no TagFS
 *                 lock held
 *   BirthsLetGo   all of it, with the volume
 *
 * The Settle's holder alone writes and reads it — the stitch's S6, Fallow's
 * gives at a cycle's DONE, the Tally — so it takes no lock.
 */
error_t     BirthsNoted(const Strip* strips, uint64_t count, uint64_t seq);
uint64_t    BirthsOf(uint64_t block);
void        BirthsForget(uint64_t start, uint64_t count);
void        BirthsTrim(void);
void        BirthsLetGo(void);

/* --- the Binder's own files (binder*.c) alone --------------------------- */

/* A file of the slice: its id, and the record of it the state after the
 * stitch names — held for as long as the Tally is being written. */
typedef struct {
    uint64_t    fid;
    FileRecord* rec;
} BinderFile;

#define BINDER_FILES_CHUNK  255u

typedef struct BinderFiles BinderFiles;
struct BinderFiles {
    BinderFiles* next;
    uint32_t     n;
    BinderFile   f[BINDER_FILES_CHUNK];
};

/* A run of ground DIGESTS speaks for: a live file's, or one held (§16.1 п.5). */
typedef struct {
    uint64_t start;
    uint64_t count;
} BinderGround;

/*
 * BinderKept — a node of the Tally before named by a Tally as it lies, by the
 * pointer, the separator and the count its parent gave it (§16.4 п.5, §16.5
 * п.16): one no copy of which reads (`rotten`), or one nothing under which
 * changed (TipIn, tip_in.h). Of these, a leaf beside a row laid anew
 * (`margin`) is taken into the row by the Tally's measure when the row comes
 * to less than half a Sheet (`taken`: laid with it, no longer named).
 */
typedef struct {
    uint64_t   tree;
    TallyPlace at;
    bool       rotten;
    bool       margin;
    bool       taken;
    uint64_t   items;       /* a leaf: what it holds, as Foliation knows it (foliation.h) */
    uint64_t   text;
} BinderKept;

/* A node as the level above names it: its first key, the count it carries,
 * its Sheets and the blocks of its two copies. */
typedef struct {
    TallyKey key;
    uint64_t count;
    uint64_t sheets;
    uint64_t block[2];
    uint64_t items;      /* a leaf laid: what it holds (FoliationNode) */
    uint64_t text;
} BinderKid;

typedef struct {
    BinderKid* kid;
    uint32_t   n;
    uint32_t   cap;
} BinderLevel;

#define BINDER_LEVELS_MOST  24u     /* a tree of 200 a node is shallower than this for any volume */

/* A tree's root as the slot names it: one laid, or the root of the Tally
 * before named as it lies — a tree nothing changed in. */
typedef struct {
    bool     laid;
    uint64_t level;
    uint64_t items;
    TallyPtr ptr;
} BinderRoot;

/* The element handed to the node being filled. */
typedef struct {
    TallyKey key;
    uint64_t count;
    uint8_t* body;
    uint32_t len;
    uint32_t text;       /* a tag: the bytes of its key and value */
} BinderItem;

/* Where a walk of DIGESTS' elements over a ground stands: the run in hand, the
 * next block looked at, and the first of the tree's nodes named as they lie
 * not behind it — a rotten range's elements are left out (binder_items.c). */
typedef struct {
    uint64_t ground;
    uint64_t next;
    uint32_t rot;
} BinderDigestAt;

/* The elements of the tree being laid, as they come (binder_items.c). */
typedef struct {
    uint32_t     tree;
    BinderFiles* chunk;          /* FILES: the slice, where the walk stands */
    uint32_t     at;
    bool         head;           /* ... the file's header handed */
    StripWalk    walk;
    uint64_t     block;          /* ... the file block its next Parcel begins at */
    uint32_t     kept;           /* FILES: the next file locked by name (book_mount.h, BookKeptFile), ... */
    uint32_t     kept_parcel;    /* ... its next Parcel, and whether its header was handed */
    bool         kept_head;
    uint32_t     tag;            /* TAGS: the next id looked for */
    bool         use;            /* USE: handed */
    BinderDigestAt digest;       /* DIGESTS: the walk over the ground — and a STRICT kernel's laid in, ... */
    BinderDigestAt tip_at;       /* ... its walk over the ground the Tally laid in speaks for, the range it */
    uint64_t     tip_range;      /* ... lays and offers the next element is looked for in, and its body */
    uint8_t*     tip_buf;
    BinderItem   item;           /* the element handed and not yet in a node */
    bool         held;
    uint8_t*     buf;            /* its body */
    uint32_t     room;
} BinderItems;

/* A node being filled: its elements' bytes one after another, where each
 * begins, the keys, and the count it carries. */
typedef struct {
    uint8_t*  bytes;
    uint32_t  len;
    uint32_t  cap;
    uint32_t* starts;
    uint32_t  n;
    uint32_t  capn;
    TallyKey  first;
    TallyKey  last;
    uint64_t  count;
    uint64_t  items;     /* a leaf: the runs of its Parcels, or its tags, and their text */
    uint64_t  text;
} BinderFill;

/* A slot's unit as a write of it sealed it — its eight sectors — kept while
 * the slot is held unsure: whichever of them the medium holds there is this
 * kernel's own Tally, known byte for byte, never by its seq alone — a seq this
 * kernel gave and the medium never showed, another machine that never saw it
 * may give again (R11; mount/perambulation.c). */
typedef struct SlotSealed SlotSealed;
struct SlotSealed {
    SlotSealed* next;
    uint8_t     unit[TAGFS_BLOCK_SIZE];
};

/* The slots as this mount knows them: what each holds (§1.4, M-7). */
typedef struct {
    bool         holds;        /* a Tally, as far as this mount knows */
    bool         unsure;       /* a write of it was refused: it may hold either Tally */
    uint64_t     seq;
    uint64_t     tally;
    uint64_t     slice;        /* the stitch its Tally took in — the older one's while it is unsure */
    uint64_t     tally2;       /* unsure: the Tally that may stand in it instead; 0 */
    SlotSealed*  sealed;       /* unsure: every unit a refused write of it sealed, newest first */
    Strip*       ground;       /* its Tallies' nodes, each copy, born at `birth`, by first block */
    uint32_t     nground;
    uint32_t     capground;
    Strip*       quires;       /* the Tally-Quires its Tallies name a node in as it lies, whole, in order
                                * (binder_quires.c) — none for a slot a mount walked: its ground names them */
    uint32_t     nquires;
    uint32_t     capquires;
    RibbonQuire* extra;        /* its ribbon's Quires off the chain */
    uint32_t     nextra;
    uint32_t     capextra;
    uint64_t     chain_from;   /* where its ribbon begins on the chain; UINT64_MAX for none */
    uint64_t     under;        /* the ground a Tally under way over it took off the free, counted its own */
    uint64_t     rest;         /* what the Tally before it let go, resting in Fallow, counted its own ... */
    uint64_t     rest_until;   /* ... until no node of it ripe by this cycle rests in Fallow */
} SlotGround;


typedef struct {
    bool           up;
    SlotGround*    slot;        /* the Spine's ring: as many slots as its FRAME names (BinderSlotsMake) */
    uint32_t       ring;
    uint32_t       nslots;      /* past the highest that holds */
    uint64_t*      own;         /* each slot's own Tallies' ground in blocks, published (BinderRingShort) */
    RibbonQuire*   chain;       /* the ribbon's Quires in its order, from chain_base on */
    uint64_t       nchain;
    uint64_t       capchain;
    uint64_t       chain_base;
    BinderKept*    rot;         /* the nodes the mount found no copy of reads */
    uint32_t       nrot;
    uint32_t       caprot;
    BinderGround*  held;        /* what the mount holds for what rotted */
    uint64_t       nheld;
    uint64_t       capheld;
    uint64_t       seen_fid;    /* the numbers the medium has shown */
    uint64_t       seen_tag;
    uint64_t       seen_bookmark;
    uint64_t       laid_fid;    /* past the highest file id a stitch of this mount named */
    uint64_t       useful;      /* the ribbon's useful bytes past the newest slot's `at` */
    uint64_t       tally_blocks;/* the last Tally's whole book — every node it names, laid or kept — one copy */
    uint64_t       cost_known;  /* the next Tally's cost as last reckoned, bytes: no more than it is while no key
                                 * went since (binder.c, TallyDue) — but by a leaf only a write under way kept live
                                 * then, one given up since (binder_live.c) — 0 once what it is reckoned by may
                                 * have shrunk */
    uint64_t       cost_gone;   /* DogEarGoneCount when it was: a key gone since may have made it less */
    uint64_t       pair_a;      /* the Tally-Quires the next Tally's nodes go on in: copy 1's run, ... */
    uint64_t       pair_b;      /* ... copy 2's at as many blocks into its own */
    uint64_t       pair_room;   /* the blocks each run holds */
    uint64_t       pair_used;   /* ... and the blocks the Tallies laid in them took */
    uint64_t       tallies;
    const char* volatile no_tallies; /* why no Tally is written by this mount at all, or NULL */
    volatile uint8_t demanded;
    volatile uint8_t due;       /* TALLY_SLACK reached: published by the Settle's holder */
    volatile uint8_t engross;   /* owed, and held back by nothing but the settled Slips: published */
    const char* volatile may_not;/* why none may be begun as the holder's own state stands, or NULL: published */
    const char*    said;        /* the last such reason said while one was owed */
    volatile uint64_t retry_after; /* a refused Tally is tried again once this cycle settled */
    volatile uint8_t  turn_gives;  /* a Tally over the slots but the newest gives ground back: published */
    volatile uint8_t  room_waits;  /* a write short of room waits on the Tally it asked: a cycle at its end */
    volatile uint32_t room_turns;  /* Tallies ended for writes short of room since one was handed ground */
    volatile uint64_t ground_held; /* the ground the book holds itself, blocks: published (BinderGroundHeld) */
} BinderBook;

extern BinderBook g_binder;

/* binder_slots.c: room for one more of `size` bytes in a list; a slot's lists
 * let go; its ground in order of first block; a Quire's place on the chain
 * (UINT64_MAX off it); what a slot written over held that no slot holds now
 * rested in Fallow — its blocks, the chain before every slot's ribbon let go
 * of in memory; the current pair of Tally-Quires let go for a new one, by
 * Quires of `quire` blocks. */
bool        BinderListRoom(void** list, uint64_t n, uint64_t* cap, size_t size);
bool        BinderListRoom32(void** list, uint32_t n, uint32_t* cap, size_t size);
void        BinderSlotLetGo(SlotGround* s);

/* binder_done.c: whether `unit`, the eight sectors read off slot `slot`, is
 * byte for byte a unit this kernel sealed into it since it was held unsure.
 * By the Settle's holder, or with the Settle held still (StitchHoldStill). */
bool        BinderSlotSealed(uint32_t slot, const uint8_t* unit);
void        BinderSlotSort(SlotGround* s);
uint64_t    BinderChainIndex(const RibbonQuire* q);
uint64_t    BinderSlotGoes(const SlotGround* old);

/* The slot the next Tally is written over: the one of the least seq, one that
 * holds nothing — or may hold either of two — least of all (§1.4). By the
 * Settle's holder. */
uint32_t    BinderSlotNext(void);

/* The stitch BinderBarPublish gives Fallow, of `n` slots `slots`: the least
 * slice of the `lag` newest that hold — one held unsure over nothing spends
 * no step — and `floor`. Memory alone. */
uint64_t    BinderBarSlice(const SlotGround* slots, uint32_t n, uint64_t lag, uint64_t floor);
void        BinderPairLetGo(uint64_t quire);

/* binder_slots.c: slot `index`'s own ground published, `more` blocks a Tally
 * under way took off the free for it added — BinderSlotsOwnPublish: every
 * slot's, as each stands; the ground a slot's Tally before let go counted its
 * own no more once Fallow gave it back, in the hold of the state lock that
 * gave it: once `resting_from`, the least cycle a node of that ground still
 * resting ripens at, is past its own (BinderRestRipens, sewing.c,
 * FallowSpineRipeFrom); what every slot but `index`
 * lacks of `tally` blocks, in whole Quires of `quire`; no Tally written by
 * this mount from here, for `why` (said by the caller). binder.c: BinderOwed's
 * words published again (BinderWeigh). */
void        BinderSlotOwnPublish(uint32_t index, uint64_t more);
void        BinderSlotsOwnPublish(void);
void        BinderRestRipens(uint64_t resting_from);
uint64_t    BinderRingShortBut(uint64_t tally, uint64_t quire, uint32_t index);
void        BinderNoTalliesFrom(const char* why);
void        BinderWeigh(void);

/* What the mount holds for what rotted, for DIGESTS (book_ground.c). */
error_t     BinderHeldRun(uint64_t start, uint64_t count);

/* What a Tally is written of, and where it is written to. */
typedef struct {
    uint64_t      seq;           /* the Tally's */
    bool          tip;           /* laid in among the nodes of the Tally before (TipIn) — not whole */
    BinderFiles*  files;         /* the slice, by fid */
    uint64_t      nfiles;
    uint32_t      tag_hi;        /* the tags below it are the book's */
    UseText*      use;
    BinderGround* ground;        /* by first block: what DIGESTS names — laid in, in the leaves it lays and */
    uint64_t      nground;       /* ... offers; a STRICT kernel's every block of the state, and ... */
    BinderGround* tground;       /* ... the ground laid in apart, held to it in the ranges it lays and offers */
    uint64_t      ntground;
    BinderGround* dranges;
    uint64_t      ndranges;
    BinderKept*   kept;          /* the nodes named as they lie, by tree, then key */
    uint32_t      nkept;
    uint64_t      a;             /* the first blocks of the runs the copies go in */
    uint64_t      b;
    uint64_t      room;          /* the blocks each run holds */
    uint32_t      grid;          /* the atom, in blocks */
    bool          measuring;     /* nothing written: the blocks counted */
    uint64_t      off;           /* the next node's place in the runs */
    uint32_t      tree;          /* the tree being laid, and its level */
    uint32_t      level;
    uint32_t      kid_at;        /* level >= 1: the next node of the level below */
    uint32_t      kept_at;       /* ... and the next node named as it lies, of the level below */
    uint32_t      tree_kept;     /* where the tree's own begin in `kept`, and where they end */
    uint32_t      tree_kept_end;
    uint32_t      bar_at;        /* the first of the tree's not behind the node being filled */
    uint32_t      cover_at;      /* level 0: the first of the tree's whose range ends past the last key */
    uint64_t      tree_off;      /* where the tree's nodes began in the runs: its measure begun again */
    uint64_t      row;           /* level 0: the bytes of the leaves laid since the last leaf named as it
                                  * lies, and of the one being filled */
    uint32_t      row_left;      /* the margin before the row, + 1; 0 for none */
    bool          keyed;         /* a key of this level was passed: the fill's last is it */
    bool          rows_short;    /* a row short of half a Sheet took the margin before it in: the tree's
                                  * measure is begun again at its end, ... */
    bool          row_again;     /* ... once, and was */
    BinderItems   items;
    BinderFill    fill;
    BinderLevel   lv[BINDER_LEVELS_MOST];
    BinderRoot    root[TALLY_SHARED + 1u];
    uint8_t*      unit;          /* a node sealed, for the medium */
    uint64_t      unit_room;
    uint64_t      nodes;         /* laid */
    uint64_t      sheets;        /* ... their Sheets, one copy */
    uint64_t      named;         /* named as they lie */
    uint64_t      dsheets;       /* the Sheets of the DIGESTS leaves of the Tally before it names as they lie,
                                  * or under a node it names so — the reads' table's, never Foliation's */
    uint64_t      dropped;       /* DIGESTS elements inside a rotten range, left out */
    bool          trees_done;
    BinderKid*    dleaves;       /* the DIGESTS leaves laid and named, for the reads to turn to (binder.c) */
    uint32_t      ndleaves;
    bool          dleaves_lost;  /* memory would not keep them */
    EndpaperLaid  laid;          /* what the walk laid of FILES, TAGS and DIGESTS: the next reserve's measure */
} BinderWork;

/* binder_nodes.c: the walk of every tree begun over again — the slice, A, B
 * and `measuring` set; the next node laid (both copies, unless measuring) —
 * *done once every tree has its root; everything the walk holds let go. OK,
 * ERR_NO_MEMORY, the medium's refusal, or ERR_TAGFS_CORRUPTED for a run whose
 * birth nobody knows (said). */
void        BinderNodesBegin(BinderWork* w);
error_t     BinderNodeNext(BinderWork* w, bool* done);
void        BinderNodesLetGo(BinderWork* w);

/* binder_nodes.c: the reads of a digest turned to the DIGESTS the walk laid —
 * its leaves, and the rotten nodes it names as they lie, in key order. OK —
 * the Gloss may let go of what it names; ERR_BUSY, nothing changed, while a
 * read is still in the table the turn takes (tally_read.h, TallyDigestsTurn);
 * ERR_NO_MEMORY, said: the reads stay on the Tally before, and the Gloss keeps
 * every digest. */
error_t     BinderDigestsTurn(const BinderWork* w);

#ifdef CONFIG_VOLUME_TESTS
/* The stand's hands on a Tally: written whole now (BinderRunWhole), stopped
 * before step `at` — the volume left as a cut there leaves it, nothing more
 * written — or its slot torn, `sectors` of its eight written: eight, the slot
 * written whole and its write refused. 0 for neither. */
void        BinderStandStopAt(TallyStep at);
void        BinderStandTearSlot(uint32_t sectors);

/* The wall's wait on a Tally held down, as a ring with nothing to give back
 * holds it: armed, BinderRoomWait answers false and a take short of room past
 * the book's measure is refused there, the refusal saying the stand holds it
 * down; false takes it down. Whether it stood armed — the runner takes it down
 * after every test and fails one that left it (tests.c). A test of the reserve
 * a write of data leaves, or of the blocks Slips are promised, weighs them
 * where the measure stands: at the wall a Tally written for room lays the
 * Slips' tails and gives a slot's ground back — the stand_book_room suite's to
 * weigh. */
bool        BinderStandNoRoomWait(bool armed);

/* What the Binder holds, for the stand: the slot it would write next, the
 * useful bytes past the newest slot's `at`, what the next Tally is reckoned to
 * cost in bytes, the Quires of the ribbon the slots hold, the blocks the last
 * Tally took in one copy, the elements its self-check held. */
typedef struct {
    uint32_t next_slot;
    uint64_t useful;
    uint64_t cost;
    uint64_t ribbon_quires;
    uint64_t tally_blocks;
    uint64_t tallies;        /* written by this mount */
    bool     owed;
    bool     demanded;       /* owed whatever the ribbon holds: a Glean its DONE chose, DogEar lost */
    uint64_t pair_left;      /* the blocks of the pair of Tally-Quires no node was laid in yet: the book's,
                              * out of the free count, kept for the next Tally to go on in */
    uint64_t checked;        /* elements of the last Tally its self-check held to the Tally before */
} BinderStandSaid;
void        BinderStand(BinderStandSaid* out);

/* Every run of ground the book's Tallies and ribbon hold now — the slots'
 * Tallies' nodes and their ribbons' Quires, and what of the pair of
 * Tally-Quires the next Tally goes on in is not laid yet — into `out` (room
 * `room`): how many there are. What a test that writes Tallies keeps as it
 * found it, for they may give it back. */
uint32_t    BinderStandGround(Strip* out, uint32_t room);

/* Every Quire the ribbon was given since BinderStandGivenBegin, across the
 * mounts between — what a test that runs the ribbon through Quires puts back
 * to zeros, so no stitch of it is found past the end of the ribbon it found:
 * into `out` (room `room`), how many. */
void        BinderStandGivenBegin(void);
uint32_t    BinderStandGiven(RibbonQuire* out, uint32_t room);
#endif

#endif /* TAGFS_SETTLE_BINDER_H */
