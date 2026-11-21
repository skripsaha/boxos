#include "endpaper_fact.h"
#include "sewing.h"
#include "../file_map/file_map.h"
#include "../mount/book_mount.h"
#include "../mount/book_unit.h"
#include "../tag_registry/tag_registry.h"

/*
 * ‼ THE NEXT TALLY BY WHAT THE LAST ONE TOOK. A measure from above of a whole
 * Tally (endpaper.c) takes every item at its widest and every pair of leaves
 * at one room — six times what a Tally of empty files lays — and the reserve
 * keeps it once for the Tally and again for each slot of the ring that holds
 * less (BinderRingShort): metadata runs into the reserve long before the
 * ground runs out. So FILES, TAGS and DIGESTS — the trees that grow with the
 * files, their tags and their data — are measured by the last Tally, counted
 * to the block as it was laid (EndpaperLaid), and what changed since, from
 * above; USE, one item, stays measured as it stands. Each tree's measure is
 * the lesser of the two: both are bounds from above.
 *
 * ‼ WHY IT IS NEVER LESS THAN THE NEXT TALLY LAYS. Not because the leaves of
 * a tree change only where its items do: the packing is greedy (binder_nodes.c,
 * FillTakes) and a byte more in a full leaf moves an item of every leaf after
 * it; a Parcel left as it was grows, its birth_delta spelled from the seq of
 * the Tally that lays it; an item's key is written from the one before it in
 * its node, the first one's from the node's head. The bound stands on these,
 * C the room of a leaf (TALLY_LEAF_ROOM — a Sheet less a NodeHead at its
 * widest) and R = C − TALLY_KEY_MOST:
 *   1. Greedy is the least: of the cuts of a tree's items in key order into
 *      nodes that fit — each closed before a node named as it lies, as the
 *      writer closes one (FillBarred) — none has fewer nodes than the one the
 *      writer lays, for a part of a node that fits fits (taking an item out
 *      never lengthens the rest, below): any cut that fits bounds the next
 *      Tally's leaves.
 *   2. Taking an item out lengthens nothing: the key of the item after it,
 *      written from the one before it, is at most the two keys it replaces
 *      (TallyKeyBytes is subadditive along the order: k1 and k2 distances add,
 *      a varint of a sum is at most both varints), its length's varint at most
 *      the one let go; one first in its node leaves a NodeHead key no wider
 *      than its own key and the item after it shrinks to two bytes.
 *   3. Putting an item in at its widest — its key no wider than its numbers
 *      whole, its length's varint counted — never lengthens the item after it
 *      (its key from nearer), but for one put before a node's first item, and
 *      that is a key at its widest more: TALLY_KEY_MOST counted with each
 *      place an item went in.
 *   4. The cut: each leaf of the last Tally is a piece, an item put in going
 *      to the piece it lies in or the one before it. A piece nothing went in
 *      or out of is the leaf it was. A piece held at most a Sheet of items as
 *      they were laid — its writer fills a leaf to a Sheet less the NodeHead
 *      it writes, narrower than its widest. Pieces one after the other cut as
 *      one run, greedily: of two of its nodes one after the other, the first
 *      was closed because the item that begins the second did not fit — an
 *      item the second writes from its head, at most TALLY_KEY_MOST shorter —
 *      so the two hold more than R. A run of k pieces g bytes went in among is
 *      at most k Sheets and g bytes, cut into at most 2(k·Sheet + g)/R + 1
 *      nodes — that many more, less k, than it took (Recut). Of FILES, the
 *      files born past every id the last Tally held (`fid`) come after every
 *      key of it — a piece of their own, at their widest, cut into at most
 *      G/(C − ENDPAPER_SMALL) + B + 1 nodes, B its items longer than
 *      ENDPAPER_SMALL (a node is closed by a short item only past
 *      C − ENDPAPER_SMALL bytes), and never more than 2G/C + 1.
 *   5. FILES: a file changed since is taken out whole and put in whole, at its
 *      widest — all its items lie together, so it is one place, a run of one
 *      piece; one changed again is counted again in place of what it was
 *      counted at (its `was`, whose gen is past the Tally's). A run of a
 *      Parcel nobody changed widens by the varint of the seqs between the two
 *      Tallies, its Parcel's length by a byte: every piece a run lies in is
 *      counted as one something went in (`run_leaves`). Nothing else of an
 *      item moves with the seq — a file's header is spelled from its own
 *      numbers, a key from the one before it — so a piece of no run and no
 *      change is the leaf it was: a Tally of empty files is measured at the
 *      leaves it laid, not at every leaf recut, three times them.
 *   6. DIGESTS: an element is the run of digested blocks of one window; a
 *      block given up shortens or splits one (the split's head is less than
 *      the digest it lost); a block put in lengthens, begins or joins them —
 *      joined across two leaves, the two pieces are cut as one: each window a
 *      run put in lies in is a place, a run of two pieces, 8 bytes a block and
 *      an element at its widest; at or past the last leaf's first key the
 *      places are one, the last piece.
 *   7. TAGS: a tag is never given back and its item never changes (the
 *      registry only grows, tag_registry.c; the system bit it marks is not
 *      laid): the tags the registry holds past the ones the Tally laid have
 *      ids past every id it holds — a piece of their own, at their widest.
 *   8. A place counts no more than the pieces there are; a node of several
 *      Sheets takes its Sheets on its atom besides; the inner levels are
 *      measured afresh at the widest from the leaves' count (every BookPtr is
 *      new). A node named as it lies at level 0 is a leaf of the next Tally
 *      as it was, on no new ground — a piece the cut closes before. A tree
 *      with one above level 0 is measured from above: such a node is a child
 *      of a level the count of the inner levels does not take it at.
 * So the next Tally's trees take no more than this measure.
 *
 * ‼ EVERY CHANGE MEMORY HOLDS, NOT ONLY WHAT SETTLED. An Ink lays its record in
 * the map and its Jot in the Docket at once (ink_book.c); the stitch that
 * carries it settles later, and a Tally sliced after that holds it. So the
 * measure counts, beside what settled since the Tally, what is owed: each Jot
 * of the Docket and of the stitch in flight as the most its change adds
 * whatever Tally it is counted against — a file born since a stitch last
 * settled has an id past every id of a Tally sliced while it waits (its id is
 * given under the state lock with its Jot, InkBookNewLocked, and a TAKE takes
 * the whole Docket), its items in the tail; any other file a place of its own,
 * its items whole — and each window of data an Ink put in as a place. A Jot
 * merged is counted again as it now stands; a stitch's owed is let go in the
 * same hold of the state lock as what it carried is counted as settled
 * (EndpaperGrew). The measure from above walks the map, so it holds the same
 * changes: the reserve kept by the lesser of the two is what "a ribbon's step
 * and a whole Tally are always possible" (§16.2 п.6) needs of the changes
 * memory holds — and a block handed to a write and not yet inked in a record
 * is counted in both by the Plot that holds it, as its Ink will count it
 * (below, EndpaperStakesSet).
 *
 * What settled since is counted as the stitches settle (EndpaperGrew), from
 * the Jots each stitch carried — no walk of the volume; at the mount, the
 * files the stitches it replayed past the Tally it stood on changed (their gen
 * past the Tally's), in one walk of memory as the mount ends. While a Tally is
 * being written the stitches between its slice and its DONE are counted for it
 * as well (`pending`), and become the growth since the Tally once it is done.
 * The Settle's holder alone changes the Tally and what settled, as the mount
 * does with the door shut, Ink what is owed and a Plot what its Stakes may
 * add, all under the state lock; a reader takes them in one hold of it
 * (EndpaperFactMost).
 */

