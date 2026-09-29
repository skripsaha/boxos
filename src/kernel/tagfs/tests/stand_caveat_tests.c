#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../cow/cow.h"
#include "../integrity/integrity.h"
#include "block_fail.h"
#include "amp.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// Known defects, reproduced on purpose
//
// Each test here walks one road a refusal takes through TagFS — a medium that
// says no (BLOCKFAIL) or a heap that says no (HEAPFAIL) — and says what it saw:
// the defect its caveat names, with the number that proves it, or the right
// behaviour, with the number that proves that. The runner judges (tests.c):
// a defect seen under its caveat is held, and a test that behaves before its
// step has landed has outlived its caveat and fails.
//
// ‼ A TEST WHOSE DEFECT IS A LEAK GIVES BACK WHAT LEAKED. The runner's residue
// guard is strict for every test, these included, and the next boot's roll
// call judges the volume too ("[TagFS FSCK] Bitmap mismatch"). So a leak is
// measured — counted by the guard's own tally, and named block by block — and
// then exactly those blocks are handed back. What the volume keeps from this
// suite is burned file ids and the chain blocks any writer lays down for good
// (tests.c, the residue guard) — and no tag: every file here is called
// test_file, the name the core suite has always used, because a create
// interns its name's stem for good.
// ============================================================================

#define STAND_SWEEP_MAX 64u     /* allocations a sweep may refuse, one per try */

// ----------------------------------------------------------------------------
// The read-modify-write whose read the medium refuses
// ----------------------------------------------------------------------------

#define STAND_RMW_AT    100u
#define STAND_RMW_BYTES 10u
#define STAND_RMW_WAS   0x5A
#define STAND_RMW_NEW   0xC3

/* Ten bytes written at offset 100 of a block of 0x5A, with the block's read
 * refused. The right answer is a refusal and the block as it was. What
 * tagfs_write_inside does instead is zero its buffer when the read fails,
 * copy the ten bytes in, write the whole block back and answer 10 — the other
 * 4086 bytes of the file are gone and the caller was told it worked.
 *
 * ‼ Nothing reads the block between its write and the armed one. read_block
 * answers from the read-ahead cache before it asks the medium, and a cached
 * copy would be served with the plan never met; the write that made the file
 * went through write_block, which drops the block from that cache, and the
 * block's number is read off the file's record, not off the block. The test
 * then asserts that the plan fired, so a read served from anywhere else fails
 * it rather than passing it. */
TestResult TestStandRmwReadRefused(void)
{
    TestResult  verdict = TEST_PASS;
    uint8_t     seat    = tagfs_get_seat();
    uint32_t    file_id = 0;
    uint32_t    block   = 0;
    bool        armed   = false;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, STAND_RMW_WAS);
    TEST_ASSERT(StandMakeFile("test_file", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a one-block file of 0x5A is made");
    TEST_ASSERT(StandBlockOf(file_id, 0, &block), "its block is found");

    fill_pattern(g_test_output, STAND_RMW_BYTES, STAND_RMW_NEW);
    BlockFailArm(seat, BLOCK_FAIL_READ, tagfs_block_to_sector(block), TAGFS_BLOCK_SECTORS, 1);
    armed = true;
    int wrote = StandWriteAt(file_id, STAND_RMW_AT, g_test_output, STAND_RMW_BYTES);
    bool fired = BlockFailFired();
    BlockFailDisarm();
    armed = false;
    TEST_ASSERT(fired, "the read-modify-write's read of the block was the read refused");

    TEST_ASSERT_EQ(TestFileRead(file_id, g_test_output, TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                   "the block reads back");
    uint32_t changed = 0;
    uint32_t zeroed  = 0;
    for (uint32_t i = 0; i < TAGFS_BLOCK_SIZE; i++) {
        if (i >= STAND_RMW_AT && i < STAND_RMW_AT + STAND_RMW_BYTES)
            continue;
        if (g_test_output[i] != STAND_RMW_WAS) {
            changed++;
            zeroed += (g_test_output[i] == 0);
        }
    }
    TestEvidence("bytes around the write that the medium now holds as zeros", zeroed);

    if (wrote == (int)STAND_RMW_BYTES && zeroed > 0 && zeroed == changed) {
        verdict = TEST_DEFECT;
        goto out;
    }
    TEST_ASSERT(wrote < 0, "a write whose read was refused is refused itself");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output, g_test_buffer, TAGFS_BLOCK_SIZE),
                   TAGFS_BLOCK_SIZE, "and leaves the block as it was");
out:
    if (armed)
        BlockFailDisarm();
    if (file_id)
        tagfs_delete_file(file_id);
    return verdict;
}

