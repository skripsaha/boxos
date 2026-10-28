#ifndef TAGFS_INK_DOCKET_H
#define TAGFS_INK_DOCKET_H

#include "../tagfs.h"
#include "../file_map/file_record.h"

/*
 * The Docket — what Ink keeps for the next stitch of a format-v2 volume (spec_v2
 * §3.7): one Jot a file, the change the next stitch carries for it, by fid.
 *
 * ‼ A JOT IS TWO RECORDS, NOT A LIST OF EDITS. It holds the file as the last
 * stitch naming it left it (`was`) and as its newest change leaves it (`now`);
 * the record the stitch carries is read off the two when the stitch is built —
 * a Born of `now`, its runs and their digests with it; a Shaped of how the
 * runs, the size, the time and the tail of `was` became those of `now`,
 * spliced (FileRecordSplices) — the tail as the one span of its last block a
 * reader of `was` holds otherwise (TailSlipDelta) — and a Labeled of what
 * differs in its name, flags and
 * tags; a Quitclaim of `was`. So the changes a file has between two stitches
 * merge by keeping the newest `now` (§3.7, the design §2 п.22): a Born and a
 * rename or a write after it are one Born of the last state, two renames one
 * Labeled of the last, writes one after another one Shaped — the appends one
 * run where they went on one another (ink_book.c, InkBookPlotJoin), and of an
 * old run only what the writes changed (FileRecordSplices) — a change and a
 * Quitclaim one Quitclaim of what settled, and a file born and given up before
 * a stitch named it no record at all (L-8). A rename and its undoing leave the two the same, and
 * the stitch carries nothing for the file. The digests of the blocks a
 * stitch's records put in are the Gloss's, put there by the Ink that wrote
 * them (integrity/gloss.h).
 *
 * ‼ THE SEQ IS THE STITCH'S, THE GEN THE INK'S (§16.2 п.14). A Jot is made by an
 * Ink, whose number — RecordWhere.gen — orders it against the waiters and the
 * Helm's views; a record of the ribbon names the file's base by the seq of the
 * stitch that last changed it (FileRecord.gen), which no Ink knows: the base is
 * `was`'s, and `now`'s is given when its stitch settles (DocketSettled) — or
 * left `was`'s, when the stitch carried nothing for the file.
 *
 * ‼ UNDER THE STATE LOCK, NOTHING ASKED OF THE HEAP. A Jot is made before the
 * lock, with the record references it holds (JotMake); under the lock it is
 * merged with the file's Jot waiting there or put in the Docket — a list by fid
 * whose levels a Jot carries in itself — and a Jot merged away, with the records
 * it no longer needs, is handed back to be let go after the lock (`spent`).
 * TAKE takes the whole Docket in the same hold of the lock as the target; a
 * cycle refused gives what it took back, merged under what came since, and
 * what settled is never touched.
 */

typedef enum {
    JOT_NONE = 0,       /* born and given up before a stitch named it: no record (L-8) */
    JOT_BORN,
    JOT_LABELED,
    JOT_QUITCLAIM,
    JOT_SHAPED,         /* its data changed — a Labeled's change with it, when its fields did too */
} JotKind;

/* The levels of the Docket's list a Jot may stand in: a quarter of the Jots
 * of a level stand in the next, so sixteen hold four billion files with no
 * more than a few steps a level. */
#define JOT_LEVELS 16u

typedef struct Jot Jot;
struct Jot {
    uint64_t    fid;
    JotKind     kind;
    uint32_t    levels;
    bool        carried;    /* the stitch built of it lays a record of the file */
    FileRecord* was;        /* held: the file as the last stitch naming it left it; NULL for a Born */
    FileRecord* now;        /* held: the file as its newest change leaves it; NULL for a Quitclaim */
    BookSplice* splices;    /* the stitch's BUILD: how `was`'s runs became `now`'s, measured once */
    uint64_t    nsplices;
    Strip*      inserted;   /* ... and the Strips they put in */
    uint32_t    slip_off;   /* ... and the Slip its record lays: the span of the last block changed since */
    uint32_t    slip_len;   /*     `was` (TailSlipDelta), 0 for none */
    Jot*        next[];     /* the Docket's list, `levels` of them */
};

/* What TAKE took: the Jots of the stitch, by fid, linked through next[0]. */
typedef struct {
    Jot*     first;
    uint64_t count;
} DocketTaken;

/* A Jot of `kind` for file `fid`, with no lock held: it takes the caller's
 * references to `was` and `now`. NULL when there is no memory — and then the
 * references are let go. */
Jot*  JotMake(JotKind kind, uint64_t fid, FileRecord* was, FileRecord* now);

/* Jots linked through next[0] let go, and the records they hold: with no lock. */
void  JotsLetGo(Jot* list);

/* Under the state lock: `j` put in the Docket — merged into the file's Jot
 * waiting there, when there is one, and then `j` is spent — and the Jots spent
 * linked onto *spent for JotsLetGo after the lock. */
