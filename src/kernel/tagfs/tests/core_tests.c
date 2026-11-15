#include "tests.h"
#include "../tagfs.h"

// ============================================================================
// Core — the mount itself, and one file through create, write, read, delete
// ============================================================================

static TestResult test_tagfs_create_file(void)
{
    TestResult verdict = TEST_PASS;
    uint64_t   file_id = 0;
    uint32_t   tag_ids[2] = {1, 2};

    TEST_ASSERT_EQ(tagfs_create_file("test_file", tag_ids, 2, &file_id), 0,
                   "tagfs_create_file should succeed");
    TEST_ASSERT(file_id > 0, "File ID should be > 0");
out:
    if (file_id)
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    return verdict;
}

static TestResult test_tagfs_write_read(void)
{
    TestResult verdict = TEST_PASS;
    uint64_t   file_id = 0;
    uint32_t   tag_ids[1] = {1};

    TEST_ASSERT_EQ(tagfs_create_file("test_rw", tag_ids, 1, &file_id), 0,
                   "tagfs_create_file should succeed");

    fill_random(g_test_buffer, TAGFS_BLOCK_SIZE);
    TEST_ASSERT_EQ(TestFileWrite(file_id, g_test_buffer, TAGFS_BLOCK_SIZE),
                   TAGFS_BLOCK_SIZE, "Should write 4096 bytes");

    /* Asked for more than was written: a file that grew past what it was
     * given answers with more than one block. */
    memset(g_test_output, 0, 2 * TAGFS_BLOCK_SIZE);
    TEST_ASSERT_EQ(TestFileRead(file_id, g_test_output, 2 * TAGFS_BLOCK_SIZE),
                   TAGFS_BLOCK_SIZE, "Should read 4096 bytes, and no more");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output, g_test_buffer, TAGFS_BLOCK_SIZE),
                   TAGFS_BLOCK_SIZE, "Data should match (first differing byte)");
out:
    if (file_id)
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    return verdict;
}

static const TestCase g_core_tests[] = {
    { .name = "tagfs_create_file", .func = test_tagfs_create_file, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "tagfs_write_read",  .func = test_tagfs_write_read,  .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_core_suite = {
    .tests = g_core_tests, .count = ARRAY_SIZE(g_core_tests),
};
