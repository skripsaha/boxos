#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../file_map/file_map.h"
#include "../metadata_pool/meta_pool.h"
#include "../block_map/block_map.h"
#include "../ink/ink.h"
#include "../ink/fallow.h"
#include "../glean/glean.h"
#include "../plot/plot.h"
#include "../settle/settle.h"
#include "block_fail.h"   /* BLOCKFAIL that refuses nothing — a count of the writes to the data run */
#include "amp.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// Ink and the pool it lays records in (step 1.4)
//
// Every change of a file is one Ink: its record laid in the pool's tail under
// the volume's state lock, the record of the same file no cycle has taken yet
// taken back and the ones after it moved down, a delete a Quitclaim appended
// like any record, and a tail with no room closed onto a Spare — the reserve's
// two shares, the deletes' and Glean's, kept from every change that frees
// nothing (meta_pool.h). These tests ask the pool's bookkeeping what the
// medium and the next mount will be told: where each file's record sits, what
// the newest record of a deleted file is, whether a volume with no free block
// can still give a file up, and whether a Spare no link names yet is free on
// the medium's map.
// ============================================================================

#define STAND_INK_FILES    16u     /* files the interleaved writes go round */
#define STAND_INK_ROUNDS   8u      /* writes each of them takes */
#define STAND_INK_CHUNK    16u     /* bytes a write appends */
#define STAND_INK_DELETES  (POOL_QUITCLAIMS_PER_BLOCK + 2u)   /* one more than a tail holds */
#define STAND_INK_REST_ROUNDS 16u  /* cycles a volume gets to bring Fallow and Glean to rest */

/* More files than the fullest tail and the deletes' share of the reserve hold
 * Quitclaims of: a tail's room is a block's at most. */
#define STAND_INK_MOST     ((1u + SPARE_FOR_QUITCLAIMS) * POOL_QUITCLAIMS_PER_BLOCK + 1u)

static uint64_t s_ink_fids[STAND_INK_MOST];

/* Files made for a test, all called test_file, and given up on the way out. */
static bool StandInkMake(uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        s_ink_fids[i] = 0;
        if (tagfs_create_file("test_file", NULL, 0, &s_ink_fids[i]) != 0)
            return false;
    }
    return true;
}

static void StandInkLetGo(uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        if (s_ink_fids[i])
            tagfs_delete_file(s_ink_fids[i], INK_UNWARRANTED);
        s_ink_fids[i] = 0;
    }
}

// ----------------------------------------------------------------------------
// Where every record sits
// ----------------------------------------------------------------------------

/* The place of every live file's record — (link << 32) | offset — into a
 * list of the caller's to kfree, and how many of them do not name a whole
 * record of their own file (RecordWhereValid). NULL for no memory. */
static uint64_t* StandInkPlaces(uint32_t* out_count, uint32_t* out_invalid)
{
    TagFSState* fs    = tagfs_get_state();
    uint32_t    total = FileMapCount(fs->files);
    uint64_t*   place = kmalloc(sizeof(uint64_t) * (total ? total : 1u));
    uint64_t    fids[64];
    uint64_t    from = 0;
    uint32_t    n = 0, invalid = 0;

    if (!place)
        return NULL;
    for (;;) {
        uint32_t got = FileMapRows(fs->files, from, fids, 64);
        for (uint32_t i = 0; i < got && n < total; i++) {
            RecordWhere where;
            FileRecord* rec = FileMapRecordHold(fs->files, fids[i], &where);
            if (!rec)
                continue;
            FileRecordRelease(rec);
            TagFSLockTake(&fs->lock);
            invalid += !RecordWhereValid(&where, fids[i]);
            TagFSLockDrop(&fs->lock);
            place[n++] = ((uint64_t)where.link << 32) | where.offset;
        }
        if (got < 64)
            break;
        from = fids[got - 1] + 1;
    }
    *out_count   = n;
    *out_invalid = invalid;
    return place;
}

/* Two places alike in the list: the list sorted first. */
static uint32_t StandInkShared(uint64_t* place, uint32_t n)
{
    for (uint32_t i = 1; i < n; i++) {
        uint64_t v = place[i];
        uint32_t j = i;
        for (; j > 0 && place[j - 1] > v; j--)
            place[j] = place[j - 1];
        place[j] = v;
    }
    uint32_t shared = 0;
    for (uint32_t i = 1; i < n; i++)
        shared += place[i] == place[i - 1];
    return shared;
}

