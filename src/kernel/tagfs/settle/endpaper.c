#include "sewing.h"
#include "binder.h"
#include "../file_map/file_map.h"
#include "../tag_registry/tag_registry.h"
#include "../use_memory/use_memory.h"
#include "../mount/book_mount.h"
#include "../tag_spelling/tag_spelling.h"
#include "../block_map/block_map.h"
#include "endpaper_fact.h"
#include "tagfs_volume.h"

/*
 * The book's reserve (sewing.h): what a whole Tally of the volume as memory
 * holds it would take, measured from above.
 *
 * ‼ AN UPPER BOUND, BY THE RULES EVERY WRITER OF A TALLY KEEPS — never less
 * than the Tally a writer lays of the same mirror (§16.2 п.24, §16.3 пп.1, 5):
 * every node from the boundary of an atom, a leaf a Sheet of 3904 bytes filled
 * greedily by its items in key order, a node of several Sheets only for an
 * item longer than one, alone in it; a file a header and its Parcels of at
 * most PARCEL_STRIPS_MOST runs; DIGESTS an element a run of blocks in one
 * window of DIGEST_ITEM_MOST. Each item is taken at the most its fields can
 * spell (a varint at its widest, a run of the Tally's form at 25 bytes).
 *
 * The nodes of a level are counted by pairs: of two nodes laid one after the
 * other, the pair holds more than one leaf's room — the first closed because
 * the item that begins the second did not fit it, and an item alone in a node
 * of several Sheets is longer than that room by itself. So a level's nodes
 * are at most twice the rooms its items fill, an item longer than a leaf
 * taken at one byte past a room — and an item that long takes the Sheets of
 * its own node besides. A long item between short ones closes the leaf before
 * it early and leaves the one after it short: a measure of the leaves by the
 * short items' bytes alone, the long ones' nodes added after, is less than
 * what a writer lays. Each level above is counted the same, at twice what its
 * pointers fill (EndpaperInnerBlocks). The reserve takes FILES, TAGS and
 * DIGESTS by this measure only where the one by the last Tally laid and the
 * growth since (endpaper_fact.c) is not known or is more; USE — one item, its
 * length now what the next Tally lays — always.
 *
 * Why from above: the reserve is what lets the ribbon go on and a Tally be
 * written on a full volume (§3.6). Measured too small, a Tally would be begun
 * that the ground does not hold; measured too large, the ribbon gives up a few
 * Quires sooner than it must.
 */

#define TALLY_FILE_HEAD_MOST  60u    /* gen, flags, size, created, mtime_z (10 each), name and tag counts (5 each) */
#define TALLY_FILE_MADE_BY_MOST 10u  /* made_by, which the header of a file a program made has besides */
#define TALLY_FILE_SHOWN_MOST  10u   /* the count, and each id, of the programs a program's file shows its tags to */
#define TALLY_FILE_HEAD_BYTES  (1u + TALLY_HEAD_MOST)   /* a file's first bytes and their count */
#define TALLY_FILE_PASSPORT_MOST 10u  /* the passport of the signed program a file holds */
#define TALLY_TAG_MOST         5u    /* a tag id, written as its distance from the one before */
#define TALLY_USE_BODY_MOST   10u    /* the Use Context's length */

/* What one tree of the Tally holds, measured: the bytes its items fill its
 * leaves by — a long item's at one byte past a leaf's room — and the Sheets of
 * the nodes its long items are alone in. */
typedef struct {
    uint64_t bytes;
    uint64_t sheets;
} TallyTreeCost;

/* One item of `body` bytes into its tree's measure. */
static void TallyItemCost(TallyTreeCost* t, uint64_t body)
{
    uint64_t bytes = TALLY_ITEM_HEAD_MOST + body;
    if (bytes <= TALLY_LEAF_ROOM) {
        t->bytes += bytes;
        return;
    }
    t->bytes  += TALLY_LEAF_ROOM + 1u;
    t->sheets += (TALLY_NODE_HEAD_MOST + bytes + TALLY_SHEET_PAYLOAD - 1u) / TALLY_SHEET_PAYLOAD;
}

/* Nodes laid in order that hold `bytes` of items, of every two one after the
 * other the pair more than `room`: at most twice the rooms the bytes fill. */