#define ENDPAPER_SMALL     (TALLY_LEAF_ROOM / 16u)
#define TALLY_KEY_MOST     20u      /* a pair key, k1 and k2 at their widest */
#define ENDPAPER_RECUT_ROOM (TALLY_LEAF_ROOM - TALLY_KEY_MOST)
#define ENDPAPER_HUGE      (1ull << 40)   /* counts past this are not counts: no measure by the last Tally */
#define ENDPAPER_TAKES_OUT UINT64_MAX     /* the factor of a count taken out: minus one, modulo 2^64 */

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

static EndpaperLaid  s_laid;                       /* the Tally `s_since` counts from */
static EndpaperSince s_since;                      /* settled since the last Tally known */
static EndpaperSince s_pending;                    /* settled since the slice of the Tally under way */
static EndpaperSince s_inked   = { .on = true };   /* owed: the Docket's Jots and the data inked for them */
static EndpaperSince s_sewing  = { .on = true };   /* owed: the stitch in flight's */
static EndpaperSince s_staked  = { .on = true };   /* what the Plots' Stakes may add once inked */
static uint32_t      s_staked_epoch;               /* the mounting `s_staked` counts Plots of */
static volatile uint64_t s_staked_files;           /* ... from above, in bytes of FILES */
static volatile uint64_t s_staked_digests;         /* ... and of DIGESTS */
static volatile uint8_t  s_grown;                  /* whether growth past the last Tally is held: published */

