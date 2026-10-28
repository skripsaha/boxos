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
 * The measure of the next Tally by what the last one took (endpaper_fact.c,
 * spec_v2 §16.4 п.7): the leaves a Tally laid, counted to the block as its
 * writer lays them, and the growth of what changed since — so the reserve
 * keeps what the next Tally can take, not what a whole Tally measured from
 * above would.
 *
 * What a Tally laid of FILES, TAGS and DIGESTS — the trees that grow with the
 * files, their tags and their data — counted by the one rule of both its
 * readers: the binder as it lays each leaf (binder_nodes.c), the mount as it
 * walks the Tally it stands on (book_ground.c, tally_leaves.c). `*_leaves`
 * counts each node of level 0 and `*_blocks` their blocks, each on its atom;
 * `rot` the trees with a node named as it lies above level 0, which the
 * measure takes from above; `runs` the runs FILES's Parcels carry; `digests_last` the first key
 * of DIGESTS's last leaf; `tags` and `tags_text` the tags TAGS holds and the
 * bytes of their keys and values.
 */
typedef struct {
    uint64_t files_leaves;
    uint64_t files_blocks;
    uint64_t runs;
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
void     EndpaperTallyBegun(uint64_t seq, uint64_t next_fid);
void     EndpaperTallyLaid(const EndpaperLaid* l);
void     EndpaperGrew(const DocketTaken* t, uint64_t seq);
void     EndpaperStood(const EndpaperLaid* l, uint64_t seq, uint64_t next_fid);
void     EndpaperForget(void);

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
void     EndpaperJotIn(const Jot* j);
void     EndpaperJotOut(const Jot* j);
void     EndpaperInked(const BookSplice* sp, uint64_t nsp);
void     EndpaperDocketTaken(void);
void     EndpaperDocketBack(void);
void     EndpaperDocketDropped(void);

/* What FILES, DIGESTS and TAGS of the next Tally take in one copy, by the
 * last Tally and the growth since, every change memory holds among it:
 * UINT64_MAX each when no Tally is known, or a node of the tree was named as
 * it lies. From any core with no TagFS lock held: the state lock is taken to
 * read what moved. */
void     EndpaperFactMost(uint64_t* files, uint64_t* digests, uint64_t* tags);

#endif /* TAGFS_SETTLE_ENDPAPER_FACT_H */
