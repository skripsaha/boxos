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
 * a Born of `now`, a Labeled of what differs between them, a Quitclaim of
 * `was`. So the changes a file has between two stitches merge by keeping the
 * newest `now` (§3.7, the design §2 п.22): a Born and a rename after it are one
 * Born of the last name, two renames one Labeled of the last, a change and a
 * Quitclaim one Quitclaim of what settled — and a file born and given up
 * before a stitch named it no record at all (L-8). A rename and its undoing
 * leave the two the same, and the stitch carries nothing for the file.
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

/* By the Settle's holder with no lock, once the stitch of seq `seq` built of
 * *t settled: each file's record as the stitch leaves it given that seq — or,
 * the stitch carrying nothing for it, the seq its base had — and the Jots let
 * go. *t is emptied. */
void  DocketSettled(DocketTaken* t, uint64_t seq);

/* By the Settle's holder with no lock, as the stitch is built of *t: the
 * records its Jots become at the most, the bytes they take with their tags
 * and lengths, the longest body, and the most tags one Jot's two records wear
 * between them (DocketMeasure); and those records laid into `out` from *at by
 * fid — a Born of the file as it stands, a Labeled of what differs from what
 * settled, a Quitclaim of what settled, nothing for a Jot whose two records
 * are the same — each one's offset into starts[*n], *n counted on, each Jot
 * told whether the stitch carries it (DocketLay). ERR_NO_MEMORY for the room
 * its tags are put in order in. */
void    DocketMeasure(const DocketTaken* t, uint64_t* records, uint64_t* bytes, uint64_t* body, uint32_t* tags);
error_t DocketLay(DocketTaken* t, uint64_t seq, uint32_t tags, uint8_t* out, uint32_t* at, uint8_t* body,
                  uint32_t* starts, uint32_t* n);

/* The bytes a change of `kind` from `was` to `now` claims on the ribbon before
 * it is made (P21, settle/sewing.h RibbonClaim): its record at the most. With
 * no lock: the records are the caller's. */
uint64_t DocketClaimOf(JotKind kind, const FileRecord* was, const FileRecord* now);

/* The kind a Jot of `older` becomes with a change of `newer` after it. */
JotKind JotMerged(JotKind older, JotKind newer);

/* The volume let go of, or mounted afresh: every Jot let go. With the door shut. */
void  DocketDropAll(void);

#endif /* TAGFS_INK_DOCKET_H */