// ----------------------------------------------------------------------------
// A create with one of its allocations refused
// ----------------------------------------------------------------------------

static uint32_t s_found[256];

/* Does the name's row of the tag index hold this file id? Asked of the index
 * itself, so neither a Use Context nor the hidden/trashed filter can stand
 * between the question and the row. */
static bool StandNameFinds(uint16_t stem, uint32_t file_id)
{
    int n = tag_bitmap_query(tagfs_get_state()->bitmap_index, &stem, 1, NULL, 0,
                             s_found, ARRAY_SIZE(s_found));
    for (int i = 0; i < n; i++)
        if (s_found[i] == file_id)
            return true;
    return false;
}

static bool StandVolumeNames(uint32_t file_id)
{
    TagFSMetadata meta;

    memset(&meta, 0, sizeof(meta));
    if (tagfs_get_metadata(file_id, &meta) != 0)
        return false;
    tagfs_metadata_free(&meta);
    return true;
}

/* One create, allocation n refused, judged whole or not. A create is whole
 * when it either made the file — named by the volume, found by its name, and
 * after its delete found by nothing — or made nothing at all. The name's row
 * is put back when a deleted file lingers in it: nothing else would ever
 * clear that bit, and the next test would inherit a name that finds a file
 * that is not there.
 *
 * The file a create made is deleted on every path out, and by the id the
 * create answered — `id` is only the one it should have taken, and may be
 * another creator's. A refused create that left a record behind is failed and
 * the record stays: it has no table row (file_table_update is the step that
 * failed), so tagfs_delete_file cannot find it and nothing a test can call
 * retires it. */
static TestResult StandCreateOnce(uint32_t n, uint16_t stem, bool* fired, bool* whole)
{
    TestResult verdict = TEST_PASS;
    uint32_t   id      = tagfs_file_ceiling();      /* the id this create takes */
    uint32_t   made    = 0;
    uint32_t   owned   = 0;                         /* made, until it is deleted */
    bool       armed   = false;

    HeapFailArm(amp_get_core_index(), n);
    armed = true;
    int rc = tagfs_create_file("test_file", NULL, 0, &made);
    *fired = HeapFailFired();
    HeapFailDisarm();
    armed = false;

    bool named = StandVolumeNames(id);
    bool found = StandNameFinds(stem, id);
    if (rc != 0) {
        TEST_ASSERT(!named, "a refused create leaves no record behind");
        if (found)
            tag_bitmap_clear(tagfs_get_state()->bitmap_index, stem, id);
        *whole = !found;
        goto out;
    }

    owned = made;
    TEST_ASSERT_EQ(made, id, "a create takes the next id");
    TEST_ASSERT_EQ(tagfs_delete_file(made), 0, "the file a create made can be deleted");
    owned = 0;
    bool lingers = StandNameFinds(stem, made);
    if (lingers)
        tag_bitmap_clear(tagfs_get_state()->bitmap_index, stem, made);
    *whole = named && found && !lingers;
out:
    if (armed)
        HeapFailDisarm();
    if (owned)
        tagfs_delete_file(owned);
    return verdict;
}