static uint64_t TallyFilled(uint64_t bytes, uint64_t room)
{
    if (bytes == 0)
        return 0;
    return bytes <= room ? 1u : 2u * ((bytes + room - 1u) / room);
}

/* The blocks a tree takes, `atom` blocks a node at the least: its nodes of
 * level 0 and the Sheets its long items take besides, and every level of
 * inner nodes above until one is its root. */
static uint64_t TallyTreeBlocks(const TallyTreeCost* t, uint64_t atom)
{
    uint64_t nodes = TallyFilled(t->bytes, TALLY_LEAF_ROOM);
    return nodes * atom + t->sheets * atom + EndpaperInnerBlocks(nodes, atom);
}

/* One live file into FILES and DIGESTS: its header; its Parcels — memory's,
 * one to one, at most PARCEL_STRIPS_MOST runs each and, of a file of more
 * than one, at least PARCEL_STRIPS_LEAST (file_record.h), so no more of them
 * than one a PARCEL_STRIPS_LEAST runs, each shorter than a leaf; and the
 * digests of the blocks its runs name — an element at the least for each run,
 * and one more for each window of DIGEST_ITEM_MOST a run crosses. */
static void TallyFileCost(const FileRecord* rec, TallyTreeCost* files, TallyTreeCost* digests)
{
    TallyItemCost(files, TALLY_FILE_HEAD_MOST + rec->name_len + (uint64_t)rec->tag_count * TALLY_TAG_MOST +
                             ((rec->flags & TALLY_FILE_MADE_BY) ? TALLY_FILE_MADE_BY_MOST : 0u) +
                             ((rec->flags & TALLY_FILE_SHOWN) ? (1u + (uint64_t)rec->shown_count) *
                                                                    TALLY_FILE_SHOWN_MOST : 0u) +
                             ((rec->flags & TALLY_FILE_HEAD) ? TALLY_FILE_HEAD_BYTES : 0u) +
                             ((rec->flags & TALLY_FILE_PASSPORT) ? TALLY_FILE_PASSPORT_MOST : 0u));
    uint64_t runs    = rec->strip_count;
    uint64_t parcels = (runs + PARCEL_STRIPS_LEAST - 1u) / PARCEL_STRIPS_LEAST;
    files->bytes += parcels * (TALLY_ITEM_HEAD_MOST + TALLY_PARCEL_HEAD_MOST) + runs * TALLY_RUN_MOST;
    uint64_t elements = 2u * runs + (rec->blocks + DIGEST_ITEM_MOST - 1u) / DIGEST_ITEM_MOST;
    digests->bytes += elements * (TALLY_ITEM_HEAD_MOST + TALLY_DIGEST_HEAD_MOST) + rec->blocks * 8u;
}

/* Every live file of the mirror, a batch of ids at a time. */
static void TallyFilesCost(TagFSVolume* vol, TallyTreeCost* files, TallyTreeCost* digests)
{
    FileMap* map  = vol->state.files;
    uint64_t from = 0, ids[32];
    uint32_t n;
    while (map && (n = FileMapFiles(map, from, ids, 32)) > 0) {
        for (uint32_t i = 0; i < n; i++) {
            FileRecord* rec = FileMapRecordHold(map, ids[i], NULL);
            if (!rec)
                continue;
            TallyFileCost(rec, files, digests);
            FileRecordRelease(rec);
        }
        from = ids[n - 1] + 1u;
    }
}