static bool SinceGrown(const EndpaperSince* a)
{
    return a->sites || a->bytes || a->tail || a->long_blocks || a->dsites || a->dtail || a->dbytes;
}

/* Whether what settled since the last Tally or is owed holds growth a Tally
 * would lay to the block — or no Tally is known: published under the state
 * lock wherever either changes, read lock-free (EndpaperGrown). */
static void SinceSaid(void)
{
    bool grown = !s_since.on || SinceGrown(&s_since) || SinceGrown(&s_inked) || SinceGrown(&s_sewing);
    __atomic_store_n(&s_grown, grown ? 1u : 0u, __ATOMIC_RELEASE);
}

bool EndpaperGrown(void)
{
    return __atomic_load_n(&s_grown, __ATOMIC_ACQUIRE) != 0;
}

static uint64_t FactGrid(void)
{
    return g_book.atom > TAGFS_BLOCK_SIZE ? g_book.atom / TAGFS_BLOCK_SIZE : 1u;
}

void EndpaperLaidNode(EndpaperLaid* l, uint64_t tree, uint64_t level, uint64_t sheets, uint64_t grid,
                      uint64_t first)
{
    uint64_t *leaves = NULL, *blocks = NULL;
    if (tree == TALLY_FILES) {
        leaves = &l->files_leaves;
        blocks = &l->files_blocks;
    } else if (tree == TALLY_TAGS) {
        leaves = &l->tags_leaves;
        blocks = &l->tags_blocks;
    } else if (tree == TALLY_DIGESTS) {
        leaves = &l->digests_leaves;
        blocks = &l->digests_blocks;
    } else {
        return;
    }
    if (sheets == 0 && level != 0) {
        l->rot |= 1u << tree;
        return;
    }
    if (level != 0)
        return;
    *leaves += 1u;
    *blocks += (sheets + grid - 1u) / grid * grid;
    if (tree == TALLY_DIGESTS && first > l->digests_last)
        l->digests_last = first;
}

void EndpaperLaidTag(EndpaperLaid* l, uint64_t text)
{
    l->tags++;
    l->tags_text += text;
}

uint64_t EndpaperInnerBlocks(uint64_t kids, uint64_t atom)
{
    uint64_t blocks = 0;
    while (kids > 1u) {
        uint64_t bytes = kids * TALLY_INNER_ITEM_MOST;
        kids    = bytes <= TALLY_LEAF_ROOM ? 1u : 2u * ((bytes + TALLY_LEAF_ROOM - 1u) / TALLY_LEAF_ROOM);
        blocks += kids * atom;
    }
    return blocks;
}

// ----------------------------------------------------------------------------
// A file's items, at the widest
// ----------------------------------------------------------------------------

typedef struct {
    uint64_t bytes;
    uint64_t big;
    uint64_t long_blocks;
} EndpaperItems;

static void ItemsPut(EndpaperItems* it, uint64_t key, uint64_t body, uint64_t grid)
{
    uint64_t bytes = TallyVarintBytes(key + body) + key + body;
    it->bytes += bytes;
    it->big   += bytes > ENDPAPER_SMALL;
    if (bytes > TALLY_LEAF_ROOM) {
        uint64_t sheets = (TALLY_NODE_HEAD_MOST + bytes + TALLY_SHEET_PAYLOAD - 1u) / TALLY_SHEET_PAYLOAD;
        it->long_blocks += (sheets + grid - 1u) / grid * grid;
    }
}

/* Every item file `fid` lays as `rec` holds it, the header written with `gen`:
 * the header's body to the byte (FileHeadItem), its key at most its fid's
 * varint and part 0's; the Parcels — at most one a PARCEL_STRIPS_LEAST runs,
 * or one — each a key of its block, its count and its runs at the widest. */
