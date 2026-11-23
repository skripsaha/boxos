#ifndef TAGFS_SETTLE_ENDPAPER_FACT_H
#define TAGFS_SETTLE_ENDPAPER_FACT_H

#include "../ink/docket.h"
#include "../tag_spelling/tag_spelling.h"

/* What the fields of a Tally take at their widest, by the rules every writer
 * of one keeps (§2.9, §16.2 п.24) — the measures from above (endpaper.c) and
 * by the last Tally (endpaper_fact.c) share them. */
#define TALLY_NODE_HEAD_MOST    44u  /* NodeHead: tag, length, tree, level, items, a pair key — each at its widest */
#define TALLY_ITEM_HEAD_MOST    25u  /* an item's length (5) and a pair key (10 + 10) */
#define TALLY_RUN_MOST          25u  /* a run of the Tally's form: head 5, start 10, birth_delta 10 (§2.9) */
#define TALLY_PARCEL_HEAD_MOST   5u  /* a Parcel's count of runs */
#define TALLY_DIGEST_HEAD_MOST   5u  /* an element's count of digests */
#define TALLY_INNER_ITEM_MOST   72u  /* an inner item: its head, a count, a BookPtr of two copies */
#define TALLY_LEAF_ROOM         (TALLY_SHEET_PAYLOAD - TALLY_NODE_HEAD_MOST)
#define TALLY_TAG_BODY_MOST     20u  /* a tag's flags, key length and value length */

/* A tag's item longer than a leaf holds: more text than this, and every one
 * of them alone in a node of at most TALLY_LONG_TAG_SHEETS Sheets. */
#define TALLY_LONG_TAG_TEXT   (TALLY_LEAF_ROOM - TALLY_ITEM_HEAD_MOST - TALLY_TAG_BODY_MOST)
#define TALLY_LONG_TAG_SHEETS ((TALLY_NODE_HEAD_MOST + TALLY_ITEM_HEAD_MOST + TALLY_TAG_BODY_MOST + TAG_TEXT_WIDTH + \
                                TALLY_SHEET_PAYLOAD - 1u) / TALLY_SHEET_PAYLOAD)

/* The blocks the inner levels over `kids` children take, `atom` blocks a
 * node, each level's items at their widest and every two nodes of a level
 * more than a leaf's room — until one node, the root. */
uint64_t EndpaperInnerBlocks(uint64_t kids, uint64_t atom);

/*
 * The measure of the next Tally by what the last one took (endpaper_fact.c):
 * the leaves a Tally laid, counted to the block as its writer lays them, and
 * the growth of what changed since — so the reserve keeps what the next Tally
 * can take, not what a whole Tally measured from above would.
 *
 * What a Tally laid of FILES, TAGS and DIGESTS — the trees that grow with the
 * files, their tags and their data — counted by the one rule of both its
 * readers: the binder as it lays each leaf (binder_nodes.c), the mount as it
 * walks the Tally it stands on (book_ground.c, tally_leaves.c). `*_leaves`
 * counts each node of level 0 and `*_blocks` their blocks, each on its atom;
 * `rot` the trees with a rotten node named as it lies above level 0, whose
 * leaves nobody knows and which the measure takes from above (a node a Tally
 * laid in names because nothing under it changed counts its leaves as
 * Foliation holds them, binder_kept.c); `runs` the runs FILES's Parcels carry and `run_leaves` the leaves of
 * FILES they lie in — only those widen with the next Tally's seq; `digests_last` the first key
 * of DIGESTS's last leaf; `tags` and `tags_text` the tags TAGS holds and the
 * bytes of their keys and values.
 */
typedef struct {
    uint64_t files_leaves;
    uint64_t files_blocks;
    uint64_t runs;
    uint64_t run_leaves;
    uint64_t digests_leaves;
    uint64_t digests_blocks;
    uint64_t digests_last;
    uint64_t tags_leaves;
    uint64_t tags_blocks;
    uint64_t tags;
    uint64_t tags_text;
    uint32_t rot;
} EndpaperLaid;