static void TallyCostOf(TagFSVolume* vol, TallyCostParts* out)
{
    TallyTreeCost files = { 0 }, tags = { 0 }, use = { 0 }, digests = { 0 };
    uint64_t      atom  = vol->book.atom > TAGFS_BLOCK_SIZE ? vol->book.atom / TAGFS_BLOCK_SIZE : 1u;
    uint64_t      ntags = 0, text = 0;

    /* The Plots' Stakes first, the map after: a Stake inked between the two
     * is counted twice, never missed (endpaper_fact.h). */
    EndpaperStakesAbove(vol, &files.bytes, &digests.bytes);
    TallyFilesCost(vol, &files, &digests);
    if (vol->state.registry)
        TagRegistryTextBytes(vol->state.registry, &ntags, &text);
    /* The registry counts its tags and their text, not each one's: every tag
     * at its whole length — more than a long one's one byte past a room — and
     * as many long ones as the text holds more than TALLY_LONG_TAG_TEXT bytes
     * a tag of. */
    tags.bytes  = ntags * (TALLY_ITEM_HEAD_MOST + TALLY_TAG_BODY_MOST) + text;
    tags.sheets = text / (TALLY_LONG_TAG_TEXT + 1u) * TALLY_LONG_TAG_SHEETS;
    UseText* memory = tagfs_recall_use_context(vol, NULL);
    if (memory && memory->bytes)
        TallyItemCost(&use, TALLY_USE_BODY_MOST + memory->bytes);
    UseTextDrop(memory);
    UseText* name = VolumeNameHeld(vol);
    if (name && name->bytes)
        TallyItemCost(&use, TALLY_USE_BODY_MOST + name->bytes);
    UseTextDrop(name);
    out->files   = TallyTreeBlocks(&files, atom);
    out->tags    = TallyTreeBlocks(&tags, atom);
    out->use     = TallyTreeBlocks(&use, atom);
    out->digests = TallyTreeBlocks(&digests, atom);
}