/* Sixteen files written in turn, eight small appends each, with a Settle cycle
 * half way: every write inks a record, the one of the same file the tail
 * still holds is taken back and every record after it moves down, and the
 * cycle freezes what it took. Two live files whose records the map places at
 * one offset, or a place that is not a whole record of its own file, is the
 * pool naming one file with another's record — what writers racing an
 * unlocked commit did (W-LOCK). Each file then reads back as written. */
static TestResult TestStandInkRecordsNeverSharePlace(void)
{
    TestResult  verdict = TEST_PASS;
    uint64_t*   place   = NULL;
    uint32_t    n = 0, invalid = 0, wrong = 0;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT(StandInkMake(STAND_INK_FILES), "sixteen files are created");
    for (uint32_t r = 0; r < STAND_INK_ROUNDS; r++) {
        if (r == STAND_INK_ROUNDS / 2)
            TEST_ASSERT_OK(TagFSSettle(), "a Settle cycle half way freezes what the tail holds");
        for (uint32_t i = 0; i < STAND_INK_FILES; i++) {
            TestFillAt(g_test_buffer, STAND_INK_CHUNK, s_ink_fids[i], (uint64_t)r * STAND_INK_CHUNK);
            TEST_ASSERT_EQ(StandWriteAt(s_ink_fids[i], (uint64_t)r * STAND_INK_CHUNK, g_test_buffer,
                                        STAND_INK_CHUNK), STAND_INK_CHUNK,
                           "each file takes its append in turn");
        }
    }

    place = StandInkPlaces(&n, &invalid);
    TEST_ASSERT(place != NULL, "the places of the live files' records are listed");
    uint32_t shared = StandInkShared(place, n);

    for (uint32_t i = 0; i < STAND_INK_FILES; i++) {
        uint32_t bytes = STAND_INK_ROUNDS * STAND_INK_CHUNK;
        TestFillAt(g_test_buffer, bytes, s_ink_fids[i], 0);
        if (TestFileRead(s_ink_fids[i], g_test_output, sizeof(g_test_output)) != (int)bytes ||
            TestFirstDifference(g_test_output, g_test_buffer, bytes) != bytes)
            wrong++;
    }

    TestEvidence("live files' records the map places where another's is, or at no whole record "
                 "of their own, of those listed", (uint64_t)shared + invalid);
    if (shared || invalid) {
        verdict = TEST_DEFECT;
        goto out;
    }
    TEST_ASSERT_EQ(wrong, 0, "and every file reads back what was written into it");
out:
    kfree(place);
    StandInkLetGo(STAND_INK_FILES);
    return verdict;
}

// ----------------------------------------------------------------------------
// A volume with no free block still deletes
// ----------------------------------------------------------------------------

error_t StandInkTakeAll(StandInkGround* g)
{
    uint64_t start = 0;
    uint32_t got   = 0;
    uint32_t all   = tagfs_get_state()->layout.data_blocks;
    error_t  rc;

    while ((rc = TagFSTakeRun(all, 0, &start, &got)) == OK) {
        if (g->count == g->cap) {
            uint32_t  cap = g->cap ? g->cap * 2u : 64u;
            uint64_t* at  = kmalloc(sizeof(uint64_t) * cap);
            uint32_t* n   = kmalloc(sizeof(uint32_t) * cap);
            if (!at || !n) {
                kfree(at);
                kfree(n);
                tagfs_free_blocks(start, got);
                return ERR_NO_MEMORY;
            }
            if (g->count) {
                memcpy(at, g->at, sizeof(uint64_t) * g->count);
                memcpy(n, g->n, sizeof(uint32_t) * g->count);
            }
            kfree(g->at);
            kfree(g->n);
            g->at  = at;
            g->n   = n;
            g->cap = cap;
        }
        g->at[g->count] = start;
        g->n[g->count]  = got;
        g->count++;
    }
    return rc;
}