/* A node of `tree` at `level`, `sheets` long on an atom of `grid` blocks, its
 * first key `first`, into *l — a leaf, child and blocks; a node named as it
 * lies, `sheets` 0, a leaf on no new ground at level 0 and its tree's `rot`
 * above it; an inner node laid, nothing. A tag of `text` bytes TAGS holds,
 * into *l. */
void     EndpaperLaidNode(EndpaperLaid* l, uint64_t tree, uint64_t level, uint64_t sheets, uint64_t grid,
                          uint64_t first);
void     EndpaperLaidTag(EndpaperLaid* l, uint64_t text);

/*
 * The Settle's holder — or the mount, with the door shut — tells what moved:
 *   EndpaperTallyBegun  a Tally of seq `seq`, past every file id below
 *                       `next_fid`, took its slice: what settles from here is
 *                       growth since it too
 *   EndpaperTallyLaid   its DONE — what it laid, the measure from here; NULL:
 *                       ended without it
 *   EndpaperGrew        the stitch `seq` settled with the Jots `t`, before
 *                       DocketSettled: their files' items and their data's
 *                       digests are growth, and what the stitch owed is let go
 *   EndpaperStood       the mount stood on the Tally `seq` that laid *l, past
 *                       every file id below `next_fid`: what the stitches it
 *                       replayed changed is growth since it
 *   EndpaperForget      no Tally known: the measure from above alone
 * With no TagFS lock held: TallyLaid and Grew take the state lock to change
 * what a reader takes.
 */
void     EndpaperTallyBegun(TagFSVolume* vol, uint64_t seq, uint64_t next_fid);
void     EndpaperTallyLaid(TagFSVolume* vol, const EndpaperLaid* l);
void     EndpaperGrew(TagFSVolume* vol, const DocketTaken* t, uint64_t seq);
void     EndpaperStood(TagFSVolume* vol, const EndpaperLaid* l, uint64_t seq, uint64_t next_fid);
void     EndpaperForget(TagFSVolume* vol);

/*
 * What Ink laid in memory and no stitch settled yet — the Jots of the Docket
 * and of the stitch in flight, and the data their Inks put in — counted in the
 * measure as it is laid, so the measure holds every change memory holds, as
 * the measure from above, which walks the map, does. Under the state lock, as
 * the Docket moves (docket.c, ink_book.c):
 *   EndpaperJotIn/Out    Jot `j` of the Docket stands for its change, or
 *                        stands for it no more (merged, given back under
 *                        another)
 *   EndpaperInked        an Ink of data put in the Strips its splices insert
 *   EndpaperDocketTaken  TAKE: what the Docket held is the stitch's in flight
 *   EndpaperDocketBack   a refused stitch: its Jots the Docket's again
 *   EndpaperDocketDropped every Jot let go
 * The stitch in flight is counted until EndpaperGrew counts it as settled.
 */
void     EndpaperJotIn(TagFSVolume* vol, const Jot* j);
void     EndpaperJotOut(TagFSVolume* vol, const Jot* j);
void     EndpaperInked(TagFSVolume* vol, const BookSplice* sp, uint64_t nsp);
void     EndpaperDocketTaken(TagFSVolume* vol);
void     EndpaperDocketBack(TagFSVolume* vol);
void     EndpaperDocketDropped(TagFSVolume* vol);

/* What FILES, DIGESTS and TAGS of the next Tally take in one copy, by the
 * last Tally and the growth since, every change memory holds among it:
 * UINT64_MAX each when no Tally is known, or a node of the tree was named as
 * it lies. From any core with no TagFS lock held: the state lock is taken to
 * read what moved. */
void     EndpaperFactMost(TagFSVolume* vol, uint64_t* files, uint64_t* digests, uint64_t* tags);

/* Whether the measure carries growth memory holds past the last Tally —
 * settled since it or owed, or no Tally known and every item measured at its
 * widest — what the next Tally lays and measures to the block (binder.h,
 * BinderRoomWait); a Plot's Stakes are no such growth, no Tally lays them.
 * From any core, lock-free: published wherever it changes. */
bool     EndpaperGrown(TagFSVolume* vol);