TestResult TestStandCreateAllOrNothing(void)
{
    TestResult verdict   = TEST_PASS;
    uint32_t   file_id   = 0;
    uint32_t   refused   = 0;
    uint32_t   broken_at = 0;
    bool       swept     = false;

    /* The name's tag and its row are made, if no test before made them, by one
     * create before the sweep — the sweep measures a create, not the first
     * create of a name. Then the ids are brought to a multiple of eight: the
     * name's row widens by a byte for every eight ids (tag_bitmap.c), so a
     * create takes one allocation more or fewer by where its id falls, and the
     * ids the tests before this one spent differ from boot to boot. */
    do {
        TEST_ASSERT_EQ(tagfs_create_file("test_file", NULL, 0, &file_id), 0,
                       "a create before the sweep interns the name and aligns the ids");
        TEST_ASSERT_EQ(tagfs_delete_file(file_id), 0, "and its file is deleted");
        file_id = 0;
    } while (tagfs_file_ceiling() % 8 != 0);
    uint16_t stem = tagfs_tag_lookup("test_file");
    TEST_ASSERT(stem != TAGFS_INVALID_TAG_ID, "the name has a tag");

    for (uint32_t n = 1; n <= STAND_SWEEP_MAX; n++) {
        bool fired = false;
        bool whole = true;
        TestResult once = StandCreateOnce(n, stem, &fired, &whole);
        if (once != TEST_PASS) {
            verdict = once;
            goto out;
        }
        if (!fired) {
            swept = true;           /* this create needed fewer than n allocations */
            TEST_ASSERT(whole, "a create with nothing refused is whole");
            break;
        }
        refused++;
        if (!whole && broken_at == 0)
            broken_at = n;
    }
    TEST_ASSERT(swept, "the sweep reached a create that needed fewer allocations than it refused");

    if (broken_at) {
        TestEvidence("half-done create at refused allocation", broken_at);
        verdict = TEST_DEFECT;
        goto out;
    }
    TestEvidence("creates whole or refused whole, allocations refused", refused);
out:
    if (file_id)
        tagfs_delete_file(file_id);
    return verdict;
}

// ----------------------------------------------------------------------------
// A sync that meets a refusal
// ----------------------------------------------------------------------------

static struct {
    VolumeLedger led;
    char         tail[TAGFS_SECTOR_SIZE];
    uint16_t     tail_len;
} s_peek;

/* The newest intact Ledger on the medium, by its sequence number — read off
 * the medium by the Ledger's own reader. */
uint64_t StandLedgerSeq(void)
{
    uint64_t newest = 0;
    for (uint32_t c = 0; c < VOLUME_LEDGER_COPIES; c++) {
        if (tagfs_ledger_peek(c, &s_peek.led, s_peek.tail, &s_peek.tail_len) &&
            s_peek.led.seq > newest)
            newest = s_peek.led.seq;
    }
    return newest;
}

/* The file's row of the file table as the MEDIUM holds it: the table's chain
 * walked from its head, on the medium, to the block that carries the row —
 * tagfs_read_block reads it there, because the sync's write_block took the
 * block out of the read-ahead cache. False when the chain does not reach it. */
bool StandTableRowOnMedium(uint32_t file_id, FileTableEntry* out, uint32_t* out_block)
{
    TagFSState*     fs    = tagfs_get_state();
    FileTableBlock* fb    = (FileTableBlock*)g_test_output;
    uint32_t        block = fs->layout.file_table_block - fs->layout.data_block;

    for (uint32_t k = file_id / TAGFS_FTABLE_PER_BLOCK; ; k--) {
        if (tagfs_read_block(block, fb) != OK || fb->magic != TAGFS_FILETBL_MAGIC)
            return false;
        if (k == 0)
            break;
        if (fb->next_block == 0)
            return false;
        block = fb->next_block;
    }
    uint32_t row = file_id % TAGFS_FTABLE_PER_BLOCK;
    if (row >= fb->entry_count)
        return false;
    *out = fb->entries[row];
    if (out_block)
        *out_block = block;
    return true;
}

/* The block of the file table that carries a file's row is refused while a
 * sync writes it. The sync goes on to write the ledger over a table the medium
 * does not have, and answers OK; the right answer names the refusal and
 * writes no ledger.
 *
 * The row is put on the medium first and then made owed again by renaming the
 * file to its own name — a new record, so a new row, and no new tag — and the
 * plan is armed on the block the medium says carries it: a table that writes
 * only the rows it owes still writes that block, wherever the file's id falls.
 *
 * On the way out the table the medium refused is still owed to it, and that
 * is proven off the medium: an unarmed sync while the file stands, then the
 * file's row read back — the refused table never carried it, so the medium
 * holds it only if the second sync wrote the table. */
