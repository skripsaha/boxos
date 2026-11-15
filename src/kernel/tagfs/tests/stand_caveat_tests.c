#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "block_fail.h"
#include "amp.h"
#include "../settle/settle.h"
#include "../ink/fallow.h"

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
// ‼ A LEAK IS MEASURED BY THE RESIDUE GUARD'S OWN TALLY. The runner's guard is
// strict for every test, these included, and the next boot's roll call judges
// the volume too ("[TagFS FSCK] Bitmap mismatch"). What the volume keeps from
// this suite is burned file ids and the chain blocks any writer lays down for
// good (tests.c, the residue guard) — and no tag: every file here is called
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
 * refused. The right answer is a refusal and the block as it was (S1): a
 * write that zeroed its buffer when the read failed, copied the ten bytes in,
 * wrote the whole block back and answered 10 took the other 4086 bytes of the
 * file and told the caller it worked.
 *
 * ‼ Nothing reads the block between its write and the armed one. The write's
 * read of it goes to the medium — no read-ahead window of a handle serves a
 * read_block — and the block's number is read off the file's record, not off
 * the block. The test then asserts that the plan fired, so a read served from
 * anywhere else fails it rather than passing it. */
TestResult TestStandRmwReadRefused(void)
{
    TestResult  verdict = TEST_PASS;
    uint8_t     seat    = tagfs_get_seat();
    uint64_t    file_id = 0;
    uint64_t    block   = 0;
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
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    return verdict;
}

// ----------------------------------------------------------------------------
// A create with one of its allocations refused
// ----------------------------------------------------------------------------

/* Does the tag index count this file id among the name's? Asked of the index
 * itself, so neither a Use Context nor a tag a question sets aside can
 * stand between the question and the answer. */
static bool StandNameFinds(uint32_t stem, uint64_t file_id)
{
    return TagBitmapHas(tagfs_get_state()->bitmap_index, stem, file_id);
}

static bool StandVolumeNames(uint64_t file_id)
{
    TagFSMetadata meta;

    memset(&meta, 0, sizeof(meta));
    if (tagfs_get_metadata(file_id, &meta) != 0)
        return false;
    tagfs_metadata_free(&meta);
    return true;
}

/* The file's id out from under the name's tag, the way an Ink takes it out:
 * under the volume's state lock, under which alone the index changes shape. */
static void StandNameLetGo(uint32_t stem, uint64_t id)
{
    TagFSState* fs = tagfs_get_state();
    TagFSLockTake(&fs->lock);
    TagBitmapLeave(fs->bitmap_index, stem, id);
    TagFSLockDrop(&fs->lock);
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
 * another creator's. A refused create that left a record behind is failed. */
static TestResult StandCreateOnce(uint32_t n, uint32_t stem, bool* fired, bool* whole)
{
    TestResult verdict = TEST_PASS;
    uint64_t   id      = tagfs_file_ceiling();      /* the id this create takes */
    uint64_t   made    = 0;
    uint64_t   owned   = 0;                         /* made, until it is deleted */
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
            StandNameLetGo(stem, id);
        *whole = !found;
        goto out;
    }

    owned = made;
    TEST_ASSERT_EQ(made, id, "a create takes the next id");
    TEST_ASSERT_EQ(tagfs_delete_file(made, INK_UNWARRANTED), 0, "the file a create made can be deleted");
    owned = 0;
    bool lingers = StandNameFinds(stem, made);
    if (lingers)
        StandNameLetGo(stem, made);
    *whole = named && found && !lingers;
out:
    if (armed)
        HeapFailDisarm();
    if (owned)
        tagfs_delete_file(owned, INK_UNWARRANTED);
    return verdict;
}