/* Cycles run until nothing rests in Fallow and the pool's chain has no slack
 * Glean could take off — the pass told the allocator is short, as a take that
 * met no room would tell it (MapShort) — at least one, so what the test made
 * is settled, and each with free ground to make the reserve whole from. They
 * are begun as the cadence begins them: a cycle with a waiter closes the tail
 * Glean has just laid records in, and the chain would keep a block of slack
 * for ever. True once at rest: then a volume whose free blocks are all taken
 * answers ERR_TAGFS_NO_SPACE, never ERR_WOULD_BLOCK, and no cycle can give a
 * block back. */
static bool StandInkAtRest(void)
{
    TagFSState* fs   = tagfs_get_state();
    bool        rest = false;

    for (uint32_t round = 0; !rest && round < STAND_INK_REST_ROUNDS; round++) {
        if (TagFSSettleAsCadence() != OK)
            return false;
        TagFSLockTake(&fs->lock);
        bool slack = GleanMayFree();
        rest       = FallowBlocks() == 0 && !slack;
        TagFSLockDrop(&fs->lock);
        if (slack)
            GleanShort();
    }
    return rest;
}

/* How many Spares the pool holds, and up to `cap` of their blocks. */
static uint32_t StandInkSpares(uint32_t* out, uint32_t cap)
{
    TagFSState* fs = tagfs_get_state();
    TagFSLockTake(&fs->lock);
    uint32_t held = PoolSparesHeld(out, cap);
    TagFSLockDrop(&fs->lock);
    return held;
}

void StandInkGiveBack(StandInkGround* g)
{
    for (uint32_t i = 0; i < g->count; i++)
        tagfs_free_blocks(g->at[i], g->n[i]);
    kfree(g->at);
    kfree(g->n);
    memset(g, 0, sizeof(*g));
}

/* A volume with no free block, and a pool's tail its deletes fill: more files
 * are given up than the tail's room and the deletes' share of the reserve —
 * one block past the tail (meta_pool.h) — hold Quitclaims of, and the files
 * hold no ground, so nothing their deletes free makes a Spare. Every delete
 * those hold stands on them alone: none is refused, and not one write reaches
 * the data run while they run — no Spare stamped, no cycle run for room. The
 * delete past them is not refused either, nor written: Glean's share is left
 * whole for the step that makes the room, and the delete waits for it —
 * ERR_WOULD_BLOCK, the word for room on its way (glean.h, the sweep). Red
 * before: a Quitclaim took the whole reserve, Glean had no Spare to take a
 * block off with, and every delete after the 98th was refused for good. */
static TestResult TestStandInkFullVolumeStillDeletes(void)
{
    TestResult     verdict = TEST_PASS;
    TagFSState*    fs      = tagfs_get_state();
    StandInkGround ground  = { NULL, NULL, 0, 0 };
    uint32_t       refused = 0, first_error = 0, writes = 0, spares = 0, holds = 0;
    int            past    = 0;
    bool           armed   = false;
    const char*    aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT(StandInkMake(STAND_INK_MOST),
                "more files are made than a tail and the deletes' share hold Quitclaims of");
    TEST_ASSERT(StandInkAtRest(),
                "cycles settle them, and nothing rests or waits to be gleaned");
    TEST_ASSERT_EQ(StandInkTakeAll(&ground), ERR_TAGFS_NO_SPACE,
                   "every free block of the volume is taken, and it says it has none");
    TagFSLockTake(&fs->lock);
    holds  = PoolTailRoom() / RECORD_HEADER_SIZE + SPARE_FOR_QUITCLAIMS * POOL_QUITCLAIMS_PER_BLOCK;
    spares = PoolSparesHeld(NULL, 0);
    TagFSLockDrop(&fs->lock);
    TEST_ASSERT_EQ(spares, SPARE_RESERVE, "the pool holds the whole reserve");

    BlockFailArm(tagfs_get_seat(), BLOCK_FAIL_WRITE, tagfs_block_to_sector(0),
                 (uint64_t)fs->layout.data_blocks * TAGFS_BLOCK_SECTORS, BLOCK_FAIL_NEVER);
    armed = true;
    for (uint32_t i = 0; i < holds; i++) {
        int gone = tagfs_delete_file(s_ink_fids[i], INK_UNWARRANTED);
        if (gone == 0) {
            s_ink_fids[i] = 0;
        } else {
            if (!refused)
                first_error = (uint32_t)-gone;
            refused++;
        }
    }
    past   = tagfs_delete_file(s_ink_fids[holds], INK_UNWARRANTED);
    writes = BlockFailSeen();
    BlockFailDisarm();
    armed  = false;
    spares = StandInkSpares(NULL, 0);
    StandInkGiveBack(&ground);

    TestEvidence("deletes refused, and writes to the data run made for them, on a volume with "
                 "no free block", (uint64_t)refused + writes);
    TestEvidence("the delete past the deletes' share, answered ERR_WOULD_BLOCK (1)",
                 past == -(int)ERR_WOULD_BLOCK);
    if (refused || writes || past != -(int)ERR_WOULD_BLOCK || spares != SPARE_FOR_GLEAN) {
        kprintf("[TESTS] %u of %u delete(s) refused, the first answering error %u; %u write(s) "
                "to the data run while they ran; the delete past them answered %d, and the pool "
                "was left %u Spare(s), where Glean's share is %u\n", refused, holds, first_error,
                writes, past, spares, (unsigned)SPARE_FOR_GLEAN);
        verdict = TEST_DEFECT;
    }
out:
    if (armed)
        BlockFailDisarm();
    StandInkGiveBack(&ground);
    StandInkLetGo(STAND_INK_MOST);
    return verdict;
}