TestResult TestStandSyncOwnsARefusedWrite(void)
{
    TestResult     verdict   = TEST_PASS;
    uint8_t        seat      = tagfs_get_seat();
    uint32_t       file_id   = 0;
    uint32_t       row_block = 0;
    FileTableEntry row       = { 0, 0 };
    bool           armed     = false;
    const char*    aside     = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT_EQ(tagfs_create_file("test_file", NULL, 0, &file_id), 0,
                   "a file is created, so the table has a row to write");
    TEST_ASSERT_OK(tagfs_sync(), "an unarmed sync puts the file's row on the medium");
    TEST_ASSERT_EQ(tagfs_rename_file(file_id, "test_file"), 0,
                   "the file is renamed to its own name, so its row is owed again");
    TEST_ASSERT(StandTableRowOnMedium(file_id, &row, &row_block),
                "the block that carries the file's row is found on the medium");
    uint64_t seq_before = StandLedgerSeq();
    TEST_ASSERT(seq_before > 0, "a Ledger copy reads");

    BlockFailArm(seat, BLOCK_FAIL_WRITE, tagfs_block_to_sector(row_block), TAGFS_BLOCK_SECTORS, 1);
    armed = true;
    error_t synced = tagfs_sync();
    bool fired = BlockFailFired();
    BlockFailDisarm();
    armed = false;
    uint64_t seq_after = StandLedgerSeq();

    TEST_ASSERT(fired, "the block that carries the file's row was the write refused");
    TestEvidence("ledger copies written after the file table's write was refused",
                 seq_after - seq_before);
    if (synced == OK && seq_after > seq_before) {
        verdict = TEST_DEFECT;
        goto out;
    }
    TEST_ASSERT(synced != OK, "a sync whose table write was refused says so");
    TEST_ASSERT_EQ(seq_after, seq_before, "and writes no ledger over it");
out:
    if (armed)
        BlockFailDisarm();
    error_t again = tagfs_sync();
    if (again != OK) {
        verdict = TestFailedAt(__FILE__, __LINE__, "an unarmed sync afterwards is taken "
                               "(error, OK)", again, OK);
    } else if (file_id) {
        uint32_t       meta_block = 0, meta_offset = 0;
        FileTableEntry on_medium  = { 0, 0 };
        bool           read_back  = StandTableRowOnMedium(file_id, &on_medium, NULL);
        if (file_table_lookup(file_id, &meta_block, &meta_offset) != 0 || !read_back ||
            on_medium.meta_block != meta_block || on_medium.meta_offset != meta_offset)
            verdict = TestFailedAt(__FILE__, __LINE__, "an unarmed sync afterwards puts the "
                                   "table on the medium — the file's row, read back off it "
                                   "(on the medium, in memory)",
                                   read_back ? (int64_t)on_medium.meta_block : -1, meta_block);
    }
    if (file_id)
        tagfs_delete_file(file_id);
    return verdict;
}

/* The medium refuses the sync's cache flush. The sync prints that none of it
 * is proven durable — and answers OK. */
TestResult TestStandSyncOwnsARefusedFlush(void)
{
    TestResult verdict = TEST_PASS;
    bool       armed   = false;

    BlockFailArm(tagfs_get_seat(), BLOCK_FAIL_FLUSH, 0, 0, 1);
    armed = true;
    error_t synced = tagfs_sync();
    bool fired = BlockFailFired();
    uint32_t seen = BlockFailSeen();
    BlockFailDisarm();
    armed = false;

    TEST_ASSERT(fired, "the sync's cache flush was the command refused");
    if (synced == OK) {
        TestEvidence("cache flushes the medium refused under a sync that answered OK", seen);
        verdict = TEST_DEFECT;
        goto out;
    }
    TestEvidence("a refused cache flush, and the sync answered it with error", synced);
out:
    if (armed)
        BlockFailDisarm();
    error_t flushed = tagfs_flush_cache();
    if (flushed != OK)
        verdict = TestFailedAt(__FILE__, __LINE__, "an unarmed flush afterwards is taken "
                               "(error, OK)", flushed, OK);
    return verdict;
}

// ----------------------------------------------------------------------------
// A write with one of its allocations refused
// ----------------------------------------------------------------------------

typedef struct {
    uint32_t refused;       /* writes swept, each with one allocation refused */
    uint32_t leaked;        /* blocks refused writes kept, named by no file */
    uint32_t short_at;      /* the first answered neither whole nor refused */
} StandWriteSweep;

static uint32_t s_kept[64];

/* The blocks the handle's extents name that the file's record does not — what
 * a write that allocated and then could not commit is left holding. */