static void FileItemsAs(const FileRecord* rec, uint64_t fid, uint64_t gen, uint64_t grid, uint64_t runs,
                        uint64_t blocks, EndpaperItems* it)
{
    memset(it, 0, sizeof(*it));
    if (!rec)
        return;
    uint64_t body = TallyVarintBytes(gen) + TallyVarintBytes(rec->flags) + TallyVarintBytes(rec->size) +
                    TallyVarintBytes(rec->created_time) +
                    TallyVarintBytes(BookZigzag((int64_t)(rec->modified_time - rec->created_time))) +
                    TallyVarintBytes(rec->name_len) + rec->name_len + TallyVarintBytes(rec->tag_count);
    for (uint32_t i = 0; i < rec->tag_count; i++)
        body += TallyVarintBytes(rec->tags[i]);
    if (rec->flags & TALLY_FILE_DELETED)
        body += TallyVarintBytes(rec->keep_until);
    if (rec->flags & TALLY_FILE_MADE_BY)
        body += TallyVarintBytes(rec->made_by);
    ItemsPut(it, TallyVarintBytes(fid) + 1u, body, grid);
    if (runs == 0)
        return;
    uint64_t parcels = (runs + PARCEL_STRIPS_LEAST - 1u) / PARCEL_STRIPS_LEAST;
    uint64_t key     = 1u + TallyVarintBytes(1u + blocks);
    uint64_t head    = key + TallyVarintBytes(PARCEL_STRIPS_MOST) + 2u;
    uint64_t least   = (ENDPAPER_SMALL - head) / TALLY_RUN_MOST + 1u;   /* the runs of a Parcel past SMALL */
    it->bytes += parcels * head + runs * TALLY_RUN_MOST;
    it->big   += runs / least < parcels ? runs / least : parcels;
}

static void FileItems(const FileRecord* rec, uint64_t fid, uint64_t gen, uint64_t grid, EndpaperItems* it)
{
    FileItemsAs(rec, fid, gen, grid, rec ? rec->strip_count : 0, rec ? rec->blocks : 0, it);
}

// ----------------------------------------------------------------------------
// What changed since
// ----------------------------------------------------------------------------

/* File `fid` changed from `was` (gen `was_gen`) to `now` (gen `now_gen`) —
 * NULL for none — into the growth of *a: what it was counted at, when it had
 * changed since already, put back. Counted exactly as added, so the
 * subtraction is exact; a count it were not would wrap past ENDPAPER_HUGE,
 * and the measure by the last Tally gives way to the one from above. */
static void SinceFile(EndpaperSince* a, uint64_t fid, const FileRecord* was, uint64_t was_gen,
                      const FileRecord* now, uint64_t now_gen, uint64_t grid)
{
    EndpaperItems in, out;
    bool          again = was && was_gen > a->seq;
    bool          tail  = fid >= a->fid;
    if (!a->on)
        return;
    FileItems(now, fid, now_gen, grid, &in);
    FileItems(again ? was : NULL, fid, was_gen, grid, &out);
    if (tail) {
        a->tail     = a->tail - out.bytes + in.bytes;
        a->tail_big = a->tail_big - out.big + in.big;
    } else {
        a->bytes = a->bytes - out.bytes + in.bytes;
        if (!again && now) {
            a->sites++;
            a->bytes += TALLY_KEY_MOST;
        }
    }
    a->long_blocks = a->long_blocks - out.long_blocks + in.long_blocks;
}

/* Blocks [start, start + count) given digests: each window they lie in a
 * place of DIGESTS — one before the leaf whose first key is `last`, or the
 * last piece. */
static void SinceGround(EndpaperSince* a, uint64_t start, uint64_t count, uint64_t last)
{
    while (a->on && count) {
        uint64_t end = (start / DIGEST_ITEM_MOST + 1u) * DIGEST_ITEM_MOST;
        uint64_t n   = end - start < count ? end - start : count;
        *(start >= last ? &a->dtail : &a->dsites) += 1u;
        a->dbytes += 8u * n + TALLY_ITEM_HEAD_MOST + TALLY_DIGEST_HEAD_MOST + TALLY_KEY_MOST;
        start     += n;
        count     -= n;
    }
}

static void SinceStrip(EndpaperSince* since, EndpaperSince* pending, const Strip* s)
{
    if (s->kind != STRIP_DATA)
        return;
    SinceGround(since, s->start, s->count, s_laid.digests_last);
    SinceGround(pending, s->start, s->count, UINT64_MAX);    /* its last leaf is not laid yet */
}

/* Jot `j`'s change into what is owed, `k` 1 to put it in, ENDPAPER_TAKES_OUT
 * to take it out — counted from its records alone, so taken out as it was put
 * in: its file's items whole, the gen at its widest (no stitch gave it yet); a
 * file born since a stitch last settled in the tail, any other a place. */
static void OwedJot(EndpaperSince* a, const Jot* j, uint64_t k)
{
    EndpaperItems it;
    if (!j->now)
        return;
    FileItems(j->now, j->fid, UINT64_MAX, FactGrid(), &it);
    a->long_blocks += k * it.long_blocks;
    if (!j->was) {
        a->tail     += k * it.bytes;
        a->tail_big += k * it.big;
    } else {
        a->sites += k;
        a->bytes += k * (it.bytes + TALLY_KEY_MOST);
    }
}