/*
 * What a Plot's Ink may add to the next Tally while its Stakes stand
 * (endpaper_fact.c, §16.6 п.8): by the last Tally, its file a place of FILES
 * with its items whole, and each window its runs lie in a place of DIGESTS;
 * from above, the bytes its runs add to FILES and DIGESTS. `epoch` is the
 * mounting it is counted in, 0 for none.
 *   EndpaperStakesFile  begins *e: file `fid` as `rec` holds it, `runs` runs
 *                       and `blocks` blocks at the most once inked
 *   EndpaperStakesRun   a run of `count` blocks from `start` into *e —
 *                       UINT64_MAX for one not taken yet, anywhere
 *   EndpaperStakesSet   what the Plot counts, *held, made *now — NULL for
 *                       none — in the measure: no TagFS lock held
 *   EndpaperStakesGoneLocked  *held counted no more, under the state lock —
 *                       the Ink that names its Stakes, which counts what it
 *                       put in (EndpaperJotIn, EndpaperInked); its Plot
 *                       forgets *held once the Ink stands
 * A Plot's of another mounting is no count of this one's (EndpaperForget
 * began it at none).
 */
typedef struct {
    uint32_t epoch;
    uint64_t sites;
    uint64_t bytes;
    uint64_t long_blocks;
    uint64_t dsites;
    uint64_t dbytes;
    uint64_t files_bytes;
    uint64_t digests_bytes;
} EndpaperStakes;

void     EndpaperStakesFile(TagFSVolume* vol, EndpaperStakes* e, const FileRecord* rec, uint64_t fid, uint64_t runs, uint64_t blocks);
void     EndpaperStakesRun(EndpaperStakes* e, uint64_t start, uint64_t count);
void     EndpaperStakesSet(TagFSVolume* vol, EndpaperStakes* held, const EndpaperStakes* now);
void     EndpaperStakesGoneLocked(TagFSVolume* vol, const EndpaperStakes* held);

/* What the Plots' Stakes add to the measure from above, in bytes of FILES and
 * of DIGESTS: read before the map is walked, so an Ink between the two is
 * counted twice, never missed. Any core, lock-free. */
void     EndpaperStakesAbove(TagFSVolume* vol, uint64_t* files_bytes, uint64_t* digests_bytes);

/* The growth since a Tally of seq `seq`, or what is owed. */
typedef struct {
    bool     on;
    uint64_t seq;          /* the Tally's: a file whose gen is past it changed since */
    uint64_t fid;          /* past every file id it held */
    uint64_t sites;        /* FILES: the places files it held changed at ... */
    uint64_t bytes;        /* ... and their items, at the widest */
    uint64_t tail;         /* FILES: the items of the files past `fid`, at the widest ... */
    uint64_t tail_big;     /* ... and those longer than ENDPAPER_SMALL */
    uint64_t long_blocks;  /* FILES: the nodes of items longer than a leaf's room */
    uint64_t dsites;       /* DIGESTS: windows run into before its last leaf's first key ... */
    uint64_t dtail;        /* ... at or past it ... */
    uint64_t dbytes;       /* ... and their elements, at the widest */
} EndpaperSince;

/* What one volume holds of endpaper_fact.c (TagFSVolume, volume/tagfs_volume.h). */
typedef struct {
    EndpaperLaid laid;  /* the Tally `endpaper_fact.since` counts from */
    EndpaperSince since;  /* settled since the last Tally known */
    EndpaperSince pending;  /* settled since the slice of the Tally under way */
    EndpaperSince inked;  /* owed: the Docket's Jots and the data inked for them */
    EndpaperSince sewing;  /* owed: the stitch in flight's */
    EndpaperSince staked;  /* what the Plots' Stakes may add once inked */
    uint32_t staked_epoch;  /* the mounting `endpaper_fact.staked` counts Plots of */
    volatile uint64_t staked_files;  /* ... from above, in bytes of FILES */
    volatile uint64_t staked_digests;  /* ... and of DIGESTS */
    volatile uint8_t grown;  /* whether growth past the last Tally is held: published */
} EndpaperFactState;

void EndpaperFactVolumeBegin(TagFSVolume* vol);

#endif /* TAGFS_SETTLE_ENDPAPER_FACT_H */
