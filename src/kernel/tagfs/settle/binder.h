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
 *                   ground the slots stop holding (SpineHolds, FALLOW_BY_SPINE)
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
 * the PAD — reach TALLY_SLACK times what the next Tally costs: its nodes in
 * both copies and its slot, 4 KiB a block. At 1 the metadata a write puts on
 * the medium is at most twice its useful bytes, and the ribbon a mount
 * replays is never longer than a Tally.
 */
#define TALLY_SLACK  1u

/* --- the Settle's holder ------------------------------------------------- */

/* Whether the Tally owes a step: one is under way, or owed — TALLY_SLACK
 * reached or a demand — and may be begun: a stitch of this mount settled, the
 * volume takes writes and is not under glass. From any core, lock-free: it
 * reads only what the Settle's holder publishes (BinderWeigh) and flags kept
 * as atomics, never the slots' or the chain's memory, which the holder moves. */
bool        BinderOwed(void);

/* One step of the Tally, by the cadence's committer: the Settle's Scribe taken
 * as a pass of its own that answers no asker, the step run, the Scribe given
 * back. False when the Scribe was another's. With no TagFS lock held. */
bool        BinderTurn(void);

/* A whole Tally written now by the caller, who may wait for it (the stand):
 * a step a turn of the Scribe, as the cadence would. OK, ERR_BUSY when none
 * may be begun — why said — or the step's refusal. */
error_t     BinderRunWhole(void);

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
error_t     BinderSlotHolds(uint32_t index, uint64_t seq, uint64_t tally_seq);
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
 * over every slot of what its own Tallies' ground — counted in whole Quires —
 * falls short of it (EndpaperKeeps adds it). From any core, lock-free: each
 * slot's own ground is published by the Settle's holder.
 */
uint64_t    BinderRingShort(uint64_t tally, uint64_t quire);

/* A node of the Tally stood on no copy of which holds — kept to be named by
 * every Tally after as it lies (§16.4 п.5): its tree, place, and level.
 * ERR_NO_MEMORY. */
error_t     BinderRotten(uint64_t tree, const TallyPlace* at);

/*
 * SpineHolds — the ground of the volume's Tallies (FALLOW_BY_SPINE, §3.4,
 * M-7): whether a node born at `birth` (its pointer's seq) that the Tally
 * `death` first stopped naming is held by a slot — one whose Tally's seq lies
 * in [birth, death). UINT64_MAX for `death` is a node every Tally still names.
 */
bool        SpineHolds(uint64_t birth, uint64_t death);

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

/* A node of the Tally stood on that no copy of reads, named by the next Tally
 * as it lies (§16.4 п.5). */
typedef struct {
    uint64_t   tree;
    TallyPlace at;
} BinderRot;

/* A node as the level above names it: its first key, the count it carries,
 * its Sheets and its place in the two runs — or, a rotten node named as it
 * lies, the pointer it is named by. */
typedef struct {
    TallyKey         key;
    uint64_t         count;
    uint64_t         sheets;
    uint64_t         off;
    const BinderRot* rot;
} BinderKid;

typedef struct {
    BinderKid* kid;
    uint32_t   n;
    uint32_t   cap;
} BinderLevel;

#define BINDER_LEVELS_MOST  24u     /* a tree of 200 a node is shallower than this for any volume */

/* A tree's root as the slot names it. */
typedef struct {
    bool     laid;
    uint64_t level;
    uint64_t items;
    uint64_t sheets;
    uint64_t off;
} BinderRoot;

/* The element handed to the node being filled. */
typedef struct {
    TallyKey key;
    uint64_t count;
    uint8_t* body;
    uint32_t len;
} BinderItem;

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
    uint64_t     ground;         /* DIGESTS: the run of ground, and the block in it */
    uint64_t     next;
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
} BinderFill;

/* The slots as this mount knows them: what each holds (§1.4, M-7). */
typedef struct {
    bool         holds;        /* a Tally, as far as this mount knows */
    bool         unsure;       /* a write of it was refused: it may hold either Tally */
    uint64_t     seq;
    uint64_t     tally;
    uint64_t     tally2;       /* unsure: the Tally that may stand in it instead; 0 */
    Strip*       ground;       /* its Tallies' nodes, each copy, born at `birth`, by first block */
    uint32_t     nground;
    uint32_t     capground;
    RibbonQuire* extra;        /* its ribbon's Quires off the chain */
    uint32_t     nextra;
    uint32_t     capextra;
    uint64_t     chain_from;   /* where its ribbon begins on the chain; UINT64_MAX for none */
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
    BinderRot*     rot;
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
    uint64_t       tally_blocks;/* the last Tally's, one copy */
    uint64_t       tallies;
    const char* volatile no_tallies; /* why no Tally is written by this mount at all, or NULL */
    volatile uint8_t demanded;
    volatile uint8_t due;       /* TALLY_SLACK reached: published by the Settle's holder */
    const char* volatile may_not;/* why none may be begun as the holder's own state stands, or NULL: published */
    const char*    said;        /* the last such reason said while one was owed */
    volatile uint64_t retry_after; /* a refused Tally is tried again once this cycle settled */
} BinderBook;