static void OwedAdd(EndpaperSince* to, const EndpaperSince* from)
{
    to->sites       += from->sites;
    to->bytes       += from->bytes;
    to->tail        += from->tail;
    to->tail_big    += from->tail_big;
    to->long_blocks += from->long_blocks;
    to->dsites      += from->dsites;
    to->dtail       += from->dtail;
    to->dbytes      += from->dbytes;
}

void EndpaperJotIn(const Jot* j)
{
    OwedJot(&s_inked, j, 1u);
    SinceSaid();
}

void EndpaperJotOut(const Jot* j)
{
    OwedJot(&s_inked, j, ENDPAPER_TAKES_OUT);
    SinceSaid();
}

/* Each window the Strips put in lie in, a place: wherever the next Tally's
 * last leaf begins, a place before it is the more. */
void EndpaperInked(const BookSplice* sp, uint64_t nsp)
{
    for (uint64_t i = 0; i < nsp; i++)
        for (uint64_t k = 0; k < sp[i].ninsert; k++)
            if (sp[i].insert[k].kind == STRIP_DATA)
                SinceGround(&s_inked, sp[i].insert[k].start, sp[i].insert[k].count, UINT64_MAX);
    SinceSaid();
}

void EndpaperDocketTaken(void)
{
    OwedAdd(&s_sewing, &s_inked);
    s_inked = (EndpaperSince){ .on = true };
}

void EndpaperDocketBack(void)
{
    OwedAdd(&s_inked, &s_sewing);
    s_sewing = (EndpaperSince){ .on = true };
}

void EndpaperDocketDropped(void)
{
    s_inked  = (EndpaperSince){ .on = true };
    s_sewing = (EndpaperSince){ .on = true };
    SinceSaid();
}

// ----------------------------------------------------------------------------
// What a Plot's Stakes may add (§16.6 п.8)
// ----------------------------------------------------------------------------

/*
 * ‼ GROUND HANDED TO A WRITE IS IN THE MEASURE BEFORE IT IS TAKEN. A Plot's
 * Stakes are named by no record until its Ink (plot.h): the measure by the
 * last Tally counts what Ink laid (above), the one from above walks the map,
 * and the kept measure's allowance for the data handed since it (endpaper.c,
 * EndpaperKeptCost) is from above alone — lost in the lesser of the two
 * wherever the measure by the last Tally is less, which past a Tally it is. So
 * a write taken to a full volume's reserve, once inked, raised the measure
 * past the ground left, and the Quire the reserve kept the ribbon was the
 * write's (fullvolume, roomback: next_quire refused under a Tally, the ribbon
 * run out, a delete refused). Each Plot counts what its Ink may add while its
 * Stakes stand, and a take's before the take is asked (plot_window.c,
 * PlotStakesCount): by the last Tally its file a place of its own with its
 * items whole at the widest — as the Jot its Ink lays is counted (OwedJot),
 * whether or not a Jot of the file stands, which a TAKE may carry off before
 * the Ink — its runs counted twice each, a run split by a write into it, and
 * each window a Stake lies in a place of DIGESTS (SinceGround; a take not had
 * yet, anywhere: every window its length can cross); from above, each Stake
 * two runs, each in a Parcel of its own, and four elements of DIGESTS and the
 * windows it crosses, its blocks' digests. The Ink lets the count go in the
 * hold of the state lock that counts its Jot and what it put in (ink_book.c);
 * a Plot that gives its Stakes back, with them.
 */
static void StakesCount(const EndpaperStakes* e, uint64_t k)
{
    s_staked.sites       += k * e->sites;
    s_staked.bytes       += k * e->bytes;
    s_staked.long_blocks += k * e->long_blocks;
    s_staked.dsites      += k * e->dsites;
    s_staked.dbytes      += k * e->dbytes;
    __atomic_add_fetch(&s_staked_files, k * e->files_bytes, __ATOMIC_RELEASE);
    __atomic_add_fetch(&s_staked_digests, k * e->digests_bytes, __ATOMIC_RELEASE);
}

void EndpaperStakesFile(EndpaperStakes* e, const FileRecord* rec, uint64_t fid, uint64_t runs, uint64_t blocks)
{
    EndpaperItems it;
    FileItemsAs(rec, fid, UINT64_MAX, FactGrid(), runs, blocks, &it);
    memset(e, 0, sizeof(*e));
    e->sites       = 1u;
    e->bytes       = it.bytes + TALLY_KEY_MOST;
    e->long_blocks = it.long_blocks;
}

