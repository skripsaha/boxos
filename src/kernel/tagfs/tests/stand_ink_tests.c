#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../file_map/file_map.h"
#include "../metadata_pool/meta_pool.h"
#include "../plot/plot.h"
#include "../settle/settle.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// Ink and the pool it lays records in (step 1.4)
//
// Every change of a file is one Ink: its record laid in the pool's tail under
// the volume's state lock, the record of the same file no cycle has taken yet
// taken back and the ones after it moved down, a delete a Quitclaim appended
// like any record, and a tail with no room closed onto a Spare — the last two
// Spares a delete's alone. These tests ask the pool's bookkeeping what the
// medium and the next mount will be told: where each file's record sits, what
// the newest record of a deleted file is, and whether a volume with no free
// block can still give a file up.
// ============================================================================

#define STAND_INK_FILES    16u     /* files the interleaved writes go round */
#define STAND_INK_ROUNDS   8u      /* writes each of them takes */
#define STAND_INK_CHUNK    16u     /* bytes a write appends */
#define STAND_INK_DELETES  (POOL_QUITCLAIMS_PER_BLOCK + 2u)   /* one more than a tail holds */

static uint32_t s_ink_fids[STAND_INK_DELETES];

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
            tagfs_delete_file(s_ink_fids[i]);
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
    uint32_t    fids[64];
    uint32_t    n = 0, invalid = 0, from = 0;

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

typedef struct {
    uint32_t* at;
    uint32_t* n;
    uint32_t  count;
    uint32_t  cap;
} StandInkGround;

/* Every free block taken, run by run; false for no memory, with what was
 * taken so far still listed for StandInkGiveBack. */
static bool StandInkTakeAll(StandInkGround* g)
{
    uint32_t start = 0, got = 0;
    uint32_t all   = tagfs_get_state()->layout.data_blocks;

    while (TagFSTakeRun(all, 0, &start, &got) == OK) {
        if (g->count == g->cap) {
            uint32_t  cap = g->cap ? g->cap * 2u : 64u;
            uint32_t* at  = kmalloc(sizeof(uint32_t) * cap);
            uint32_t* n   = kmalloc(sizeof(uint32_t) * cap);
            if (!at || !n) {
                kfree(at);
                kfree(n);
                tagfs_free_blocks(start, got);
                return false;
            }
            if (g->count) {
                memcpy(at, g->at, sizeof(uint32_t) * g->count);
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
    return true;
}

static void StandInkGiveBack(StandInkGround* g)
{
    for (uint32_t i = 0; i < g->count; i++)
        tagfs_free_blocks(g->at[i], g->n[i]);
    kfree(g->at);
    kfree(g->n);
    memset(g, 0, sizeof(*g));
}

/* A volume with no free block, and a pool's tail that fills: one more file is
 * given up than an empty tail holds Quitclaims, so the tail must close onto a
 * Spare, and there is no block to make one from. The last Spares are a
 * delete's (SPARE_RESERVE): without them the delete that fills the tail waits
 * for a block no cycle can give back, and a full volume can free nothing. */
static TestResult TestStandInkFullVolumeStillDeletes(void)
{
    TestResult     verdict = TEST_PASS;
    StandInkGround ground  = { NULL, NULL, 0, 0 };
    uint32_t       refused = 0, first_error = 0;
    const char*    aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT(StandInkMake(STAND_INK_DELETES), "one file more than a tail holds Quitclaims is made");
    TEST_ASSERT_OK(TagFSSettle(), "a Settle cycle closes the tail they are in, and the next is empty");
    TEST_ASSERT(StandInkTakeAll(&ground), "every free block of the volume is taken");

    for (uint32_t i = 0; i < STAND_INK_DELETES; i++) {
        int gone = tagfs_delete_file(s_ink_fids[i]);
        if (gone == 0) {
            s_ink_fids[i] = 0;
        } else {
            if (!refused)
                first_error = (uint32_t)-gone;
            refused++;
        }
    }
    StandInkGiveBack(&ground);

    TestEvidence("deletes refused on a volume with no free block", refused);
    if (refused) {
        kprintf("[TESTS] the first delete refused answered error %u\n", first_error);
        verdict = TEST_DEFECT;
    }
out:
    StandInkGiveBack(&ground);
    StandInkLetGo(STAND_INK_DELETES);
    return verdict;
}

// ----------------------------------------------------------------------------
// A deleted file's newest record is its Quitclaim
// ----------------------------------------------------------------------------

/* The newest record of `fid` in the pool as the medium holds it, walked from
 * the head the Deed names the way a reader that knows no seal walks it: false
 * when the medium holds none; *active whether it is an ACTIVE record rather
 * than a Quitclaim. */
static bool StandInkNewestOnMedium(uint32_t fid, bool* active, bool* whole)
{
    TagFSState*    fs    = tagfs_get_state();
    MetaPoolBlock* blk   = (MetaPoolBlock*)g_test_output;
    uint32_t       block = fs->layout.metadata_pool_block - fs->layout.data_block;
    bool           found = false;

    *whole = true;
    for (uint32_t hops = 0; hops < fs->layout.data_blocks; hops++) {
        if (tagfs_read_block(block, blk) != OK || blk->magic != TAGFS_MPOOL_MAGIC) {
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
                if (UnpackV1(blk->payload + pos, len, &meta) == OK) {
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
 * the next. */
static TestResult TestStandInkQuitclaimIsTheNewestRecord(void)
{
    TestResult     verdict = TEST_PASS;
    uint32_t       file_id = 0, block = 0;
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
    TEST_ASSERT_EQ(tagfs_delete_file(file_id), 0, "and given up");
    uint32_t gone = file_id;
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
    TEST_ASSERT_OK(TagFSSettle(), "one cycle more");
    TEST_ASSERT(!StandBlockInUse(block), "and goes back with it");
out:
    if (file_id)
        tagfs_delete_file(file_id);
    return verdict;
}

// ============================================================================
// The suite
// ============================================================================

static const TestCase g_stand_ink_tests[] = {
    { .name  = "stand_ink_records_never_share_a_place", .func = TestStandInkRecordsNeverSharePlace,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "writers of different files commit their records without the volume's "
                       "state lock, so two files' records are laid at one offset of the pool" },
    { .name  = "stand_ink_full_volume_still_deletes", .func = TestStandInkFullVolumeStillDeletes,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a volume with no free block and a full pool tail refuses every delete, "
                       "so nothing can make room on it" },
    { .name  = "stand_ink_quitclaim_is_the_newest_record",
      .func  = TestStandInkQuitclaimIsTheNewestRecord, .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a file written and then deleted is named again by the next mount, its "
                       "write's record newer on the medium than its delete" },
};

const TestSuite g_stand_ink_suite = {
    .tests = g_stand_ink_tests, .count = ARRAY_SIZE(g_stand_ink_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