static uint32_t StandKeptUnnamed(const TagFSFileHandle* handle, uint32_t file_id)
{
    TagFSMetadata meta;
    uint32_t      kept = 0;

    memset(&meta, 0, sizeof(meta));
    bool have = tagfs_get_metadata(file_id, &meta) == 0;
    for (uint16_t e = 0; handle->extents && e < handle->extent_count; e++) {
        for (uint32_t k = 0; k < handle->extents[e].block_count; k++) {
            uint32_t block = handle->extents[e].start_block + k;
            bool     named = false;
            for (uint16_t r = 0; have && meta.extents && r < meta.extent_count && !named; r++) {
                uint32_t from = meta.extents[r].start_block;
                named = block >= from && block - from < meta.extents[r].block_count;
            }
            if (!named && kept < ARRAY_SIZE(s_kept))
                s_kept[kept++] = block;
        }
    }
    if (have)
        tagfs_metadata_free(&meta);
    return kept;
}

/* One block written into a fresh file with allocation n refused, the file
 * closed and deleted, and whatever the write kept that no file names counted
 * by the residue guard's own tally, matched block for block against what the
 * handle named and the record did not, and given back — whether or not the
 * delete went through: what the record does not name is no file's either
 * way, and tagfs_close commits nothing of the handle's to the record. */
static TestResult StandWriteOnce(uint32_t n, StandWriteSweep* sweep, bool* fired)
{
    TestResult       verdict = TEST_PASS;
    uint32_t         file_id = 0;
    TagFSFileHandle* handle  = NULL;
    bool             armed   = false;
    VolumeTally      before;
    VolumeTally      after;

    memset(&before, 0, sizeof(before));
    memset(&after, 0, sizeof(after));
    TEST_ASSERT_EQ(tagfs_create_file("test_file", NULL, 0, &file_id), 0,
                   "a file to write into is created");
    handle = tagfs_open(file_id, TAGFS_HANDLE_READ | TAGFS_HANDLE_WRITE);
    TEST_ASSERT(handle != NULL, "and opened");
    TEST_ASSERT(VolumeTallyTake(&before), "the volume is counted before the write");

    TestFillAt(g_test_buffer, TAGFS_BLOCK_SIZE, 0x5C, 0);
    HeapFailArm(amp_get_core_index(), n);
    armed = true;
    int wrote = tagfs_write(handle, g_test_buffer, TAGFS_BLOCK_SIZE);
    *fired = HeapFailFired();
    HeapFailDisarm();
    armed = false;

    uint32_t kept = StandKeptUnnamed(handle, file_id);
    tagfs_close(handle);
    handle = NULL;
    int deleted = tagfs_delete_file(file_id);
    if (deleted == 0)
        file_id = 0;

    /* Counted before anything is given back, and given back before anything
     * is asserted: a count that does not match is a failure, and still not a
     * reason to leave the blocks behind. */
    uint32_t still = 0;
    for (uint32_t i = 0; i < kept; i++)
        still += StandBlockInUse(s_kept[i]);
    bool    counted = VolumeTallyTake(&after);
    int64_t left    = (int64_t)(after.held - before.held);
    for (uint32_t i = 0; i < kept; i++)
        if (StandBlockInUse(s_kept[i]))
            tagfs_free_blocks(s_kept[i], 1);
    TEST_ASSERT_EQ(deleted, 0, "the file is deleted");
    TEST_ASSERT(counted, "the volume is counted after the write");
    TEST_ASSERT_EQ(left, still, "what the file left in use is exactly what its handle named "
                                "and its record did not");
    TEST_ASSERT(VolumeTallyTake(&after), "the volume is counted once those are given back");
    TEST_ASSERT_EQ(after.held, before.held, "and it holds what it held before the write");

    if (!*fired) {
        TEST_ASSERT_EQ(wrote, TAGFS_BLOCK_SIZE, "a write with nothing refused is whole");
        goto out;
    }
    sweep->refused++;
    sweep->leaked += still;
    if (wrote >= 0 && wrote != (int)TAGFS_BLOCK_SIZE && !sweep->short_at)
        sweep->short_at = n;
out:
    if (armed)
        HeapFailDisarm();
    if (handle)
        tagfs_close(handle);
    if (file_id)
        tagfs_delete_file(file_id);
    return verdict;
}