void EndpaperStakesRun(EndpaperStakes* e, uint64_t start, uint64_t count)
{
    EndpaperSince d       = { .on = true };
    uint64_t      windows = (count + DIGEST_ITEM_MOST - 1u) / DIGEST_ITEM_MOST;
    if (count == 0)
        return;
    if (start != UINT64_MAX) {
        SinceGround(&d, start, count, UINT64_MAX);
    } else {
        d.dsites = windows + 1u;
        d.dbytes = 8u * count + d.dsites * (TALLY_ITEM_HEAD_MOST + TALLY_DIGEST_HEAD_MOST + TALLY_KEY_MOST);
    }
    e->dsites        += d.dsites;
    e->dbytes        += d.dbytes;
    e->files_bytes   += 2u * (TALLY_RUN_MOST + TALLY_ITEM_HEAD_MOST + TALLY_PARCEL_HEAD_MOST);
    e->digests_bytes += (4u + windows) * (TALLY_ITEM_HEAD_MOST + TALLY_DIGEST_HEAD_MOST) + 8u * count;
}

void EndpaperStakesGoneLocked(const EndpaperStakes* held)
{
    if (held->epoch && held->epoch == s_staked_epoch)
        StakesCount(held, ENDPAPER_TAKES_OUT);
}

void EndpaperStakesSet(EndpaperStakes* held, const EndpaperStakes* now)
{
    TagFSLock* lock  = &tagfs_get_state()->lock;
    uint32_t   epoch = tagfs_mount_epoch();
    TagFSLockTake(lock);
    EndpaperStakesGoneLocked(held);
    memset(held, 0, sizeof(*held));
    if (now && epoch && s_staked_epoch != epoch) {
        /* The first Plot of a mounting counts from none: a Plot of the one
         * before is counted in no measure of this one. */
        s_staked       = (EndpaperSince){ .on = true };
        s_staked_epoch = epoch;
        __atomic_store_n(&s_staked_files, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&s_staked_digests, 0, __ATOMIC_RELEASE);
    }
    if (now && epoch) {
        *held       = *now;
        held->epoch = epoch;
        StakesCount(held, 1u);
    }
    TagFSLockDrop(lock);
}

void EndpaperStakesAbove(uint64_t* files_bytes, uint64_t* digests_bytes)
{
    *files_bytes   = __atomic_load_n(&s_staked_files, __ATOMIC_ACQUIRE);
    *digests_bytes = __atomic_load_n(&s_staked_digests, __ATOMIC_ACQUIRE);
}

// ----------------------------------------------------------------------------
// The measure
// ----------------------------------------------------------------------------

/* What a reader takes in one hold of the state lock, and the numbers the
 * measure needs besides. */
typedef struct {
    EndpaperLaid  laid;
    EndpaperSince since;
    EndpaperSince owed;
    uint64_t      seqs;
    uint64_t      grid;
} EndpaperView;

/* The nodes more than they took that `pieces` pieces of the last Tally take,
 * cut in `runs` runs of pieces one after another, `bytes` gone in among them
 * (п.4): a run of k pieces at most 2(k·Sheet + g)/R + 1 − k more. */
static uint64_t Recut(uint64_t pieces, uint64_t runs, uint64_t bytes)
{
    if (pieces == 0 && bytes == 0)
        return 0;
    uint64_t r = ENDPAPER_RECUT_ROOM;
    uint64_t n = (2u * (pieces * TALLY_SHEET_PAYLOAD + bytes) + r - 1u) / r + runs;
    return n > pieces ? n - pieces : 0;
}

static uint64_t TailNodes(uint64_t bytes, uint64_t big)
{
    if (bytes == 0)
        return 0;
    uint64_t pairs = 2u * bytes / TALLY_LEAF_ROOM + 1u;
    uint64_t short_ = bytes / (TALLY_LEAF_ROOM - ENDPAPER_SMALL) + big + 1u;
    return pairs < short_ ? pairs : short_;
}

static bool SinceHuge(const EndpaperSince* a)
{
    return a->sites > ENDPAPER_HUGE || a->bytes > ENDPAPER_HUGE || a->tail > ENDPAPER_HUGE ||
           a->tail_big > ENDPAPER_HUGE || a->long_blocks > ENDPAPER_HUGE || a->dsites > ENDPAPER_HUGE ||
           a->dtail > ENDPAPER_HUGE || a->dbytes > ENDPAPER_HUGE;
}

