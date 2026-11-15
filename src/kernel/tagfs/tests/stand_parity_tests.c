#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../settle/settle.h"
#include "../settle/sewing.h"
#include "stand_stitch.h"
#include "../lock_rank/lock_rank.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_parity suite: properties of the write core, each held here on the
// book.
// ============================================================================

/*
 * A book's cycle crosses the medium holding no TagFS lock: a core that waits
 * on a lock held across the medium spins with its interrupts off for as long
 * as the medium takes. What it writes is a file's data, by its writer, and a stitch with
 * its flush, by the cycle — so a file is made and written, and its cycle
 * settled: this core's count of doors to the medium passed holding a TagFS lock
 * (LockRankCountHere) is the answer, and the larger seq the ribbon's writer
 * gave proves the cycle wrote at all.
 */
static TestResult TestStandParityStitchHoldsNoLock(void)
{
    TestResult    verdict = TEST_PASS;
    uint64_t      file_id = 0;
    RibbonCursor  was, now;
    LockRankCount before, after;

    fill_pattern(g_test_buffer, TAGFS_BLOCK_SIZE, 0x5Au);
    TEST_ASSERT_OK(TagFSSettle(), "what the tests before owe is settled first");
    RibbonCursorStand(&was);
    before = LockRankCountHere();
    TEST_ASSERT(StandMakeFile("stand_parity_lock", g_test_buffer, TAGFS_BLOCK_SIZE, &file_id),
                "a file is made and written");
    error_t settled = TagFSSettle();
    after = LockRankCountHere();
    RibbonCursorStand(&now);
    TEST_ASSERT_OK(settled, "its cycle settles");
    TEST_ASSERT(now.given > was.given, "and the ribbon's writer gave a stitch for it");
    TEST_ASSERT_EQ(after.Witnessed - before.Witnessed, 0,
                   "no door to the medium was passed holding a TagFS lock, by the write or by the cycle");
out:
    if (file_id) {
        (void)tagfs_delete_file(file_id, INK_UNWARRANTED);
        (void)TagFSSettle();
    }
    return verdict;
}

#define STAND_PARITY_FILES   16u    /* files the appends go round */
#define STAND_PARITY_ROUNDS  8u     /* appends each of them takes */
#define STAND_PARITY_CHUNK   16u    /* bytes an append lays */

/*
 * Files appended to in turn keep each its own bytes: two live files never
 * named by one record's place. A file's change is a record of the stitch its
 * cycle lays, and an append that leaves
 * its last block short rides the stitch as the file's Slip (§2.8) — so sixteen
 * files take eight appends each in turn, a cycle half way and one after, and
 * the volume is mounted again: what each file reads was replayed from the
 * ribbon, a record and a Slip a file, and is its own bytes, no other's.
 */
static TestResult TestStandParityFilesInTurn(void)
{
    TestResult  verdict = TEST_PASS;
    uint64_t    fid[STAND_PARITY_FILES] = { 0 };
    uint32_t    wrong = 0;
    uint32_t    bytes = STAND_PARITY_ROUNDS * STAND_PARITY_CHUNK;
    StandStitch s;
    const char* aside = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    for (uint32_t i = 0; i < STAND_PARITY_FILES; i++)
        TEST_ASSERT_EQ(tagfs_create_file("stand_parity_turn", NULL, 0, &fid[i]), 0, "sixteen files are made");
    for (uint32_t r = 0; r < STAND_PARITY_ROUNDS; r++) {
        if (r == STAND_PARITY_ROUNDS / 2)
            TEST_ASSERT_OK(TagFSSettle(), "a cycle half way settles what they took");
        for (uint32_t i = 0; i < STAND_PARITY_FILES; i++) {
            TestFillAt(g_test_buffer, STAND_PARITY_CHUNK, fid[i], (uint64_t)r * STAND_PARITY_CHUNK);
            TEST_ASSERT_EQ(StandWriteAt(fid[i], (uint64_t)r * STAND_PARITY_CHUNK, g_test_buffer, STAND_PARITY_CHUNK),
                           STAND_PARITY_CHUNK, "each file takes its append in turn");
        }
    }
    TEST_ASSERT_OK(TagFSSettle(), "and a cycle after the last");
    TEST_ASSERT(StandStitchRemount(), "the volume mounts again, the ribbon replayed");
    for (uint32_t i = 0; i < STAND_PARITY_FILES; i++) {
        TestFillAt(g_test_buffer, bytes, fid[i], 0);
        if (TestFileRead(fid[i], g_test_output, sizeof(g_test_output)) != (int)bytes ||
            TestFirstDifference(g_test_output, g_test_buffer, bytes) != bytes)
            wrong++;
    }
    TestEvidence("files that read other than their own appends", wrong);
    TEST_ASSERT_EQ(wrong, 0, "every file reads its own appends, and no other's");
out:
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

static const TestCase g_stand_parity_tests[] = {
    { .name  = "stand_parity_stitch_holds_no_lock", .func = TestStandParityStitchHoldsNoLock,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name  = "stand_parity_files_in_turn", .func = TestStandParityFilesInTurn,
      .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_parity_suite = {
    .tests = g_stand_parity_tests, .count = ARRAY_SIZE(g_stand_parity_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
