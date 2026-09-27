#include "tests.h"
#include "../tagfs.h"

// ============================================================================
// Dedup index — does a block leave it when it leaves the allocator?
//
// It did not, until now: only truncate ever unregistered anything, so every
// deleted file left its blocks in the index for the lifetime of the system,
// each one mapping content that no longer exists to bytes handed to somebody
// else. These pin both halves of the fix — the entry goes when the block goes,
// and a block the index still counts as shared does NOT go.
// ============================================================================

#define DEDUP_ASIDE "the dedup index is not up on this volume, so there is no " \
                    "index for a block to be entered in or struck out of"

// Give a block content nothing else in the volume holds, so a hash lookup for
// it can only find this test's block.
static void dedup_stamp(uint8_t* buf, uint8_t fill, const char* marker)
{
    fill_pattern(buf, TAGFS_BLOCK_SIZE, fill);
    for (uint32_t i = 0; marker[i]; i++)
        buf[i] = (uint8_t)marker[i];
}

static TestResult test_dedup_delete_unregisters(void)
{
    TestResult verdict    = TEST_PASS;
    uint32_t   file_id    = 0;
    uint16_t   tag_ids[1] = {1};
    uint32_t   block      = 0;
    bool       is_dup     = false;
    DedupStats before, after;

    if (!TagFS_DedupIsInitialized())
        return test_steps_aside(DEDUP_ASIDE);

    TEST_ASSERT_OK(TagFS_DedupGetStats(&before), "stats before");

    dedup_stamp(g_test_buffer, 0xD1, "dedup-delete-unregisters");
    TEST_ASSERT_EQ(tagfs_create_file("dedup_del", tag_ids, 1, &file_id), 0,
                   "create should succeed");
    TEST_ASSERT_EQ(TestFileWrite(file_id, g_test_buffer, TAGFS_BLOCK_SIZE),
                   TAGFS_BLOCK_SIZE, "should write a whole block");

    TEST_ASSERT_OK(TagFS_DedupCheck(g_test_buffer, &block, &is_dup),
                   "the written block should be indexed");
    TEST_ASSERT(is_dup && block != 0, "and found by its content");

    uint32_t deleting = file_id;
    file_id = 0;
    TEST_ASSERT_EQ(tagfs_delete_file(deleting), 0, "delete should succeed");

    is_dup = false;
    TagFS_DedupCheck(g_test_buffer, &block, &is_dup);
    TEST_ASSERT(!is_dup, "delete must take the block out of the index");

    TEST_ASSERT_OK(TagFS_DedupGetStats(&after), "stats after");
    TEST_ASSERT_EQ(after.unique_blocks, before.unique_blocks,
                   "a create/write/delete cycle must not grow the index");
out:
    if (file_id)
        tagfs_delete_file(file_id);
    return verdict;
}

static TestResult test_dedup_truncate_unregisters(void)
{
    TestResult verdict    = TEST_PASS;
    uint32_t   file_id    = 0;
    uint16_t   tag_ids[1] = {1};
    uint32_t   block      = 0;
    bool       is_dup     = false;

    if (!TagFS_DedupIsInitialized())
        return test_steps_aside(DEDUP_ASIDE);

    dedup_stamp(g_test_buffer, 0xD2, "dedup-truncate-unregisters");
    TEST_ASSERT_EQ(tagfs_create_file("dedup_trunc", tag_ids, 1, &file_id), 0,
                   "create should succeed");
    TEST_ASSERT_EQ(TestFileWrite(file_id, g_test_buffer, TAGFS_BLOCK_SIZE),
                   TAGFS_BLOCK_SIZE, "should write a whole block");

    TagFS_DedupCheck(g_test_buffer, &block, &is_dup);
    TEST_ASSERT(is_dup, "the written block should be indexed");

    TEST_ASSERT_EQ(tagfs_truncate_file(file_id, 0), 0, "truncate to empty");

    is_dup = false;
    TagFS_DedupCheck(g_test_buffer, &block, &is_dup);
    TEST_ASSERT(!is_dup, "a truncated-away block must leave the index too");
out:
    if (file_id)
        tagfs_delete_file(file_id);
    return verdict;
}

