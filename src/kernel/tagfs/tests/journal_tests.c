#include "tests.h"
#include "../tagfs.h"
#include "../disk_book/disk_book.h"

// ============================================================================
// Journal (DiskBook) — the CoW redirect log is open, and a checkpoint lands
// ============================================================================

static TestResult test_diskbook_init(void)
{
    TestResult verdict = TEST_PASS;
    TEST_ASSERT(DiskBookIsInitialized(), "DiskBook should be initialized");
out:
    return verdict;
}

static TestResult test_diskbook_checkpoint(void)
{
    TestResult verdict = TEST_PASS;
    TEST_ASSERT_OK(DiskBookCheckpoint(), "DiskBookCheckpoint should succeed");
out:
    return verdict;
}

static const TestCase g_journal_tests[] = {
    { .name = "diskbook_init",       .func = test_diskbook_init,       .reach = TEST_READS_ONLY },
    { .name = "diskbook_checkpoint", .func = test_diskbook_checkpoint, .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_journal_suite = {
    .tests = g_journal_tests, .count = ARRAY_SIZE(g_journal_tests),
};