/*
 * A tail a cycle closes early for a waiter leaves Glean its share of the
 * reserve. On a volume with no free block and nothing at rest the reserve
 * cannot be made again: the first anchored change closes the tail onto the
 * deletes' Spare, the next finds only Glean's and keeps its tail open — said
 * once — and Glean's share is whole for the step that takes a block off. An
 * early close that took it would leave a volume whose deletes free no ground
 * with nothing to make room by, and every delete after refused (E1); one that
 * took the last would leave the deletes' Quitclaims in a tail nothing can
 * close, and the ground they free resting for good (Z09-1).
 */
static TestResult TestStandInkEarlyCloseSparesTheLast(void)
{
    TestResult     verdict = TEST_PASS;
    StandInkGround ground  = { NULL, NULL, 0, 0 };
    uint64_t       file_id = 0;
    const char*    aside   = StandStepsAsideFor();
    TagSpelling    key     = TagSpellingOfKey("stand_ink");

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT_EQ(tagfs_create_file("test_file", NULL, 0, &file_id), 0, "a file is made");
    TEST_ASSERT_OK(tagfs_add_tag_string(file_id, &key, INK_UNWARRANTED), "it wears stand_ink");
    uint32_t tag = TagRegistryLookup(tagfs_get_state()->registry, &key);
    TEST_ASSERT(StandInkAtRest(), "cycles settle it, and nothing rests or waits to be gleaned");
    TEST_ASSERT_EQ(StandInkSpares(NULL, 0), SPARE_RESERVE, "the pool holds the whole reserve");
    TEST_ASSERT_EQ(StandInkTakeAll(&ground), ERR_TAGFS_NO_SPACE,
                   "every free block of the volume is taken, and it says it has none");

    TEST_ASSERT_EQ(tagfs_remove_tag(file_id, tag, INK_UNWARRANTED), 0,
                   "stand_ink comes off — a change, with no block to make a Spare from");
    uint64_t first = InkedGen();
    TEST_ASSERT_OK(TagFSSettle(), "a cycle with a waiter settles it");
    TEST_ASSERT(PoolClosedSettledGen() >= first, "in a closed block: the tail closed onto a Spare");
    TEST_ASSERT_EQ(StandInkSpares(NULL, 0), SPARE_FOR_GLEAN,
                   "and no Spare could be made again: Glean's share is what is left");

    TEST_ASSERT_OK(tagfs_add_tag_string(file_id, &key, INK_UNWARRANTED),
                   "stand_ink goes on again, a tag the medium already holds — a second change");
    uint64_t second = InkedGen();
    TEST_ASSERT_OK(TagFSSettle(), "the next cycle with a waiter settles it too");
    TEST_ASSERT(PoolClosedSettledGen() < second, "but not in a closed block");
    TEST_ASSERT(PoolTailGen() >= second, "it lies in the tail the cycle kept open");
    TEST_ASSERT_EQ(StandInkSpares(NULL, 0), SPARE_FOR_GLEAN,
                   "and not one Spare of Glean's share was taken");
out:
    StandInkGiveBack(&ground);
    if (file_id)
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    (void)TagFSSettle();
    return verdict;
}