extern BinderBook g_binder;

/* binder_slots.c: room for one more of `size` bytes in a list; a slot's lists
 * let go; its ground in order of first block; a Quire's place on the chain
 * (UINT64_MAX off it); what a slot written over held that no slot holds now
 * rested in Fallow — its blocks, the chain before every slot's ribbon let go
 * of in memory. */
bool        BinderListRoom(void** list, uint64_t n, uint64_t* cap, size_t size);
bool        BinderListRoom32(void** list, uint32_t n, uint32_t* cap, size_t size);
void        BinderSlotLetGo(SlotGround* s);
void        BinderSlotSort(SlotGround* s);
uint64_t    BinderChainIndex(const RibbonQuire* q);
uint64_t    BinderSlotGoes(const SlotGround* old);

/* binder_slots.c: slot `index`'s own ground published, `more` blocks a Tally
 * under way took for it added; what every slot but `index` lacks of `tally`
 * blocks, in whole Quires of `quire`; no Tally written by this mount from here, for `why` (said by the
 * caller). binder.c: BinderOwed's words published again (BinderWeigh). */
void        BinderSlotOwnPublish(uint32_t index, uint64_t more);
uint64_t    BinderRingShortBut(uint64_t tally, uint64_t quire, uint32_t index);
void        BinderNoTalliesFrom(const char* why);
void        BinderWeigh(void);

/* What the mount holds for what rotted, for DIGESTS (book_ground.c). */
error_t     BinderHeldRun(uint64_t start, uint64_t count);

/* What a whole Tally is written of, and where it is written to. */
typedef struct {
    uint64_t      seq;           /* the Tally's */
    BinderFiles*  files;         /* the slice, by fid */
    uint64_t      nfiles;
    uint32_t      tag_hi;        /* the tags below it are the book's */
    UseText*      use;
    BinderGround* ground;        /* by first block: what DIGESTS names */
    uint64_t      nground;
    const BinderRot* rot;        /* by tree, then key */
    uint32_t      nrot;
    uint64_t      a;             /* the first blocks of the runs the copies go in */
    uint64_t      b;
    uint64_t      room;          /* the blocks each run holds */
    uint32_t      grid;          /* the atom, in blocks */
    bool          measuring;     /* nothing written: the blocks counted */
    uint64_t      off;           /* the next node's place in the runs */
    uint32_t      tree;          /* the tree being laid, and its level */
    uint32_t      level;
    uint32_t      kid_at;        /* level >= 1: the next node of the level below */
    uint32_t      rot_at;        /* ... and the next rotten node */
    BinderItems   items;
    BinderFill    fill;
    BinderLevel   lv[BINDER_LEVELS_MOST];
    BinderRoot    root[TALLY_SHARED + 1u];
    uint8_t*      unit;          /* a node sealed, for the medium */
    uint64_t      unit_room;
    uint64_t      nodes;         /* laid */
    uint64_t      dropped;       /* DIGESTS elements inside a rotten range, left out */
    bool          trees_done;
    BinderKid*    dleaves;       /* the DIGESTS leaves laid, for the reads to turn to (binder.c) */
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
 * written — or its slot torn, `sectors` of its eight written. 0 for neither. */
void        BinderStandStopAt(TallyStep at);
void        BinderStandTearSlot(uint32_t sectors);

/* What the Binder holds, for the stand: the slot it would write next, the
 * useful bytes past the newest slot's `at`, what the next Tally is reckoned to
 * cost in bytes, the Quires of the ribbon the slots hold, the blocks the last
 * Tally took in one copy. */
typedef struct {
    uint32_t next_slot;
    uint64_t useful;
    uint64_t cost;
    uint64_t ribbon_quires;
    uint64_t tally_blocks;
    uint64_t tallies;        /* written by this mount */
    bool     owed;
} BinderStandSaid;
void        BinderStand(BinderStandSaid* out);

/* Every run of ground the slots hold now — their Tallies' nodes and their
 * ribbons' Quires — into `out` (room `room`): how many there are. What a
 * test that writes Tallies keeps as it found it, for they may give it back. */
uint32_t    BinderStandGround(Strip* out, uint32_t room);

/* Every Quire the ribbon was given since BinderStandGivenBegin, across the
 * mounts between — what a test that runs the ribbon through Quires puts back
 * to zeros, so no stitch of it is found past the end of the ribbon it found:
 * into `out` (room `room`), how many. */
void        BinderStandGivenBegin(void);
uint32_t    BinderStandGiven(RibbonQuire* out, uint32_t room);
#endif

#endif /* TAGFS_SETTLE_BINDER_H */
