#include "tests.h"
#include "../tagfs.h"
#include "../braid/braid.h"
#include "boardroom.h"

// ============================================================================
// Braid
//
// Every test builds its own braid: there is no Braid of the machine for a test
// to leave standing. The volume's is the one its mount assembled, and a v1
// volume has none — so no read or write of it ever asks Braid, and nothing a
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
                "the mounted v1 volume is assembled in no Braid, so none of its blocks is sent to one");
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

/*
 * A block through a braid of its own and back: a stripe whose one disk is the
 * volume's own seat, over a block the volume hands out. The bytes come back as
 * written, each command is counted once, and a copy that does not match the
 * checksum asked for is refused and counted. The block holds the same bytes
 * through the volume's own door first and after, so the integrity map's digest
 * of it is the medium's all along.
 */
static TestResult test_braid_writes_and_reads(void)
{
    TestResult verdict = TEST_PASS;
    uint64_t   blk     = 0;
    uint8_t    seat    = tagfs_get_seat();
    uint8_t*   wrote   = g_test_buffer;
    uint8_t*   got     = g_test_output;
    BraidStats stats;

    memset(&s_braid, 0, sizeof(s_braid));
    if (seat >= BRAID_MAX_DISKS)
        return test_steps_aside("the volume's seat is past the disks a braid can name");
    if (tagfs_alloc_blocks(1, &blk) != 0)
        return test_steps_aside("the volume would not hand out a block to write through a braid");

    TestFillAt(wrote, TAGFS_BLOCK_SIZE, 0xB7, 0);
    TEST_ASSERT_OK(tagfs_write_block(blk, wrote), "the block takes its bytes through the volume");

    TEST_ASSERT_OK(BraidInit(&s_braid, BraidModeStripe, s_braid_seed), "a stripe of its own is begun");
    TEST_ASSERT_OK(BraidAddDisk(&s_braid, seat, BRAID_FAKE_DISK_BLOCKS),
                   "with the volume's seat as its one disk");
    TEST_ASSERT(BraidIsHealthy(&s_braid), "one disk is a whole stripe");

    uint64_t sector = tagfs_block_to_sector(blk);
    TEST_ASSERT_OK(BraidWriteBlock(&s_braid, sector, wrote, NULL), "the braid writes the block");
    memset(got, 0, TAGFS_BLOCK_SIZE);
    TEST_ASSERT_OK(BraidReadBlock(&s_braid, sector, got, NULL), "and reads it back");
    TEST_ASSERT_EQ(TestFirstDifference(got, wrote, TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                   "as it was written");

    BoxHash right = BoxHashContent(wrote, TAGFS_BLOCK_SIZE, &s_braid.hash_ctx);
    TEST_ASSERT_OK(BraidReadBlock(&s_braid, sector, got, &right),
                   "a read asked for the checksum the block has is served");
    BoxHash wrong = right;
    wrong.bytes[0] ^= 0xFF;
    TEST_ASSERT_EQ(BraidReadBlock(&s_braid, sector, got, &wrong), ERR_CORRUPTED,
                   "and one asked for another checksum is refused");

    TEST_ASSERT_OK(BraidGetStats(&s_braid, &stats), "the braid's counts are read");
    TEST_ASSERT_EQ(stats.total_writes, 1, "one write, counted once");
    TEST_ASSERT_EQ(stats.total_reads, 2, "two reads served, each counted once");
    TEST_ASSERT_EQ(stats.checksum_errors, 1, "and the one refused counted as a checksum error");

    memset(got, 0, TAGFS_BLOCK_SIZE);
    TEST_ASSERT_OK(tagfs_read_block(blk, got), "the volume reads the block through its own door");
    TEST_ASSERT_EQ(TestFirstDifference(got, wrote, TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                   "and finds what the braid wrote, matching its digest");
out:
    BraidShutdown(&s_braid);
    if (blk)
        tagfs_free_blocks(blk, 1);
    return verdict;
}

static const TestCase g_braid_tests[] = {
    { .name = "braid_init",             .func = test_braid_init,             .reach = TEST_READS_ONLY },
    { .name = "braid_counts_disks",     .func = test_braid_counts_disks,     .reach = TEST_READS_ONLY },
    { .name = "braid_writes_and_reads", .func = test_braid_writes_and_reads, .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_braid_suite = {
    .tests = g_braid_tests, .count = ARRAY_SIZE(g_braid_tests),
};