/*
 * A Spare is taken in memory and named on the medium only by the link that
 * closes onto it, so until then every sector of the map goes out leaving it
 * free: a volume cut by the power finds no stamped block nobody owns. The
 * creates fill the tail, and it turns onto a Spare with no cycle between —
 * its link is the next cycle's to write. The map's debts paid by the door a
 * give pays them through, the medium calls every Spare and the tail they
 * turned onto free, memory calls them taken; and the next cycle, whose S3
 * writes the link, writes the tail's block taken at its S1 — a link never
 * lands on a block the medium calls free.
 */
static TestResult TestStandInkSparesFreeOnTheMedium(void)
{
    TestResult  verdict = TEST_PASS;
    TagFSState* fs      = tagfs_get_state();
    uint32_t    spares[SPARE_RESERVE + 2u];
    uint32_t    held = 0, tail = 0, wrong = 0;
    bool        taken = true;
    RecordWhere first_at, last_at;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT_OK(TagFSSettle(), "a cycle settles what the tests before left, and empties the tail");
    TEST_ASSERT(StandInkMake(STAND_INK_DELETES), "more files are made than a tail holds records of");
    FileRecord* rec = FileMapRecordHold(fs->files, s_ink_fids[0], &first_at);
    TEST_ASSERT(rec != NULL, "the first file's record is found");
    FileRecordRelease(rec);
    rec = FileMapRecordHold(fs->files, s_ink_fids[STAND_INK_DELETES - 1u], &last_at);
    TEST_ASSERT(rec != NULL, "and the last's");
    FileRecordRelease(rec);
    TEST_ASSERT(first_at.link != last_at.link, "the tail turned onto a Spare between them");
    TagFSLockTake(&fs->lock);
    bool linked = PoolLinkBlock(last_at.link, &tail);
    TagFSLockDrop(&fs->lock);
    TEST_ASSERT(linked, "the tail's block is found");

    BlockMapSettle();
    held = StandInkSpares(spares, ARRAY_SIZE(spares));
    TEST_ASSERT(held > 0 && held <= ARRAY_SIZE(spares), "the pool holds Spares, every one listed");
    for (uint32_t i = 0; i < held; i++)
        wrong += !BlockMapIsTaken(spares[i]) ||
                 !BlockMapMediumSays(spares[i], &taken) || taken;
    TestEvidence("Spares the medium's map calls taken, or memory free, of those held", wrong);
    TEST_ASSERT_EQ(wrong, 0, "the medium calls every Spare free, and memory every one taken");
    TEST_ASSERT(BlockMapIsTaken(tail), "memory calls the tail they turned onto taken");
    TEST_ASSERT(BlockMapMediumSays(tail, &taken) && !taken,
                "and the medium free: no cycle has written the link naming it");

    TEST_ASSERT_OK(TagFSSettle(), "the cycle that writes the link settles");
    TEST_ASSERT(BlockMapMediumSays(tail, &taken) && taken,
                "and its map, written before the link, calls the tail's block taken");
out:
    StandInkLetGo(STAND_INK_DELETES);
    (void)TagFSSettle();
    return verdict;
}

// ----------------------------------------------------------------------------
// A deleted file's newest record is its Quitclaim
// ----------------------------------------------------------------------------

/* The newest record of `fid` in the pool as the medium holds it, walked from
 * the head the Deed names the way a reader that knows no seal walks it: false
 * when the medium holds none; *active whether it is an ACTIVE record rather
 * than a Quitclaim. */