static TestResult StandWriteSweepRun(StandWriteSweep* sweep)
{
    TestResult  verdict = TEST_PASS;
    bool        swept   = false;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    memset(sweep, 0, sizeof(*sweep));
    for (uint32_t n = 1; n <= STAND_SWEEP_MAX && !swept; n++) {
        bool fired = false;
        TestResult once = StandWriteOnce(n, sweep, &fired);
        if (once != TEST_PASS)
            return once;
        swept = !fired;
    }
    TEST_ASSERT(swept, "the sweep reached a write that needed fewer allocations than it refused");
out:
    return verdict;
}

TestResult TestStandWriteReturnsItsGround(void)
{
    StandWriteSweep sweep;
    TestResult      swept = StandWriteSweepRun(&sweep);

    if (swept != TEST_PASS)
        return swept;
    TestEvidence("blocks refused writes kept named by no file, all given back", sweep.leaked);
    return sweep.leaked ? TEST_DEFECT : TEST_PASS;
}

TestResult TestStandWriteAllOrRefusal(void)
{
    StandWriteSweep sweep;
    TestResult      swept = StandWriteSweepRun(&sweep);

    if (swept != TEST_PASS)
        return swept;
    if (sweep.short_at) {
        TestEvidence("write answered short with no refusal at refused allocation", sweep.short_at);
        return TEST_DEFECT;
    }
    TestEvidence("writes whole or refused, allocations refused", sweep.refused);
    return TEST_PASS;
}

// ----------------------------------------------------------------------------
// An open with one of its allocations refused
//
// Never read through the handle: one that counts extents and holds none takes
// the kernel down on the first read (tagfs.c, tagfs_read_inside), so each
// handle is looked at and closed.
// ----------------------------------------------------------------------------

typedef struct {
    uint32_t refused;
    uint32_t hollow_at;     /* the first handle that counted extents and held none */
    uint32_t emptied_at;    /* the first that called a one-block file empty */
    uint32_t alone_at;      /* the first with no open-file entry */
} StandOpenSweep;

