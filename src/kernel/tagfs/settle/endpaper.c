#include "sewing.h"
#include "../file_map/file_map.h"
#include "../tag_registry/tag_registry.h"
#include "../use_memory/use_memory.h"
#include "../mount/book_mount.h"
#include "../tag_spelling/tag_spelling.h"

/*
 * The book's reserve (sewing.h): what a whole Tally of the volume as memory
 * holds it would take, measured from above (spec_v2 §3.6, §16.2 п.6).
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
 * pointers fill. The writer of the whole Tally (the next step) lays it to the
 * byte, and this measure becomes that: one function, the Tally's own.
 *
 * Why from above: the reserve is what lets the ribbon go on and a Tally be
 * written on a full volume (§3.6). Measured too small, a Tally would be begun
 * that the ground does not hold; measured too large, the ribbon gives up a few
 * Quires sooner than it must.
 */

#define TALLY_NODE_HEAD_MOST  44u    /* NodeHead: tag, length, tree, level, items, a pair key — each at its widest */
#define TALLY_ITEM_HEAD_MOST  25u    /* an item's length (5) and a pair key (10 + 10) */
#define TALLY_FILE_HEAD_MOST  60u    /* gen, flags, size, created, mtime_z (10 each), name and tag counts (5 each) */
#define TALLY_TAG_MOST         5u    /* a tag id, written as its distance from the one before */
#define TALLY_RUN_MOST        25u    /* a run of the Tally's form: head 5, start 10, birth_delta 10 (§2.9) */
#define TALLY_PARCEL_HEAD_MOST 5u    /* a Parcel's count of runs */
#define TALLY_TAG_BODY_MOST   20u    /* a tag's flags, key length and value length */
#define TALLY_USE_BODY_MOST   10u    /* the Use Context's length */
#define TALLY_DIGEST_HEAD_MOST 5u    /* an element's count of digests */
#define TALLY_INNER_ITEM_MOST 72u    /* an inner item: its head, a count, a BookPtr of two copies */

#define TALLY_LEAF_ROOM       (TALLY_SHEET_PAYLOAD - TALLY_NODE_HEAD_MOST)

/* A tag's item longer than a leaf holds: more text than this, and every one
 * of them alone in a node of at most TALLY_LONG_TAG_SHEETS Sheets. */
#define TALLY_LONG_TAG_TEXT   (TALLY_LEAF_ROOM - TALLY_ITEM_HEAD_MOST - TALLY_TAG_BODY_MOST)
#define TALLY_LONG_TAG_SHEETS ((TALLY_NODE_HEAD_MOST + TALLY_ITEM_HEAD_MOST + TALLY_TAG_BODY_MOST + TAG_TEXT_WIDTH + \
                                TALLY_SHEET_PAYLOAD - 1u) / TALLY_SHEET_PAYLOAD)

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
    uint64_t nodes  = TallyFilled(t->bytes, TALLY_LEAF_ROOM);
    uint64_t blocks = nodes * atom + t->sheets * atom;
    while (nodes > 1u) {
        nodes   = TallyFilled(nodes * TALLY_INNER_ITEM_MOST, TALLY_LEAF_ROOM);
        blocks += nodes * atom;
    }
    return blocks;
}

/* One live file into FILES and DIGESTS: its header, its Parcels, and the
 * digests of the blocks its runs name — an element at the least for each run,
 * and one more for each window of DIGEST_ITEM_MOST a run crosses. */
static void TallyFileCost(const FileRecord* rec, TallyTreeCost* files, TallyTreeCost* digests)
{
    TallyItemCost(files, TALLY_FILE_HEAD_MOST + rec->name_len + (uint64_t)rec->tag_count * TALLY_TAG_MOST);
    uint64_t runs    = rec->strip_count;
    uint64_t parcels = (runs + PARCEL_STRIPS_MOST - 1u) / PARCEL_STRIPS_MOST;
    for (uint64_t p = 0; p < parcels; p++) {
        uint64_t in = runs / parcels + (p < runs % parcels ? 1u : 0u);
        TallyItemCost(files, TALLY_PARCEL_HEAD_MOST + in * TALLY_RUN_MOST);
    }
    uint64_t elements = 2u * runs + (rec->blocks + DIGEST_ITEM_MOST - 1u) / DIGEST_ITEM_MOST;
    digests->bytes += elements * (TALLY_ITEM_HEAD_MOST + TALLY_DIGEST_HEAD_MOST) + rec->blocks * 8u;
}

/* Every live file of the mirror, a batch of ids at a time. */
static void TallyFilesCost(TallyTreeCost* files, TallyTreeCost* digests)
{
    FileMap* map  = tagfs_get_state()->files;
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

uint64_t TallyCostBlocks(void)
{
    TallyTreeCost files = { 0 }, tags = { 0 }, use = { 0 }, digests = { 0 };
    uint64_t      atom  = g_book.atom > TAGFS_BLOCK_SIZE ? g_book.atom / TAGFS_BLOCK_SIZE : 1u;
    uint64_t      ntags = 0, text = 0;

    TallyFilesCost(&files, &digests);
    if (tagfs_get_state()->registry)
        TagRegistryTextBytes(tagfs_get_state()->registry, &ntags, &text);
    /* The registry counts its tags and their text, not each one's: every tag
     * at its whole length — more than a long one's one byte past a room — and
     * as many long ones as the text holds more than TALLY_LONG_TAG_TEXT bytes
     * a tag of. */
    tags.bytes  = ntags * (TALLY_ITEM_HEAD_MOST + TALLY_TAG_BODY_MOST) + text;
    tags.sheets = text / (TALLY_LONG_TAG_TEXT + 1u) * TALLY_LONG_TAG_SHEETS;
    UseText* memory = tagfs_recall_use_context(NULL);
    if (memory && memory->bytes)
        TallyItemCost(&use, TALLY_USE_BODY_MOST + memory->bytes);
    UseTextDrop(memory);
    return TallyTreeBlocks(&files, atom) + TallyTreeBlocks(&tags, atom) + TallyTreeBlocks(&use, atom) +
           TallyTreeBlocks(&digests, atom);
}

uint64_t EndpaperKeeps(uint64_t quire_blocks)
{
    if (quire_blocks == 0)
        return 0;
    uint64_t cost = TallyCostBlocks();
    return 2u * ((cost + quire_blocks - 1u) / quire_blocks) * quire_blocks;
}