static bool StandInkNewestOnMedium(uint64_t fid, bool* active, bool* whole)
{
    TagFSState*    fs    = tagfs_get_state();
    MetaPoolBlock* blk   = (MetaPoolBlock*)g_test_output;
    uint32_t       block = fs->layout.metadata_pool_block - fs->layout.data_block;
    bool           found = false;

    *whole = true;
    for (uint32_t hops = 0; hops < fs->layout.data_blocks; hops++) {
        if (TagFSMetaBlockRead(block, blk) != OK || blk->magic != TAGFS_MPOOL_MAGIC) {
            *whole = false;
            return found;
        }
        uint32_t used = blk->used_bytes <= TAGFS_MPOOL_DATA_SIZE ? blk->used_bytes
                                                                 : TAGFS_MPOOL_DATA_SIZE;
        for (uint32_t pos = 0; pos + RECORD_HEADER_SIZE <= used; ) {
            uint16_t len;
            uint32_t id;
            memcpy(&len, blk->payload + pos, sizeof(len));
            if (len < RECORD_HEADER_SIZE || len > used - pos)
                break;
            memcpy(&id, blk->payload + pos + 2, sizeof(id));
            if (id == fid) {
                TagFSMetadata meta;
                memset(&meta, 0, sizeof(meta));
                if (UnpackV1(blk->payload + pos, len, tagfs_get_state()->layout.data_blocks,
                             &meta, NULL) == OK) {
                    found   = true;
                    *active = (meta.flags & TAGFS_FILE_ACTIVE) != 0;
                }
                tagfs_metadata_free(&meta);
            }
            pos += len;
        }
        if (blk->next_block == 0)
            break;
        block = blk->next_block;
    }
    return found;
}

/* A one-block file, settled, written again and given up: once its delete has
 * settled, the newest record the medium holds for it is its Quitclaim and its
 * row is gone from the table on the medium, so no mount finds it again (the
 * resurrection D7 made of a write committed after its file's retire). Its
 * block rests in Fallow through the cycle that settled the delete — the
 * Ledger's other copy still stands on the cycle before — and goes back with
 * the last of the FallowLag() cycles after it: one, or two on a volume whose
 * medium once lied (fallow.h). */