static TestResult StandOpenSweepRun(StandOpenSweep* sweep)
{
    TestResult  verdict = TEST_PASS;
    uint8_t     me      = amp_get_core_index();
    uint32_t    file_id = 0;
    bool        armed   = false;
    bool        swept   = false;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    memset(sweep, 0, sizeof(*sweep));
    TestFillAt(g_test_buffer, TAGFS_BLOCK_SIZE, 0x5E, 0);
    TEST_ASSERT(StandMakeFile("test_file", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a one-block file is made");

    for (uint32_t n = 1; n <= STAND_SWEEP_MAX && !swept; n++) {
        HeapFailArm(me, n);
        armed = true;
        TagFSFileHandle* handle = tagfs_open(file_id, TAGFS_HANDLE_READ | TAGFS_HANDLE_WRITE);
        bool fired = HeapFailFired();
        HeapFailDisarm();
        armed = false;

        bool hollow  = handle && handle->extent_count > 0 && !handle->extents;
        bool emptied = handle && handle->file_size != TAGFS_BLOCK_SIZE;
        bool alone   = handle && !handle->ofe;
        tagfs_close(handle);

        if (!fired) {
            swept = true;
            TEST_ASSERT(handle && !hollow && !emptied && !alone,
                        "an open with nothing refused is whole");
            break;
        }
        sweep->refused++;
        if (hollow && !sweep->hollow_at)
            sweep->hollow_at = n;
        if (emptied && !sweep->emptied_at)
            sweep->emptied_at = n;
        if (alone && !sweep->alone_at)
            sweep->alone_at = n;
    }
    TEST_ASSERT(swept, "the sweep reached an open that needed fewer allocations than it refused");
out:
    if (armed)
        HeapFailDisarm();
    if (file_id)
        tagfs_delete_file(file_id);
    return verdict;
}

TestResult TestStandOpenHoldsWhatItNames(void)
{
    StandOpenSweep sweep;
    TestResult     swept = StandOpenSweepRun(&sweep);

    if (swept != TEST_PASS)
        return swept;
    if (sweep.hollow_at) {
        TestEvidence("handle counting extents it did not hold at refused allocation",
                     sweep.hollow_at);
        return TEST_DEFECT;
    }
    TestEvidence("opens whole or refused, allocations refused", sweep.refused);
    return TEST_PASS;
}

TestResult TestStandOpenRefusesUnreadMetadata(void)
{
    StandOpenSweep sweep;
    TestResult     swept = StandOpenSweepRun(&sweep);

    if (swept != TEST_PASS)
        return swept;
    if (sweep.emptied_at) {
        TestEvidence("one-block file opened as empty at refused allocation", sweep.emptied_at);
        return TEST_DEFECT;
    }
    TestEvidence("opens whole or refused, allocations refused", sweep.refused);
    return TEST_PASS;
}

TestResult TestStandOpenJoinsTheWriters(void)
{
    StandOpenSweep sweep;
    TestResult     swept = StandOpenSweepRun(&sweep);

    if (swept != TEST_PASS)
        return swept;
    if (sweep.alone_at) {
        TestEvidence("handle with no open-file entry at refused allocation", sweep.alone_at);
        return TEST_DEFECT;
    }
    TestEvidence("opens whole or refused, allocations refused", sweep.refused);
    return TEST_PASS;
}

// ----------------------------------------------------------------------------
// A block a snapshot keeps, given back twice
//
// The state this stood for was the one a write left when it had copied a
// block for a snapshot and recorded the redirect, and then could not commit
// the file onto the copy: the snapshot kept the old block, and the file still
// named it. Deleting the file gave the block back, and deleting the snapshot
// gave it back again — free_run_locked does not look at the bit, so the block
// went on the free list a second time and was counted free a second time.
//
// A format-v1 volume never reaches that state now: the snapshot is refused,
// and the write lands on the block the file has. So the road is walked as it
// now is — a one-block file, a snapshot of it asked for and refused, its block
// written over whole, the file deleted — and the allocator is then asked how
// far its count drifted from its bitmap (TagFSAllocatorRederive, which also
// derives it again, so no drift outlives the test).
// ----------------------------------------------------------------------------

TestResult TestStandFrozenBlockFreedTwice(void)
{
    TestResult  verdict = TEST_PASS;
    uint32_t    file_id = 0;
    uint32_t    kept    = 0;
    uint32_t    now     = 0;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TestFillAt(g_test_buffer, TAGFS_BLOCK_SIZE, 0x5F, 0);
    TEST_ASSERT(StandMakeFile("test_file", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a one-block file is made");
    TEST_ASSERT(StandBlockOf(file_id, 0, &kept), "its block is found");
    TEST_ASSERT_EQ(TagFS_SnapshotRefused("a snapshot of the stand's one-block file"),
                   ERR_UNSUPPORTED, "a snapshot of the file is refused");

    TestFillAt(g_test_output, TAGFS_BLOCK_SIZE, 0x60, 0);
    TEST_ASSERT_EQ(TestFileWrite(file_id, g_test_output, TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                   "the file's block is written over whole");
    TEST_ASSERT(StandBlockOf(file_id, 0, &now), "and found again");
    TEST_ASSERT_EQ(now, kept, "where it was: nothing was copied for a snapshot");

    TEST_ASSERT_EQ(TagFSAllocatorRederive(), 0, "the allocator agrees with its bitmap before the delete");
    TEST_ASSERT_EQ(tagfs_delete_file(file_id), 0, "the file is deleted");
    file_id = 0;
    TEST_ASSERT(!StandBlockInUse(kept), "and its block is given back with it");

    int64_t drift = TagFSAllocatorRederive();
    TEST_ASSERT(drift >= 0, "the allocator counts no block taken that its bitmap calls free");
    TestEvidence("blocks the allocator was given back twice", (uint64_t)drift);
    if (drift)
        verdict = TEST_DEFECT;
out:
    if (file_id)
        tagfs_delete_file(file_id);
    return verdict;
}

// ----------------------------------------------------------------------------
// A block out of the read-ahead cache, verified as one off the medium is
// (CW-D8)
//
// A read of a file's block prefetches the rest of its extent into the
// read-ahead cache (ReadAheadPrefetch), and read_block handed those blocks
// out of the cache without verify-on-read: a block that rotted on the medium
// was caught only when it was the first of the run a read prefetched, and once
// extents merge a file is read in runs.
//
// A three-block extent is read once — its first block off the medium, the
// others prefetched — then its middle block rots on the medium behind the
// map's back and all three are dropped from the cache, and the file is read
// again: its first block off the medium, and its middle block out of the
// prefetch that read makes. The first read is what makes that so: a prefetch
// passes over the block the last one ended on, and the first read's ended on
// the third. The evidence is how many rotten blocks the second read caught, of
// the one there is.
// ----------------------------------------------------------------------------

#define STAND_RA_BLOCKS 3u

TestResult TestStandReadAheadHitIsVerified(void)
{
    TestResult  verdict  = TEST_PASS;
    TagFSState* fs       = tagfs_get_state();
    uint32_t    file_id  = 0;
    uint32_t    blocks[STAND_RA_BLOCKS] = { 0, 0, 0 };
    bool        rotten   = false;
    uint8_t*    was      = g_test_buffer;                                   /* 3 blocks */
    uint8_t*    rot      = g_test_buffer + STAND_RA_BLOCKS * TAGFS_BLOCK_SIZE;
    uint32_t    middle   = TAGFS_BLOCK_SIZE;
    const char* aside    = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);
    if (!IntegrityIsInitialized())
        return test_steps_aside("verify-on-read is not armed on this volume, so no rot "
                                "can be caught on either road");

    TestFillAt(was, STAND_RA_BLOCKS * TAGFS_BLOCK_SIZE, 0x6A, 0);
    TEST_ASSERT(StandMakeFile("test_file", was, STAND_RA_BLOCKS * TAGFS_BLOCK_SIZE, &file_id),
                "a three-block file is made");
    TEST_ASSERT_EQ(tagfs_defrag_file(file_id, 0), 0, "and gathered into one extent");
    for (uint32_t k = 0; k < STAND_RA_BLOCKS; k++)
        TEST_ASSERT(StandBlockOf(file_id, k, &blocks[k]), "each of its blocks is found");
    TEST_ASSERT(blocks[1] == blocks[0] + 1 && blocks[2] == blocks[0] + 2,
                "one run on the medium, so a read of its first block prefetches the others");

    TEST_ASSERT_EQ(TestFileRead(file_id, g_test_output, STAND_RA_BLOCKS * TAGFS_BLOCK_SIZE),
                   STAND_RA_BLOCKS * TAGFS_BLOCK_SIZE, "the file is read once, whole");

    memcpy(rot, was + middle, TAGFS_BLOCK_SIZE);
    rot[100] ^= 0xFF;
    uint64_t vlba = ((uint64_t)fs->layout.data_block + blocks[1]) * TAGFS_BLOCK_SECTORS;
    TEST_ASSERT_EQ(tagfs_volume_write(vlba, TAGFS_BLOCK_SECTORS, rot), 0,
                   "its middle block rots on the medium, behind the map's back");
    rotten = true;
    for (uint32_t k = 0; k < STAND_RA_BLOCKS; k++)
        tagfs_readahead_invalidate(blocks[k]);

    IntegrityDrainReports();
    uint32_t errors_before = IntegrityErrorCount();
    TEST_ASSERT_EQ(TestFileRead(file_id, g_test_output, STAND_RA_BLOCKS * TAGFS_BLOCK_SIZE),
                   STAND_RA_BLOCKS * TAGFS_BLOCK_SIZE, "the file is read again, whole");
    uint32_t caught = IntegrityErrorCount() - errors_before;
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output + middle, rot, TAGFS_BLOCK_SIZE),
                   TAGFS_BLOCK_SIZE, "its middle block reads as the medium now holds it");
    TEST_ASSERT(caught <= 1, "and the one rotten block is caught at most once");

    TestEvidence("rotten blocks a read caught out of the read-ahead cache, of 1", caught);
    if (caught == 0)
        verdict = TEST_DEFECT;
out:
    if (rotten) {
        /* The block and the map agree again before the file lets it go, so
         * nobody who takes the block next is told of rot it never had. */
        if (tagfs_write_block(blocks[1], was + middle) != OK)
            verdict = TestFailedAt(__FILE__, __LINE__,
                                   "the rotten block would not take its bytes back — "
                                   "the map and the medium disagree over it (block, 0)",
                                   blocks[1], 0);
        IntegrityDrainReports();
    }
    if (file_id)
        tagfs_delete_file(file_id);
    return verdict;
}

#endif /* CONFIG_VOLUME_TESTS */
