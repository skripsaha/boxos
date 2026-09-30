#include "tests.h"
#include "../tagfs.h"
#include "../cow/cow.h"

// ============================================================================
// Copy-on-write, as format v1 leaves it
//
// A format-v1 volume takes no snapshot: every request for one is answered
// ERR_UNSUPPORTED (TagFS_SnapshotRefused), and a write lands where the file's
// blocks are — nothing is copied for a snapshot, and no extent moves.
// ============================================================================

static uint8_t s_cow_block[TAGFS_BLOCK_SIZE];

#define COW_SPAN_BLOCKS 3u

/* The data-run block that holds the k-th block of a file's record, or 0 when
 * the record does not reach that far. */
static uint32_t CowBlockOf(const TagFSMetadata* meta, uint32_t k)
{
    for (uint32_t e = 0; e < meta->extent_count && meta->extents; e++) {
        if (k < meta->extents[e].block_count)
            return meta->extents[e].start_block + k;
        k -= meta->extents[e].block_count;
    }
    return 0;
}

/*
 * W-COW. A file whose data is ONE extent three blocks long, a snapshot of it
 * asked for, and a write of its first block alone. The write path used to copy
 * that block for the snapshot and move the extent's start onto the copy — the
 * whole extent, three blocks long — so the file's second and third blocks then
 * read the two blocks after the copy, which it never held. Now the snapshot is
 * refused and the write lands where the block is: the evidence counts every
 * block of the file that the write moved, or changed besides the one it wrote.
 *
 * The extent is laid down by gathering three one-block extents into one
 * (tagfs_defrag_file): the write path gives every 4 KiB an extent of its own,
 * so that is how a longer one arises on this volume. Its three blocks are then
 * written over in place with bytes of this file's own (the stream names the
 * file id, which no earlier boot had), so no other block on the volume holds
 * what they hold, and only the file's own blocks can read back right.
 */
static TestResult TestCowMovesOnlyTheBlockWritten(void)
{
    TestResult    verdict   = TEST_PASS;
    uint32_t      file_id   = 0;
    uint32_t      start     = 0;
    bool          have_meta = false;
    uint8_t*      was       = g_test_buffer;                                /* 3 blocks */
    uint8_t*      now       = g_test_buffer + COW_SPAN_BLOCKS * TAGFS_BLOCK_SIZE;
    TagFSMetadata meta;

    TEST_ASSERT_EQ(tagfs_create_file("cow-test", NULL, 0, &file_id), 0, "a file is created");
    TestFillAt(g_test_output, COW_SPAN_BLOCKS * TAGFS_BLOCK_SIZE, 0xC1DE0000u | file_id, 0);
    TestFillAt(was, COW_SPAN_BLOCKS * TAGFS_BLOCK_SIZE, 0xC2DE0000u | file_id, 0);
    TestFillAt(now, TAGFS_BLOCK_SIZE, 0xC3DE0000u | file_id, 0);
    TEST_ASSERT_EQ(TestFileWrite(file_id, g_test_output, COW_SPAN_BLOCKS * TAGFS_BLOCK_SIZE),
                   COW_SPAN_BLOCKS * TAGFS_BLOCK_SIZE, "three blocks are written");
    TEST_ASSERT_EQ(tagfs_defrag_file(file_id, 0), 0, "and gathered into one extent");
    TEST_ASSERT_EQ(TestFileWrite(file_id, was, COW_SPAN_BLOCKS * TAGFS_BLOCK_SIZE),
                   COW_SPAN_BLOCKS * TAGFS_BLOCK_SIZE, "and written over");

    TEST_ASSERT_EQ(tagfs_get_metadata(file_id, &meta), 0, "the file's metadata reads");
    have_meta = true;
    TEST_ASSERT_EQ(meta.extent_count, 1, "the file is one extent, written over in place");
    TEST_ASSERT_EQ(meta.extents[0].block_count, COW_SPAN_BLOCKS, "three blocks long");
    start = meta.extents[0].start_block;
    tagfs_metadata_free(&meta);
    have_meta = false;

    TEST_ASSERT_EQ(TagFS_SnapshotRefused("a snapshot of the cow suite's file"), ERR_UNSUPPORTED,
                   "a snapshot of the file is refused, as every snapshot is on format v1");

    TEST_ASSERT_EQ(TestFileWrite(file_id, now, TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                   "the first block alone is written");
    TEST_ASSERT_EQ(TestFileRead(file_id, g_test_output, COW_SPAN_BLOCKS * TAGFS_BLOCK_SIZE),
                   COW_SPAN_BLOCKS * TAGFS_BLOCK_SIZE, "the file reads back whole");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output, now, TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                   "its first block holds the write");
    TEST_ASSERT_OK(tagfs_read_block(start, s_cow_block), "the extent's first block reads");
    TEST_ASSERT_EQ(TestFirstDifference(s_cow_block, now, TAGFS_BLOCK_SIZE), TAGFS_BLOCK_SIZE,
                   "and the write landed there, in place");

    TEST_ASSERT_EQ(tagfs_get_metadata(file_id, &meta), 0, "the file's metadata reads again");
    have_meta = true;
    uint32_t off = 0;
    for (uint32_t k = 0; k < COW_SPAN_BLOCKS; k++) {
        off += CowBlockOf(&meta, k) != start + k;
        if (k > 0)
            off += TestFirstDifference(g_test_output + k * TAGFS_BLOCK_SIZE,
                                       was + k * TAGFS_BLOCK_SIZE,
                                       TAGFS_BLOCK_SIZE) != TAGFS_BLOCK_SIZE;
    }
    TestEvidence("blocks of the file the write moved or changed besides the one it wrote", off);
    if (off)
        verdict = TEST_DEFECT;
out:
    if (have_meta)
        tagfs_metadata_free(&meta);
    if (file_id)
        tagfs_delete_file(file_id);
    return verdict;
}

static const TestCase g_cow_tests[] = {
    { .name  = "cow_moves_only_the_block_written", .func = TestCowMovesOnlyTheBlockWritten,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a write into the first block of a longer extent, under a snapshot, "
                       "moves the whole extent onto a one-block copy, so the file's later "
                       "blocks read blocks it never held" },
};

const TestSuite g_cow_suite = {
    .tests = g_cow_tests, .count = ARRAY_SIZE(g_cow_tests),
};