static uint64_t TallyLess(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

/* The next Tally (§16.4 п.7), `above` the measure from above: each of FILES,
 * TAGS and DIGESTS by the last Tally and the growth since — every change
 * memory holds among it (endpaper_fact.c) — where that is known and less;
 * both are bounds of it from above. */
static uint64_t TallyNextOf(TagFSVolume* vol, const TallyCostParts* above)
{
    uint64_t files = 0, digests = 0, tags = 0;
    EndpaperFactMost(vol, &files, &digests, &tags);
    return TallyLess(files, above->files) + TallyLess(tags, above->tags) + above->use +
           TallyLess(digests, above->digests);
}

#ifdef CONFIG_VOLUME_TESTS
uint64_t TallyCostBlocks(TagFSVolume* vol)
{
    TallyCostParts p;
    TallyCostOf(vol, &p);
    return TallyNextOf(vol, &p);
}
#endif

/*
 * ‼ THE RESERVE A WRITE OF DATA LEAVES IS KEPT, NOT MEASURED A WRITE (§3.6). A
 * measure walks every live file of the mirror — too much for every window a
 * write takes — so each measure is kept, with the blocks a book's data had been
 * handed when the measure before it began (AllotmentBookTaken): a block handed
 * since may be in no record the measure walked yet, a Stake of a write still
 * in flight among them. Every such block is counted as the most it can add to
 * a Tally — a run of its own in FILES, in a Parcel of its own, and an element
 * of its own in DIGESTS with its digest. What is kept is the measure from
 * above, its trees apart: the allowance is a bound by the same rule as it,
 * not by the one of the last Tally (endpaper_fact.c) — by which a block put
 * in an old file counts the file whole — and the measure by the last Tally is
 * read as it stands each time, every change memory holds in it. So the kept
 * measure is the lesser of the two as a measure taken now is, each no less
 * than a measure taken now's, and never less than it. A block still staked
 * is counted by its Plot in both measures besides (endpaper_fact.c,
 * EndpaperStakesSet) — the allowance here, from above alone, is lost wherever
 * the measure by the last Tally is the lesser. The ribbon measures afresh each
 * time it is given a Quire, and a write that finds no room asks a fresh
 * measure before it is refused (AllotmentTakeBook).
 */
#define TALLY_DATA_FILES_MOST   (TALLY_RUN_MOST + TALLY_ITEM_HEAD_MOST + TALLY_PARCEL_HEAD_MOST)
#define TALLY_DATA_DIGESTS_MOST (8u + 2u * (TALLY_ITEM_HEAD_MOST + TALLY_DIGEST_HEAD_MOST))


/*
 * ‼ A KEPT MEASURE IS ONE PAIR, NEVER TWO HALVES. The ribbon measures when it is
 * given a Quire and a data writer measures afresh, with no lock between them:
 * a cost kept beside the `from` of another measure — one begun later — would
 * leave out the blocks handed between the two, and the reserve kept for the
 * data be short by their Tally. So one measure is kept at a time — one that
 * finds another being kept keeps nothing, and the next is the more careful for
 * it — and a reader takes the pair whole, or measures afresh. A measure is kept
 * with the data handed when the one kept before it began, or when it began
 * itself, whichever came first.
 */
uint64_t EndpaperKeeps(TagFSVolume* vol, uint64_t quire_blocks)
{
    if (quire_blocks == 0)
        return 0;
    TallyCostParts above;
    uint32_t       epoch = tagfs_mount_epoch(vol);
    uint64_t       began = AllotmentBookTaken(vol);
    TallyCostOf(vol, &above);
    uint64_t cost = TallyNextOf(vol, &above);
    uint32_t turn = __atomic_load_n(&vol->endpaper.kept_turn, __ATOMIC_ACQUIRE);
    if (!(turn & 1u) &&
        __atomic_compare_exchange_n(&vol->endpaper.kept_turn, &turn, turn + 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        uint64_t before = vol->endpaper.kept_epoch == epoch ? vol->endpaper.kept_began : began;
        vol->endpaper.kept_above = above;
        vol->endpaper.kept_from  = before < began ? before : began;
        vol->endpaper.kept_began = before > began ? before : began;
        vol->endpaper.kept_epoch = epoch;
        __atomic_store_n(&vol->endpaper.kept_turn, turn + 2u, __ATOMIC_RELEASE);
    }
    uint64_t tally = 2u * ((cost + quire_blocks - 1u) / quire_blocks) * quire_blocks;
    return tally + BinderRingShort(vol, tally, quire_blocks);
}

/* The kept measure of this mounting, whole: false when there is none, or one
 * was being kept as it was read. */
static bool EndpaperKept(TagFSVolume* vol, TallyCostParts* above, uint64_t* from)
{
    uint32_t turn = __atomic_load_n(&vol->endpaper.kept_turn, __ATOMIC_ACQUIRE);
    if (turn & 1u)
        return false;
    bool ours = vol->endpaper.kept_epoch == tagfs_mount_epoch(vol);
    *above    = vol->endpaper.kept_above;
    *from     = vol->endpaper.kept_from;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return ours && __atomic_load_n(&vol->endpaper.kept_turn, __ATOMIC_RELAXED) == turn;
}

/* The next Tally by the kept measure, the data handed since it and what the
 * Plots' Stakes may add now — a take counted before it is asked, which no
 * measure kept before it holds (endpaper_fact.c): false when there is none
 * whole. */
static bool EndpaperKeptCost(TagFSVolume* vol, uint64_t* cost)
{
    TallyCostParts above;
    uint64_t       from = 0, staked = 0, sdigests = 0;
    EndpaperStakesAbove(vol, &staked, &sdigests);
    if (!EndpaperKept(vol, &above, &from))
        return false;
    uint64_t      since = AllotmentBookTaken(vol) - from;
    uint64_t      atom  = vol->book.atom > TAGFS_BLOCK_SIZE ? vol->book.atom / TAGFS_BLOCK_SIZE : 1u;
    TallyTreeCost files   = { since * TALLY_DATA_FILES_MOST + staked, 0 };
    TallyTreeCost digests = { since * TALLY_DATA_DIGESTS_MOST + sdigests, 0 };
    above.files   += TallyTreeBlocks(&files, atom);
    above.digests += TallyTreeBlocks(&digests, atom);
    *cost = TallyNextOf(vol, &above);
    return true;
}

#ifdef CONFIG_VOLUME_TESTS
uint64_t TallyKeptBlocks(TagFSVolume* vol)
{
    uint64_t cost = UINT64_MAX;
    return EndpaperKeptCost(vol, &cost) ? cost : UINT64_MAX;
}
#endif

uint64_t EndpaperDataKeeps(TagFSVolume* vol, bool afresh)
{
    uint64_t q = RibbonCursorNow(vol)->quire_blocks, cost = 0;
    if (q == 0)
        return 0;
    if (afresh || !EndpaperKeptCost(vol, &cost))
        return EndpaperKeeps(vol, q) + q;
    uint64_t tally = 2u * ((cost + q - 1u) / q) * q;
    return tally + BinderRingShort(vol, tally, q) + q;
}