/* FILES of the next Tally: the leaves laid, the pieces touched — each leaf a
 * Parcel's runs lie in, which widen (п.5), and each place a change went in —
 * each a run of its own, and what went in. A leaf of no run and no change is
 * the leaf it was: its items are spelled from nothing the next Tally's seq
 * moves (a file's header from its own numbers, a key from the one before it). */
static uint64_t FilesMost(const EndpaperView* v)
{
    const EndpaperSince *s = &v->since, *o = &v->owed;
    uint64_t             n0     = v->laid.files_leaves;
    uint64_t             pieces = s->sites + o->sites + v->laid.run_leaves;
    uint64_t             widen  = v->laid.runs * (TallyVarintBytes(v->seqs) + 1u);
    if (pieces > n0 + 1u)
        pieces = n0 + 1u;
    uint64_t more = Recut(pieces, pieces, s->bytes + o->bytes + widen) +
                    TailNodes(s->tail + o->tail, s->tail_big + o->tail_big);
    return v->laid.files_blocks + more * v->grid + s->long_blocks + o->long_blocks +
           EndpaperInnerBlocks(n0 + more, v->grid);
}

/* DIGESTS: each place a run of two pieces, the last piece one. */
static uint64_t DigestsMost(const EndpaperView* v)
{
    const EndpaperSince *s = &v->since, *o = &v->owed;
    uint64_t             n0     = v->laid.digests_leaves;
    uint64_t             last   = s->dtail || o->dtail ? 1u : 0;
    uint64_t             places = s->dsites + o->dsites;
    uint64_t             pieces = 2u * places + last, runs = places + last;
    if (pieces > n0 + 1u)
        pieces = n0 + 1u;
    if (runs > pieces)
        runs = pieces;
    uint64_t more = Recut(pieces, runs, s->dbytes + o->dbytes);
    return v->laid.digests_blocks + more * v->grid + EndpaperInnerBlocks(n0 + more, v->grid);
}

/* TAGS: the tags past the ones laid, `tags` and `text` the registry's now. */
static uint64_t TagsMost(const EndpaperView* v, uint64_t tags, uint64_t text)
{
    const EndpaperLaid* l = &v->laid;
    if (tags < l->tags || text < l->tags_text)
        return UINT64_MAX;
    uint64_t n     = tags - l->tags, t = text - l->tags_text;
    uint64_t more  = TailNodes(n * (TALLY_ITEM_HEAD_MOST + TALLY_TAG_BODY_MOST) + t, n);
    uint64_t sheets = t / (TALLY_LONG_TAG_TEXT + 1u) * TALLY_LONG_TAG_SHEETS;
    return l->tags_blocks + (more + sheets) * v->grid + EndpaperInnerBlocks(l->tags_leaves + more, v->grid);
}

void EndpaperFactMost(uint64_t* files, uint64_t* digests, uint64_t* tags)
{
    TagFSLock*   lock = &tagfs_get_state()->lock;
    EndpaperView v;
    uint64_t     ntags = 0, text = 0;
    TagFSLockTake(lock);
    v.laid  = s_laid;
    v.since = s_since;
    v.owed  = s_inked;
    OwedAdd(&v.owed, &s_sewing);
    OwedAdd(&v.owed, &s_staked);
    TagFSLockDrop(lock);
    *files = *digests = *tags = UINT64_MAX;
    if (!v.since.on || SinceHuge(&v.since) || SinceHuge(&v.owed) || v.laid.runs >= ENDPAPER_HUGE ||
        v.laid.tags >= ENDPAPER_HUGE || v.laid.tags_text >= ENDPAPER_HUGE)
        return;
    /* The next Tally's seq is past every one given, and past the tries the
     * medium may refuse before a stitch settles — at most one a sector of the
     * next_quire (§16.4 п.4). */
    uint64_t last = BookSeqSeen() + RibbonCursorNow()->quire_blocks * TAGFS_BLOCK_SECTORS;
    v.seqs = last >= v.since.seq ? last + 1u - v.since.seq : 1u;
    v.grid = FactGrid();
    if (!(v.laid.rot & (1u << TALLY_FILES)))
        *files = FilesMost(&v);
    if (!(v.laid.rot & (1u << TALLY_DIGESTS)))
        *digests = DigestsMost(&v);
    if (!(v.laid.rot & (1u << TALLY_TAGS)) && tagfs_get_state()->registry) {
        TagRegistryTextBytes(tagfs_get_state()->registry, &ntags, &text);
        if (ntags < ENDPAPER_HUGE && text < ENDPAPER_HUGE)
            *tags = TagsMost(&v, ntags, text);
    }
}