TestResult TestStandCreateAllOrNothing(void)
{
    TestResult verdict   = TEST_PASS;
    uint64_t   file_id   = 0;
    uint32_t   refused   = 0;
    uint32_t   broken_at = 0;
    bool       swept     = false;

    /* The name's tag is made, if no test before made it, by one create before
     * the sweep — the sweep measures a create, not the first create of a name.
     * That create also leaves the index and the map of live files as every
     * create of the sweep finds them: a leaf of the map that was full is split
     * by it and stays split, and what the name's members need for one more
     * file is taken and given back by each create alike (tag_bitmap.c,
     * file_map/). */
    TEST_ASSERT_EQ(tagfs_create_file("test_file", NULL, 0, &file_id), 0,
                   "a create before the sweep interns the name");
    TEST_ASSERT_EQ(tagfs_delete_file(file_id, INK_UNWARRANTED), 0, "and its file is deleted");
    file_id = 0;
    uint32_t stem = tagfs_tag_lookup("test_file");
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
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    return verdict;
}

// ----------------------------------------------------------------------------
// A write with one of its allocations refused
// ----------------------------------------------------------------------------

typedef struct {
    uint32_t refused;       /* writes swept, each with one allocation refused */
    uint32_t leaked;        /* blocks refused writes left in use, named by no file */
    uint32_t short_at;      /* the first answered neither whole nor refused */
} StandWriteSweep;

/* One block written into a fresh file with allocation n refused, the file
 * closed and deleted, and the volume counted by the residue guard's own tally
 * before the write and after the delete. What a write takes and does not
 * commit it gives back on its way out (CR-1), so a file that is deleted
 * leaves nothing of any write in use. A block it did leave is named by no
 * record and cannot be found to be given back: the sweep counts it, and the
 * runner's guard fails the test for it as well. */
static TestResult StandWriteOnce(uint32_t n, StandWriteSweep* sweep, bool* fired)
{
    TestResult       verdict = TEST_PASS;
    uint64_t         file_id = 0;
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
    int wrote = TestHandleWrite(handle, g_test_buffer, TAGFS_BLOCK_SIZE);
    *fired = HeapFailFired();
    HeapFailDisarm();
    armed = false;

    tagfs_close(handle);
    handle = NULL;
    TEST_ASSERT_EQ(tagfs_delete_file(file_id, INK_UNWARRANTED), 0, "the file is deleted");
    file_id = 0;
    TEST_ASSERT(VolumeTallyTake(&after), "the volume is counted after the delete");
    TEST_ASSERT(after.held >= before.held,
                "the delete gave back no block that was not in use before the write");
    uint64_t left = after.held - before.held;

    if (!*fired) {
        TEST_ASSERT_EQ(wrote, TAGFS_BLOCK_SIZE, "a write with nothing refused is whole");
        TEST_ASSERT_EQ(left, 0, "and its file, deleted, leaves nothing in use");
        goto out;
    }
    sweep->refused++;
    sweep->leaked += (uint32_t)left;
    if (wrote >= 0 && wrote != (int)TAGFS_BLOCK_SIZE && !sweep->short_at)
        sweep->short_at = n;
out:
    if (armed)
        HeapFailDisarm();
    if (handle)
        tagfs_close(handle);
    if (file_id)
        tagfs_delete_file(file_id, INK_UNWARRANTED);
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
    TestEvidence("blocks refused writes left in use, named by no file", sweep.leaked);
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
// Never read through the handle: one opened to read that holds no record of
// its file — it used to count extents and hold none, and took the kernel down
// on the first read — would read a file of nothing, so each handle is looked
// at and closed.
// ----------------------------------------------------------------------------

typedef struct {
    uint32_t refused;
    uint32_t hollow_at;     /* the first handle opened to read that held no record */
    uint32_t emptied_at;    /* the first that called a one-block file empty */
    uint32_t alone_at;      /* the first with no open-file entry */
} StandOpenSweep;

static TestResult StandOpenSweepRun(StandOpenSweep* sweep)
{
    TestResult  verdict = TEST_PASS;
    uint8_t     me      = amp_get_core_index();
    uint64_t    file_id = 0;
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

        bool hollow  = handle && !handle->rec;
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
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    return verdict;
}

TestResult TestStandOpenHoldsWhatItNames(void)
{
    StandOpenSweep sweep;
    TestResult     swept = StandOpenSweepRun(&sweep);

    if (swept != TEST_PASS)
        return swept;
    if (sweep.hollow_at) {
        TestEvidence("handle opened to read holding no record at refused allocation",
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

#endif /* CONFIG_VOLUME_TESTS */