static TestResult test_dedup_shared_block_survives_release(void)
{
    TestResult  verdict = TEST_PASS;
    uint32_t    block   = 0;
    uint32_t    held    = 0;       /* references this test still holds on the block */
    uint32_t    found   = 0;
    bool        is_dup  = false;
    TagFSState* fs      = tagfs_get_state();

    if (!TagFS_DedupIsInitialized())
        return test_steps_aside(DEDUP_ASIDE);

    TEST_ASSERT(fs != NULL, "TagFS state should not be NULL");
    TEST_ASSERT_EQ(tagfs_alloc_blocks(1, &block), 0, "alloc one block");
    held = 1;
    TEST_ASSERT(block != 0, "block 0 is not a block");

    dedup_stamp(g_test_buffer, 0x5A, "dedup-shared-block");
    TEST_ASSERT_OK(TagFS_DedupRegister(block, g_test_buffer, 0xBEEF), "register");
    TEST_ASSERT_OK(TagFS_DedupAddRef(block), "a second owner takes a reference");
    held = 2;

    uint64_t free_before = fs->ledger.free_blocks;
    held = 1;
    TEST_ASSERT_EQ(tagfs_free_blocks(block, 1), 0, "first release");
    TEST_ASSERT_EQ(fs->ledger.free_blocks, free_before,
                   "a shared block must NOT go back to the allocator");

    TagFS_DedupCheck(g_test_buffer, &found, &is_dup);
    TEST_ASSERT(is_dup && found == block, "its entry survives the first release");

    held = 0;
    TEST_ASSERT_EQ(tagfs_free_blocks(block, 1), 0, "last release");
    TEST_ASSERT_EQ(fs->ledger.free_blocks, free_before + 1,
                   "the last release hands the block back");

    is_dup = false;
    TagFS_DedupCheck(g_test_buffer, &found, &is_dup);
    TEST_ASSERT(!is_dup, "and takes the entry with it");
out:
    for (; held > 0; held--)
        tagfs_free_blocks(block, 1);
    return verdict;
}

static TestResult test_dedup_reregister_follows_content(void)
{
    TestResult verdict = TEST_PASS;
    uint32_t   block   = 0;
    bool       held    = false;
    uint32_t   found   = 0;
    bool       is_dup  = false;

    if (!TagFS_DedupIsInitialized())
        return test_steps_aside(DEDUP_ASIDE);

    TEST_ASSERT_EQ(tagfs_alloc_blocks(1, &block), 0, "alloc one block");
    held = true;

    dedup_stamp(g_test_buffer, 0xA1, "dedup-content-one");
    TEST_ASSERT_OK(TagFS_DedupRegister(block, g_test_buffer, 0xC0DE), "register first content");

    dedup_stamp(g_test_output, 0xA2, "dedup-content-two");
    TEST_ASSERT_OK(TagFS_DedupRegister(block, g_test_output, 0xC0DE), "register second content");

    TagFS_DedupCheck(g_test_output, &found, &is_dup);
    TEST_ASSERT(is_dup && found == block, "the block is found by what it holds now");

    is_dup = false;
    TagFS_DedupCheck(g_test_buffer, &found, &is_dup);
    TEST_ASSERT(!is_dup, "and not by what it used to hold");

    // One block, one entry: a single release must empty it.
    held = false;
    TEST_ASSERT_EQ(tagfs_free_blocks(block, 1), 0, "release");
    is_dup = false;
    TagFS_DedupCheck(g_test_output, &found, &is_dup);
    TEST_ASSERT(!is_dup, "re-registering must not have forked a second entry");
out:
    if (held)
        tagfs_free_blocks(block, 1);
    return verdict;
}

static const TestCase g_dedup_tests[] = {
    { .name = "dedup_delete_unregisters",            .func = test_dedup_delete_unregisters,            .reach = TEST_WRITES_TO_VOLUME },
    { .name = "dedup_truncate_unregisters",          .func = test_dedup_truncate_unregisters,          .reach = TEST_WRITES_TO_VOLUME },
    { .name = "dedup_shared_block_survives_release", .func = test_dedup_shared_block_survives_release, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "dedup_reregister_follows_content",    .func = test_dedup_reregister_follows_content,    .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_dedup_suite = {
    .name = "dedup", .tests = g_dedup_tests, .count = ARRAY_SIZE(g_dedup_tests),
};