// ----------------------------------------------------------------------------
// What moved
// ----------------------------------------------------------------------------

void EndpaperTallyBegun(uint64_t seq, uint64_t next_fid)
{
    s_pending = (EndpaperSince){ .on = true, .seq = seq, .fid = next_fid };
}

void EndpaperTallyLaid(const EndpaperLaid* l)
{
    TagFSLock* lock = &tagfs_get_state()->lock;
    if (l && s_pending.on) {
        TagFSLockTake(lock);
        s_laid  = *l;
        s_since = s_pending;
        SinceSaid();
        TagFSLockDrop(lock);
    }
    memset(&s_pending, 0, sizeof(s_pending));
}

/* The ground a settled Jot's records put in: every run of a Born, the runs
 * the splices of a Shaped put in — a Jot of any kind with splices carries one
 * (docket.c, JotChangedPut). */
static void GrewGround(const Jot* j, EndpaperSince* since, EndpaperSince* pending)
{
    if (j->kind == JOT_BORN && j->now) {
        StripWalk w;
        Strip     s;
        StripWalkBegin(j->now, &w);
        while (StripWalkNextGround(&w, &s))
            SinceStrip(since, pending, &s);
        return;
    }
    uint64_t n = 0;
    for (uint64_t i = 0; i < j->nsplices; i++)
        n += j->splices[i].ninsert;
    for (uint64_t i = 0; i < n; i++)
        SinceStrip(since, pending, &j->inserted[i]);
}

void EndpaperGrew(const DocketTaken* t, uint64_t seq)
{
    TagFSLock*    lock    = &tagfs_get_state()->lock;
    EndpaperSince since   = s_since;
    uint64_t      grid    = FactGrid();
    for (const Jot* j = t->first; j; j = j->next[0]) {
        if (!j->carried)
            continue;       /* nothing of the file differs: what it was counted at stands */
        uint64_t was_gen = j->was ? __atomic_load_n(&j->was->gen, __ATOMIC_RELAXED) : 0;
        SinceFile(&since, j->fid, j->was, was_gen, j->now, seq, grid);
        SinceFile(&s_pending, j->fid, j->was, was_gen, j->now, seq, grid);
        GrewGround(j, &since, &s_pending);
    }
    TagFSLockTake(lock);
    s_since  = since;
    s_sewing = (EndpaperSince){ .on = true };
    SinceSaid();
    TagFSLockDrop(lock);
}

/* A file the mount replayed a stitch of past the Tally: in whole, and the
 * ground its stitches put in. */
static void StoodFile(uint64_t fid, const FileRecord* rec, uint64_t grid)
{
    StripWalk w;
    Strip     s;
    SinceFile(&s_since, fid, NULL, 0, rec, rec->gen, grid);
    StripWalkBegin(rec, &w);
    while (StripWalkNextGround(&w, &s))
        if (s.kind == STRIP_DATA && s.birth > s_since.seq)     /* a run SHARED lays no element of DIGESTS */
            SinceGround(&s_since, s.start, s.count, s_laid.digests_last);
}

void EndpaperStood(const EndpaperLaid* l, uint64_t seq, uint64_t next_fid)
{
    FileMap* map  = tagfs_get_state()->files;
    uint64_t grid = FactGrid(), from = 0, ids[32];
    uint32_t n;
    s_laid    = *l;
    s_since   = (EndpaperSince){ .on = true, .seq = seq, .fid = next_fid };
    memset(&s_pending, 0, sizeof(s_pending));
    EndpaperDocketDropped();
    while (map && (n = FileMapFiles(map, from, ids, 32)) > 0) {
        for (uint32_t i = 0; i < n; i++) {
            FileRecord* rec = FileMapRecordHold(map, ids[i], NULL);
            if (rec && __atomic_load_n(&rec->gen, __ATOMIC_RELAXED) > seq)
                StoodFile(ids[i], rec, grid);
            if (rec)
                FileRecordRelease(rec);
        }
        from = ids[n - 1] + 1u;
    }
    SinceSaid();
}

void EndpaperForget(void)
{
    memset(&s_laid, 0, sizeof(s_laid));
    memset(&s_since, 0, sizeof(s_since));
    memset(&s_pending, 0, sizeof(s_pending));
    s_staked       = (EndpaperSince){ .on = true };
    s_staked_epoch = 0;
    __atomic_store_n(&s_staked_files, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_staked_digests, 0, __ATOMIC_RELEASE);
    EndpaperDocketDropped();
}
