#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../cow/cow.h"
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
// suite is burned file ids, and nothing else.
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
    TEST_ASSERT(StandMakeFile("stand_rmw.tst", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
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

#define STAND_RT17_NAME "stand_rt17.tst"
#define STAND_RT17_STEM "stand_rt17"

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
 * that is not there. */
static TestResult StandCreateOnce(uint32_t n, uint16_t stem, bool* fired, bool* whole)
{
    TestResult verdict = TEST_PASS;
    uint32_t   id      = tagfs_file_ceiling();      /* the id this create takes */
    uint32_t   made    = 0;
    bool       armed   = false;

    HeapFailArm(amp_get_core_index(), n);
    armed = true;
    int rc = tagfs_create_file(STAND_RT17_NAME, NULL, 0, &made);
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

    TEST_ASSERT_EQ(made, id, "a create takes the next id");
    TEST_ASSERT_EQ(tagfs_delete_file(made), 0, "the file a create made can be deleted");
    bool lingers = StandNameFinds(stem, made);
    if (lingers)
        tag_bitmap_clear(tagfs_get_state()->bitmap_index, stem, made);
    *whole = named && found && !lingers;
out:
    if (armed)
        HeapFailDisarm();
    return verdict;
}

TestResult TestStandCreateAllOrNothing(void)
{
    TestResult verdict   = TEST_PASS;
    uint32_t   file_id   = 0;
    uint32_t   refused   = 0;
    uint32_t   broken_at = 0;
    bool       swept     = false;

    /* The name's tag is interned, and its row made, by one create before the
     * sweep — the sweep measures a create, not the first create of a name. */
    TEST_ASSERT_EQ(tagfs_create_file(STAND_RT17_NAME, NULL, 0, &file_id), 0,
                   "a create before the sweep interns the name");
    TEST_ASSERT_EQ(tagfs_delete_file(file_id), 0, "and its file is deleted");
    file_id = 0;
    uint16_t stem = tagfs_tag_lookup(STAND_RT17_STEM);
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
static uint64_t StandLedgerSeq(void)
{
    uint64_t newest = 0;
    for (uint32_t c = 0; c < VOLUME_LEDGER_COPIES; c++) {
        if (tagfs_ledger_peek(c, &s_peek.led, s_peek.tail, &s_peek.tail_len) &&
            s_peek.led.seq > newest)
            newest = s_peek.led.seq;
    }
    return newest;
}

/* The file table's head block is refused while a sync writes it. The sync
 * goes on to write the ledger over a table the medium does not have, and
 * answers OK; the right answer names the refusal and writes no ledger. */
TestResult TestStandSyncOwnsARefusedWrite(void)
{
    TestResult  verdict = TEST_PASS;
    uint8_t     seat    = tagfs_get_seat();
    uint32_t    file_id = 0;
    bool        armed   = false;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TagFSState* fs = tagfs_get_state();
    TEST_ASSERT_EQ(tagfs_create_file("stand_sync.tst", NULL, 0, &file_id), 0,
                   "a file is created, so the table has a row to write");
    uint64_t seq_before = StandLedgerSeq();
    TEST_ASSERT(seq_before > 0, "a Ledger copy reads");

    uint64_t head = tagfs_get_volume_base() +
                    (uint64_t)fs->layout.file_table_block * TAGFS_BLOCK_SECTORS;
    BlockFailArm(seat, BLOCK_FAIL_WRITE, head, TAGFS_BLOCK_SECTORS, 1);
    armed = true;
    error_t synced = tagfs_sync();
    bool fired = BlockFailFired();
    BlockFailDisarm();
    armed = false;
    uint64_t seq_after = StandLedgerSeq();

    TEST_ASSERT(fired, "the file table's head block was the write refused");
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
    if (file_id)
        tagfs_delete_file(file_id);
    /* The table the medium refused is still owed to it. */
    error_t again = tagfs_sync();
    if (again != OK)
        verdict = TestFailedAt(__FILE__, __LINE__, "an unarmed sync afterwards puts the "
                               "table on the medium (error, OK)", again, OK);
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
    uint32_t leak_at;       /* the first refused allocation that leaked */
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
 * handle named and the record did not, and given back. */
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
    TEST_ASSERT_EQ(tagfs_create_file("stand_sweep.tst", NULL, 0, &file_id), 0,
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
    TEST_ASSERT_EQ(tagfs_delete_file(file_id), 0, "the file is deleted");
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
    if (still && !sweep->leak_at)
        sweep->leak_at = n;
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
    TEST_ASSERT(StandMakeFile("stand_open.tst", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
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
// The state is the one a write leaves when it has copied a block for a
// snapshot and recorded the redirect, and then could not commit the file onto
// the copy — the medium refused the copy's write, or the record could not be
// made (tagfs_write_inside): the snapshot keeps the old block, and the file
// still names it. It is staged here with the copy-on-write calls themselves,
// as the cow suite stages it. Then the file is deleted, which gives the block
// back, and the snapshot is deleted, which gives it back again —
// free_run_locked does not look at the bit, so the block is put on the free
// list a second time and counted free a second time. That is measured as the
// count's drift from the bitmap, and the allocator is then derived again from
// the bitmap (TagFSAllocatorRederive) so no block is left for two owners.
// ----------------------------------------------------------------------------

TestResult TestStandFrozenBlockFreedTwice(void)
{
    TestResult  verdict = TEST_PASS;
    uint32_t    file_id = 0;
    uint32_t    snap_id = 0;
    uint32_t    frozen  = 0;
    uint32_t    copy    = 0;
    bool        staged  = false;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TestFillAt(g_test_buffer, TAGFS_BLOCK_SIZE, 0x5F, 0);
    TEST_ASSERT(StandMakeFile("stand_frozen.tst", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a one-block file is made");
    TEST_ASSERT(StandBlockOf(file_id, 0, &frozen), "its block is found");

    error_t made = TagFS_SnapshotCreate("stand-frozen", file_id, &snap_id);
    if (made == ERR_UNSUPPORTED) {
        snap_id = 0;        /* no snapshot can keep a block, so none is given back twice */
        TestEvidence("blocks the allocator was given back twice", 0);
        goto out;
    }
    if (made != OK)
        snap_id = 0;        /* ERR_ALREADY_EXISTS names somebody else's snapshot */
    TEST_ASSERT_OK(made, "a snapshot of the file is taken");
    staged = true;

    TEST_ASSERT_OK(TagFS_CowBeforeWrite(file_id, frozen, &copy), "the file's block is copied");
    TEST_ASSERT_OK(TagFS_CowAfterWrite(file_id, frozen, copy),
                   "and the snapshot keeps the block, while the file still names it");

    TEST_ASSERT_EQ(TagFSAllocatorRederive(), 0, "the allocator agrees with its bitmap before the deletes");
    TEST_ASSERT_EQ(tagfs_delete_file(file_id), 0, "the file is deleted");
    file_id = 0;
    TEST_ASSERT(!StandBlockInUse(frozen), "and the block the snapshot keeps is given back with it");

    TEST_ASSERT_OK(TagFS_SnapshotDelete(snap_id), "the snapshot is deleted");
    snap_id = 0;
    int64_t drift = TagFSAllocatorRederive();
    TestEvidence("blocks the allocator was given back twice", (uint64_t)(drift > 0 ? drift : 0));
    if (drift == 0)
        goto out;
    TEST_ASSERT_EQ(drift, 1, "the one block the snapshot kept, and no other");
    verdict = TEST_DEFECT;
out:
    if (file_id)
        tagfs_delete_file(file_id);
    if (snap_id)
        TagFS_SnapshotDelete(snap_id);
    if (staged) {
        /* Whatever was given back twice on the way out is derived away, and
         * the copy — which the deleted snapshot named and nothing else — goes
         * back once. */
        TagFSAllocatorRederive();
        if (copy && StandBlockInUse(copy))
            tagfs_free_blocks(copy, 1);
    }
    return verdict;
}

#endif /* CONFIG_VOLUME_TESTS */
