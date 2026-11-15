#include "tests.h"
#include "../tagfs.h"
#include "../braid/braid.h"
#include "boardroom.h"

// ============================================================================
// Braid
//
// Every test builds its own braid: there is no Braid of the machine for a test
// to leave standing. The volume's is the one its mount names, and a book's
// mount names none — so no read or write of it ever asks Braid, and nothing a
// test does here can send one of its blocks anywhere but its own medium.
// ============================================================================

#define BRAID_FAKE_DISK_BLOCKS 1000000u

/* Static, as a test's own braid: a BraidState is a lock, eight disks and a
 * hash context — no frame of a test carries it. Zeroed before each use. */
static BraidState s_braid;

static const uint8_t s_braid_seed[16] = { 'b', 'r', 'a', 'i', 'd', '-', 't', 'e',
                                          's', 't', '-', 's', 'e', 'e', 'd', 0 };

static TestResult test_braid_init(void)
{
    TestResult verdict = TEST_PASS;

    memset(&s_braid, 0, sizeof(s_braid));
    TEST_ASSERT(tagfs_get_state()->braid == NULL,
                "the mounted volume is assembled in no Braid, so none of its blocks is sent to one");
    TEST_ASSERT_OK(BraidInit(&s_braid, BraidModeMirror, s_braid_seed), "a braid of its own is begun");
    TEST_ASSERT_EQ(BraidInit(&s_braid, BraidModeMirror, s_braid_seed), ERR_ALREADY_INITIALIZED,
                   "and begun only once");
    TEST_ASSERT(!BraidIsHealthy(&s_braid), "with no disk it is not healthy");
out:
    BraidShutdown(&s_braid);
    return verdict;
}

/* What a braid counts moves only when a disk's membership or answering does:
 * a member added twice is refused, a disk taken out is not a failure, and a
 * member set to what it already is changes nothing. */
static TestResult test_braid_counts_disks(void)
{
    TestResult verdict = TEST_PASS;
    BraidStats stats;

    memset(&s_braid, 0, sizeof(s_braid));
    TEST_ASSERT_OK(BraidInit(&s_braid, BraidModeMirror, s_braid_seed), "a braid of its own is begun");

    TEST_ASSERT_OK(BraidAddDisk(&s_braid, 0, BRAID_FAKE_DISK_BLOCKS), "disk 0 joins");
    TEST_ASSERT_EQ(BraidAddDisk(&s_braid, 0, BRAID_FAKE_DISK_BLOCKS), ERR_ALREADY_INITIALIZED,
                   "and joining it again is refused");
    TEST_ASSERT_EQ(BraidGetActiveDiskCount(&s_braid), 1, "one disk answers");
    TEST_ASSERT(!BraidIsHealthy(&s_braid), "one is not a mirror");
    TEST_ASSERT_OK(BraidAddDisk(&s_braid, 1, BRAID_FAKE_DISK_BLOCKS), "disk 1 joins");
    TEST_ASSERT(BraidIsHealthy(&s_braid), "two are");

    TEST_ASSERT_OK(BraidSetDiskOnline(&s_braid, 1, false), "disk 1 stops answering");
    TEST_ASSERT_OK(BraidSetDiskOnline(&s_braid, 1, false), "and is told so twice");
    TEST_ASSERT_EQ(BraidGetActiveDiskCount(&s_braid), 1, "which leaves one answering, not zero");
    TEST_ASSERT_OK(BraidGetStats(&s_braid, &stats), "the braid's counts are read");
    TEST_ASSERT_EQ(stats.disk_failures, 1, "one failure, counted once");

    TEST_ASSERT_OK(BraidSetDiskOnline(&s_braid, 1, true), "disk 1 answers again");
    TEST_ASSERT_EQ(BraidGetActiveDiskCount(&s_braid), 2, "two answer");
    TEST_ASSERT_OK(BraidRemoveDisk(&s_braid, 1), "disk 1 is taken out");
    TEST_ASSERT_EQ(BraidRemoveDisk(&s_braid, 1), ERR_DEVICE_NOT_READY,
                   "and cannot be taken out twice");
    TEST_ASSERT_EQ(BraidSetDiskOnline(&s_braid, 1, true), ERR_DEVICE_NOT_READY,
                   "nor brought back online once it is no member");
    TEST_ASSERT_EQ(BraidGetActiveDiskCount(&s_braid), 1, "one answers");
    TEST_ASSERT_OK(BraidGetStats(&s_braid, &stats), "the braid's counts are read again");
    TEST_ASSERT_EQ(stats.disk_failures, 1, "and a disk taken out is no failure");
out:
    BraidShutdown(&s_braid);
    return verdict;
}

static const TestCase g_braid_tests[] = {
    { .name = "braid_init",             .func = test_braid_init,             .reach = TEST_READS_ONLY },
    { .name = "braid_counts_disks",     .func = test_braid_counts_disks,     .reach = TEST_READS_ONLY },
};

const TestSuite g_braid_suite = {
    .tests = g_braid_tests, .count = ARRAY_SIZE(g_braid_tests),
};
