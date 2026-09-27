#include "tests.h"
#include "../tagfs.h"
#include "../braid/braid.h"

// ============================================================================
// Braid
//
// ‼ A BRAID WITH ENOUGH DISKS IS HEALTHY, AND A HEALTHY BRAID TAKES EVERY
// BLOCK. read_block and write_block (tagfs.c) route through Braid whenever
// BraidIsHealthy() says so, so a pair of fake disks left behind by a test
// sends every later read and write of the volume to media that do not exist.
// These tests are READS_ONLY — they run on every machine that boots — so the
// disks they add come out on every path.
// ============================================================================

#define BRAID_FAKE_DISK_BLOCKS 1000000u

static TestResult test_braid_init(void)
{
    TestResult verdict = TEST_PASS;
    TEST_ASSERT_OK(BraidInit(BraidModeMirror), "BraidInit should succeed");
out:
    return verdict;
}

static TestResult test_braid_add_disk(void)
{
    TestResult verdict = TEST_PASS;
    bool       added[2] = { false, false };

    TEST_ASSERT_EQ(BraidGetActiveDiskCount(), 0,
                   "no disk is in the braid before the test adds its own");
    TEST_ASSERT_OK(BraidAddDisk(0, BRAID_FAKE_DISK_BLOCKS), "BraidAddDisk should succeed");
    added[0] = true;
    TEST_ASSERT_OK(BraidAddDisk(1, BRAID_FAKE_DISK_BLOCKS),
                   "BraidAddDisk second disk should succeed");
    added[1] = true;

    TEST_ASSERT(BraidIsHealthy(), "Braid should be healthy with 2 disks");
    TEST_ASSERT_EQ(BraidGetActiveDiskCount(), 2, "Should have 2 active disks");
out:
    for (uint8_t d = 0; d < 2; d++)
        if (added[d])
            BraidRemoveDisk(d);
    if ((added[0] || added[1]) && BraidIsHealthy())
        verdict = TestFailedAt(__FILE__, __LINE__,
                               "the braid is still healthy after the test's disks "
                               "came out, so every block of the volume goes to it "
                               "(active disks, wanted)",
                               BraidGetActiveDiskCount(), 0);
    return verdict;
}

static TestResult test_braid_write_read(void)
{
    /* Nothing is examined here on ANY machine: this test was never given a way
     * to attach the two media a braid is written across, so it steps aside on
     * the board exactly as it does in QEMU. Said out loud because the count
     * above would otherwise read as coverage. */
    return test_steps_aside("it has no way to attach the second medium a braid "
                            "is written across, so it stands down on every "
                            "machine and Braid's write-then-read path is "
                            "covered by nothing");
}

static const TestCase g_braid_tests[] = {
    { .name = "braid_init",       .func = test_braid_init,       .reach = TEST_READS_ONLY },
    { .name = "braid_add_disk",   .func = test_braid_add_disk,   .reach = TEST_READS_ONLY },
    { .name = "braid_write_read", .func = test_braid_write_read, .reach = TEST_READS_ONLY },
};

const TestSuite g_braid_suite = {
    .name = "braid", .tests = g_braid_tests, .count = ARRAY_SIZE(g_braid_tests),
};
