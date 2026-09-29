#include "tests.h"
#include "../tagfs.h"
#include "../disk_book/disk_book.h"

// ============================================================================
// Journal (DiskBook) — the redirect log an earlier kernel kept is open, for
// the replay of the snapshots it left
// ============================================================================

static TestResult test_diskbook_init(void)
{
    TestResult verdict = TEST_PASS;
    TEST_ASSERT(DiskBookIsInitialized(), "DiskBook should be initialized");
out:
    return verdict;
}

static const TestCase g_journal_tests[] = {
    { .name = "diskbook_init", .func = test_diskbook_init, .reach = TEST_READS_ONLY },
};

const TestSuite g_journal_suite = {
    .tests = g_journal_tests, .count = ARRAY_SIZE(g_journal_tests),
};
