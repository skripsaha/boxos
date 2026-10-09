#include "tests.h"
#include "../tagfs.h"

// ============================================================================
// Stress — many files, one large file
//
// A stress_concurrent_operations used to stand here. It was stress_many_files
// again under another name — the same ten files, written and read one after
// another on one core — with every result ignored, so it neither ran anything
// concurrently nor could it fail. It is gone rather than renamed: writers that
// really run at once are write_concurrent's, under tools/stress_matrix.sh, and
// a loop on the boot core cannot stand in for them.
// ============================================================================

#define STRESS_FILE_COUNT  10
#define STRESS_LARGE_BYTES (64u * 1024u)      /* sixteen blocks */

static uint64_t s_stress_ids[STRESS_FILE_COUNT];

/* Ten files, each written with bytes no other file holds, and each read back
 * whole. It used to ignore every open, write and read result, and wrote the
 * same bytes into all ten — a file handed another file's block would have
 * read back as a pass. */
static TestResult test_stress_many_files(void)
{
    TestResult verdict    = TEST_PASS;
    uint16_t   tag_ids[2] = {1, 2};
    char       name[32];

    memset(s_stress_ids, 0, sizeof(s_stress_ids));

    for (uint32_t i = 0; i < STRESS_FILE_COUNT; i++) {
        ksnprintf(name, sizeof(name), "stress_file_%u", i);
        TEST_ASSERT_EQ(tagfs_create_file(name, tag_ids, 2, &s_stress_ids[i]), 0,
                       "tagfs_create_file should succeed");
    }

    for (uint32_t i = 0; i < STRESS_FILE_COUNT; i++) {
        TestFillAt(g_test_buffer, TAGFS_BLOCK_SIZE, i + 1, 0);
        TEST_ASSERT_EQ(TestFileWrite(s_stress_ids[i], g_test_buffer, TAGFS_BLOCK_SIZE),
                       TAGFS_BLOCK_SIZE, "every file takes its block");
    }

    for (uint32_t i = 0; i < STRESS_FILE_COUNT; i++) {
        TestFillAt(g_test_buffer, TAGFS_BLOCK_SIZE, i + 1, 0);
        TEST_ASSERT_EQ(TestFileRead(s_stress_ids[i], g_test_output, 2 * TAGFS_BLOCK_SIZE),
                       TAGFS_BLOCK_SIZE, "every file reads back one block, and no more");
        uint32_t diff = TestFirstDifference(g_test_output, g_test_buffer, TAGFS_BLOCK_SIZE);
        TEST_ASSERT_EQ((uint64_t)i * TAGFS_BLOCK_SIZE + diff, (uint64_t)(i + 1) * TAGFS_BLOCK_SIZE,
                       "every file holds its own bytes (file number x 4096 + first "
                       "differing byte)");
    }
out:
    for (uint32_t i = 0; i < STRESS_FILE_COUNT; i++)
        if (s_stress_ids[i])
            tagfs_delete_file(s_stress_ids[i], INK_UNWARRANTED);
    return verdict;
}

/* Sixteen blocks, every one of them read back and compared — each with bytes
 * that depend on where in the file they are, so a block that comes back from
 * the wrong place is caught. It used to write 16 KiB of one repeated block and
 * check only the first. */
static TestResult test_stress_large_file(void)
{
    TestResult       verdict    = TEST_PASS;
    uint64_t         file_id    = 0;
    uint16_t         tag_ids[1] = {1};
    TagFSFileHandle* handle     = NULL;
    bool             have_meta  = false;
    TagFSMetadata    meta;

    TEST_ASSERT_EQ(tagfs_create_file("large_file", tag_ids, 1, &file_id), 0,
                   "tagfs_create_file should succeed");

    handle = tagfs_open(file_id, TAGFS_HANDLE_READ | TAGFS_HANDLE_WRITE);
    TEST_ASSERT(handle != NULL, "tagfs_open should succeed");
    for (uint32_t at = 0; at < STRESS_LARGE_BYTES; at += TAGFS_BLOCK_SIZE) {
        TestFillAt(g_test_buffer, TAGFS_BLOCK_SIZE, 0x1A, at);
        TEST_ASSERT_EQ(TestHandleWrite(handle, g_test_buffer, TAGFS_BLOCK_SIZE),
                       TAGFS_BLOCK_SIZE, "every block of the file is written");
    }
    tagfs_close(handle);
    handle = NULL;

    TEST_ASSERT_EQ(tagfs_get_metadata(file_id, &meta), 0, "the file's metadata reads");
    have_meta = true;
    uint32_t blocks = 0;
    for (uint32_t e = 0; e < meta.extent_count && meta.extents; e++)
        blocks += meta.extents[e].count;
    TEST_ASSERT_EQ(meta.size, STRESS_LARGE_BYTES, "the file is as long as what was written");
    TEST_ASSERT_EQ(blocks, STRESS_LARGE_BYTES / TAGFS_BLOCK_SIZE,
                   "and holds a block for every 4 KiB of it");

    handle = tagfs_open(file_id, TAGFS_HANDLE_READ);
    TEST_ASSERT(handle != NULL, "tagfs_open for read should succeed");
    for (uint32_t at = 0; at < STRESS_LARGE_BYTES; at += TAGFS_BLOCK_SIZE) {
        TestFillAt(g_test_buffer, TAGFS_BLOCK_SIZE, 0x1A, at);
        TEST_ASSERT_EQ(TestHandleRead(handle, g_test_output, TAGFS_BLOCK_SIZE),
                       TAGFS_BLOCK_SIZE, "every block reads");
        uint32_t diff = TestFirstDifference(g_test_output, g_test_buffer, TAGFS_BLOCK_SIZE);
        TEST_ASSERT_EQ(at + diff, at + TAGFS_BLOCK_SIZE,
                       "every block holds its own bytes (first differing byte of the file)");
    }
    TEST_ASSERT_EQ(TestHandleRead(handle, g_test_output, TAGFS_BLOCK_SIZE), 0,
                   "and the file ends where the writing ended");
out:
    if (have_meta)
        tagfs_metadata_free(&meta);
    if (handle)
        tagfs_close(handle);
    if (file_id)
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    return verdict;
}

static const TestCase g_stress_tests[] = {
    { .name = "stress_many_files",       .func = test_stress_many_files,       .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stress_large_file",       .func = test_stress_large_file,       .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stress_suite = {
    .tests = g_stress_tests, .count = ARRAY_SIZE(g_stress_tests),
};