static TestResult TestStandInkQuitclaimIsTheNewestRecord(void)
{
    TestResult     verdict = TEST_PASS;
    uint64_t       file_id = 0;
    uint64_t       block   = 0;
    bool           active  = true, whole = true;
    FileTableEntry row     = { 0, 0 };
    const char*    aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TestFillAt(g_test_buffer, TAGFS_BLOCK_SIZE, 0x71, 0);
    TEST_ASSERT(StandMakeFile("test_file", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a one-block file is made");
    TEST_ASSERT(StandBlockOf(file_id, 0, &block), "its block is found");
    TEST_ASSERT_OK(TagFSSettle(), "and settled");
    TEST_ASSERT_EQ(StandWriteAt(file_id, 0, g_test_buffer, 64), 64, "it is written again");
    TEST_ASSERT_EQ(tagfs_delete_file(file_id, INK_UNWARRANTED), 0, "and given up");
    uint64_t gone = file_id;
    file_id = 0;
    TEST_ASSERT_OK(TagFSSettle(), "the delete is settled");

    bool held = StandInkNewestOnMedium(gone, &active, &whole);
    TEST_ASSERT(whole, "the pool's chain on the medium walks whole");
    TEST_ASSERT(held, "the medium holds a record of the deleted file");
    bool row_gone = !StandTableRowOnMedium(gone, &row, NULL) ||
                    (row.meta_block == 0 && row.meta_offset == 0);

    TestEvidence("deleted files the medium still names by an ACTIVE newest record or a row",
                 (uint64_t)(active || !row_gone));
    if (active || !row_gone) {
        verdict = TEST_DEFECT;
        goto out;
    }
    TEST_ASSERT(StandBlockInUse(block), "its block rests through the cycle that settled the delete");
    for (uint64_t more = 1; more < FallowLag(); more++) {
        TEST_ASSERT_OK(TagFSSettle(), "a cycle more of Fallow's lag");
        TEST_ASSERT(StandBlockInUse(block), "and the block still rests");
    }
    TEST_ASSERT_OK(TagFSSettle(), "the lag's last cycle");
    TEST_ASSERT(!StandBlockInUse(block), "and the block goes back with it");
out:
    if (file_id)
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    return verdict;
}

/* Whether the file's record, as memory holds it, is called `name`. */
static bool StandInkCalled(uint64_t file_id, const char* name)
{
    if (!tagfs_enter())
        return false;
    FileRecord* rec    = FileMapRecordHold(tagfs_get_state()->files, file_id, NULL);
    bool        called = rec && strcmp(rec->name, name) == 0;
    FileRecordRelease(rec);
    tagfs_leave();
    return called;
}

/* A name wider than a record of format v1 holds, given to a one-block file:
 * the answer is the record's lack of room, as format v1 always gave it —
 * judged on the extents the record names before its runs are laid flat for
 * the packer — so the one allocation before it is the list of tags the file
 * would wear. HEAPFAIL armed on the call's second allocation never fires, and
 * the file keeps its name. */
static TestResult TestStandInkNoRoomBeforeMemory(void)
{
    TestResult  verdict = TEST_PASS;
    uint64_t    file_id = 0;
    bool        asked   = false;
    char*       name    = NULL;
    InkReceipt  receipt;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    name = kmalloc(TAGFS_MPOOL_DATA_SIZE + 1u);
    TEST_ASSERT(name != NULL, "room for the name");
    memset(name, 'q', TAGFS_MPOOL_DATA_SIZE);
    name[0]                     = '.';    /* no stem: the name wears no tag */
    name[TAGFS_MPOOL_DATA_SIZE] = '\0';
    TestFillAt(g_test_buffer, TAGFS_BLOCK_SIZE, 0x72, 0);
    TEST_ASSERT(StandMakeFile("test_file", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a one-block file is made");

    HeapFailArm(amp_get_core_index(), 2);
    error_t rc = InkFields(file_id, name, TAGFS_INVALID_TAG_ID, TAGFS_INVALID_TAG_ID,
                           INK_UNWARRANTED, &receipt);
    asked = HeapFailFired();
    HeapFailDisarm();
    TEST_ASSERT_EQ(rc, ERR_TAGFS_RECORD_FULL, "a name wider than a record holds, the heap "
                   "refusing the second allocation, is refused for the record's room");
    TEST_ASSERT(!asked, "before more than the list of its tags was allocated");
    TEST_ASSERT(StandInkCalled(file_id, "test_file"), "and the file keeps its name");
out:
    HeapFailDisarm();
    kfree(name);
    if (file_id)
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    return verdict;
}

// ============================================================================
// The suite
// ============================================================================

static const TestCase g_stand_ink_tests[] = {
    { .name  = "stand_ink_records_never_share_a_place", .func = TestStandInkRecordsNeverSharePlace,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "writers of different files commit their records without the volume's "
                       "state lock, so two files' records are laid at one offset of the pool",
      .format = DEED_FORMAT_V1, .analog = "stand_parity_files_in_turn" },
    { .name  = "stand_ink_full_volume_still_deletes", .func = TestStandInkFullVolumeStillDeletes,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a volume with no free block and a full pool tail refuses every delete, "
                       "so nothing can make room on it",
      .format = DEED_FORMAT_V1, .analog = "stand_fresh_ground_fill_and_empty" },
    { .name  = "stand_ink_early_close_spares_the_last",
      .func  = TestStandInkEarlyCloseSparesTheLast, .reach = TEST_WRITES_TO_VOLUME,
      .format = DEED_FORMAT_V1, .analog = "stand_slip_holder_engrosses" },
    { .name  = "stand_ink_spares_free_on_the_medium",
      .func  = TestStandInkSparesFreeOnTheMedium, .reach = TEST_WRITES_TO_VOLUME,
      .format = DEED_FORMAT_V1, .analog = "stand_slip_logger" },
    { .name  = "stand_ink_quitclaim_is_the_newest_record",
      .func  = TestStandInkQuitclaimIsTheNewestRecord, .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a file written and then deleted is named again by the next mount, its "
                       "write's record newer on the medium than its delete",
      .format = DEED_FORMAT_V1, .analog = "stand_docket_ground_rests" },
    { .name  = "stand_ink_no_room_before_memory",
      .func  = TestStandInkNoRoomBeforeMemory, .reach = TEST_WRITES_TO_VOLUME,
      .format = DEED_FORMAT_V1, .analog = "stand_docket_claim_refused" },
};

const TestSuite g_stand_ink_suite = {
    .tests = g_stand_ink_tests, .count = ARRAY_SIZE(g_stand_ink_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