void  DocketPutLocked(Jot* j, Jot** spent);

/* Under the state lock, with the target of the cycle: the Docket taken whole
 * into *out, and an empty one begun. */
void  DocketTakeLocked(DocketTaken* out);

/* Under the state lock: what a refused cycle took, given back — each Jot merged
 * under the file's Jot the Docket gained since, or put back as it was. *t is
 * emptied; the Jots spent linked onto *spent. */
void  DocketBackLocked(DocketTaken* t, Jot** spent);

/* Under the state lock: whether `block`, at the file's block k, is ground
 * written since the record the next stitch lays the file's change against —
 * the `was` of the file's Jot waiting in the Docket, none for a file born since
 * the last stitch, and the map's record itself while the Docket holds no Jot of
 * the file. Such ground no stitch named, and a run of it may be grown: the
 * stitch puts it in whole, the appends one run (§3.7). The answer holds while
 * DocketTakes says the same — a TAKE makes what the Docket held the stitch's. */
bool     DocketBaseLacks(uint64_t fid, uint64_t k, uint64_t block);
uint64_t DocketTakes(void);

/* Under the state lock: the first Jot at or past `fid` — its fid in *at, and
 * in *was the file as the last stitch that named it left it, with a reference
 * the caller lets go (NULL for a file born since: no settled stitch names
 * it). False when there is none. Between two cycles this and the map's record
 * of every other file are the state after the last stitch that settled — what
 * a whole Tally is written of (settle/binder_slice.c). */
bool  DocketWasFrom(uint64_t fid, uint64_t* at, FileRecord** was);

/* By the Settle's holder with no lock, at the S6 of stitch `seq` built of *t,
 * before DocketSettled: the ground its records put in named as born at `seq`
 * (settle/births.c) — every run of a Born, the runs a Shaped put in — and the
 * highest file id it named raised into *fid_most. ERR_NO_MEMORY. */
error_t DocketBirths(const DocketTaken* t, uint64_t seq, uint64_t* fid_most);

/* By the Settle's holder with no lock, once the stitch of seq `seq` built of
 * *t settled: each file's record as the stitch leaves it given that seq — or,
 * the stitch carrying nothing for it, the seq its base had — and the Jots let
 * go. *t is emptied. */
void  DocketSettled(DocketTaken* t, uint64_t seq);

/* By the Settle's holder with no lock, as the stitch is built of *t: the
 * records its Jots become at the most, the bytes they take with their tags
 * and lengths, the longest body, the most tags one Jot's two records wear
 * between them, and the runs of fresh data they will name and its blocks —
 * what a mount's check of the stitch reads (StitchToll) — each changed
 * file's splices found on the way (DocketMeasure, ERR_NO_MEMORY for no room
 * for them); and those records laid into `out` from
 * *at by fid — a Born of the file as it stands, its runs and their digests; a
 * Shaped of how its data changed since what settled — the Slip of each the
 * span of its last block changed since (TailSlipDelta) — then a Labeled of what
 * else differs; a Quitclaim of what settled; nothing for a Jot whose two
 * records are the same — each one's offset into starts[*n], *n counted on,
 * the blocks of fresh data they name added to *fresh (Clasp.fresh), each Jot
 * told whether the stitch carries it (DocketLay). ERR_NO_MEMORY for the room
 * its tags are put in order in, and the runs and digests laid apart first. */
error_t DocketMeasure(DocketTaken* t, uint64_t* records, uint64_t* bytes, uint64_t* body, uint32_t* tags,
                      uint64_t* runs, uint64_t* fresh);
error_t DocketLay(DocketTaken* t, uint64_t seq, uint32_t tags, uint8_t* out, uint32_t* at, uint8_t* body,
                  uint32_t* starts, uint32_t* n, uint64_t* fresh);

/* The bytes a change of `kind` from `was` to `now` claims on the ribbon before
 * it is made (P21, settle/sewing.h RibbonClaim): its record at the most. With
 * no lock: the records are the caller's. */
uint64_t DocketClaimOf(JotKind kind, const FileRecord* was, const FileRecord* now);

/* The bytes a change of a file's data claims (P21): a Shaped of `nsplice`
 * splices putting in `runs` Strips and `fresh` blocks of fresh data, each
 * splice counted with two Strips more — the two a later change may cut out of
 * one, which the merged record then carries — and a Slip of `slip` bytes, the
 * span the tail it leaves covers (0 for none: the empty Slip's two bytes are
 * every record's) — so the claims of the changes a Jot merges hold the one
 * record it lays. */
uint64_t DocketClaimOfShaped(uint64_t runs, uint64_t nsplice, uint64_t fresh, uint64_t slip);

/* The kind a Jot of `older` becomes with a change of `newer` after it. */
JotKind JotMerged(JotKind older, JotKind newer);

/* The volume let go of, or mounted afresh: every Jot let go. With the door shut. */
void  DocketDropAll(void);

#endif /* TAGFS_INK_DOCKET_H */
