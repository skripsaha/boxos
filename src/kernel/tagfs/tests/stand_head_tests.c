#include "tests.h"
#include "stand_tests.h"
#include "stand_stitch.h"
#include "stand_binder.h"
#include "../tagfs.h"
#include "../settle/binder.h"
#include "../settle/settle.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_head suite: a file's first bytes, kept by its card
// (tagfs_constants.h TAGFS_FILE_HEAD, FileRecord.head)
//
// What a file is is told by its first bytes, by whoever knows what they mean:
// the volume keeps min(size, 16) of them in the file's record, exact through
// every change of what the file holds — a write of a Slip or of blocks, an
// append, a write over them, a cut and a growth — and through a mount off the
// ribbon and off a Tally; a change that leaves what the file holds as it was
// (its tail laid in its block) leaves them; and every live file of the
// volume, the image builder's among them, is held to what a read of it gives.
// ============================================================================

#define STAND_HEAD_MOST  TAGFS_HEAD_MOST
#define STAND_HEAD_PAGE  64u

static uint8_t s_head_block[TAGFS_BLOCK_SIZE];

/* Whether `fid`'s card keeps exactly `want`, `len` of them, the bit on
 * exactly when it keeps any. */
static bool StandHeadIs(uint64_t fid, const void* want, uint32_t len)
{
    TagFSMetadata md;
    if (tagfs_get_metadata(fid, &md) != 0)
        return false;
    bool same = md.head_len == len && ((md.flags & TAGFS_FILE_HEAD) != 0) == (len != 0) &&
                memcmp(md.head, want, len) == 0;
    tagfs_metadata_free(&md);
    return same;
}

/* Whether `fid`'s card keeps the bytes a read of the file begins with: 1, 0,
 * or -1 when the file would not be read. */
static int StandHeadReadIs(uint64_t fid)
{
    uint8_t          first[STAND_HEAD_MOST];
    uint64_t         got = 0;
    TagFSFileHandle* h   = tagfs_open(fid, TAGFS_HANDLE_READ);
    if (!h)
        return -1;
    error_t rc = tagfs_read(h, first, sizeof(first), &got);
    tagfs_close(h);
    return rc != OK ? -1 : StandHeadIs(fid, first, (uint32_t)got) ? 1 : 0;
}

static bool StandHeadRead(uint64_t fid)
{
    return StandHeadReadIs(fid) == 1;
}

static TestResult TestStandHeadWrites(void)
{
    TestResult verdict = TEST_PASS;
    uint64_t   fid     = 0;
    TEST_ASSERT_EQ(tagfs_create_file("head-w.txt", NULL, 0, 0, &fid), 0, "a file is made");
    TEST_ASSERT(StandHeadIs(fid, NULL, 0), "a file of no bytes keeps none");
    TEST_ASSERT_EQ(StandWriteAt(fid, 0, "hello", 5), 5, "five bytes are written");
    TEST_ASSERT(StandHeadIs(fid, "hello", 5), "it keeps the five");
    TEST_ASSERT_EQ(StandWriteAt(fid, 5, " world, and more after", 22), 22, "more go on at its end");
    TEST_ASSERT(StandHeadIs(fid, "hello world, and", 16), "it keeps its first sixteen");
    TEST_ASSERT_EQ(StandWriteAt(fid, 2, "LLO", 3), 3, "three are written over its third");
    TEST_ASSERT(StandHeadIs(fid, "heLLO world, and", 16), "and they are kept");
    TEST_ASSERT_EQ(StandWriteAt(fid, 20, "far", 3), 3, "three past them change nothing of them");
    TEST_ASSERT(StandHeadIs(fid, "heLLO world, and", 16) && StandHeadRead(fid), "they are what a read gives");
out:
    (void)tagfs_delete_file(fid, INK_WARRANTED);
    return verdict;
}

static TestResult TestStandHeadBlocks(void)
{
    TestResult verdict = TEST_PASS;
    uint64_t   fid     = 0;
    for (uint32_t i = 0; i < sizeof(s_head_block); i++)
        s_head_block[i] = (uint8_t)(i * 7u + 3u);
    TEST_ASSERT_EQ(tagfs_create_file("head-b.bin", NULL, 0, 0, &fid), 0, "a file is made");
    TEST_ASSERT_EQ(StandWriteAt(fid, 0, s_head_block, sizeof(s_head_block)), (int)sizeof(s_head_block),
                   "a whole block is written");
    TEST_ASSERT(StandHeadIs(fid, s_head_block, STAND_HEAD_MOST), "the card keeps its first bytes");
    TEST_ASSERT_EQ(StandWriteAt(fid, sizeof(s_head_block), s_head_block, sizeof(s_head_block)),
                   (int)sizeof(s_head_block), "a second block after it");
    TEST_ASSERT(StandHeadIs(fid, s_head_block, STAND_HEAD_MOST) && StandHeadRead(fid), "which changes none");
out:
    (void)tagfs_delete_file(fid, INK_WARRANTED);
    return verdict;
}

static TestResult TestStandHeadSpans(void)
{
    static const uint8_t grown[STAND_HEAD_MOST] = { 'a', 'b', 'c', 'd', 'e' };
    TestResult verdict = TEST_PASS;
    uint64_t   fid     = 0;
    TEST_ASSERT_EQ(tagfs_create_file("head-s.txt", NULL, 0, 0, &fid), 0, "a file is made");
    TEST_ASSERT_EQ(StandWriteAt(fid, 0, "abcdefghijklmnopqrstuvwxyz", 26), 26, "26 bytes are written");
    TEST_ASSERT_OK(tagfs_span_file(fid, 5), "the file is cut to five");
    TEST_ASSERT(StandHeadIs(fid, "abcde", 5), "it keeps the five left");
    TEST_ASSERT_OK(tagfs_span_file(fid, 40), "and grown to forty");
    TEST_ASSERT(StandHeadIs(fid, grown, STAND_HEAD_MOST) && StandHeadRead(fid),
                "it keeps the five and the zeros the growth reads as");
    TEST_ASSERT_OK(tagfs_span_file(fid, 0), "cut to nothing");
    TEST_ASSERT(StandHeadIs(fid, NULL, 0), "it keeps none, its bit off");
out:
    (void)tagfs_delete_file(fid, INK_WARRANTED);
    return verdict;
}

/* The tail laid in its block by the Tally a test asks: what the file holds is
 * as it was, and so are its first bytes. Then a mount off the ribbon and one
 * off a Tally read them as they were laid. */
static TestResult TestStandHeadLaidAndMounted(void)
{
    TestResult         verdict = TEST_PASS;
    static StandBinder b;
    uint64_t           small = 0, other = 0;
    const char*        aside = StandBinderAside(&b);
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT_EQ(tagfs_create_file("head-m.txt", NULL, 0, 0, &small), 0, "a file is made");
    TEST_ASSERT_EQ(StandWriteAt(small, 0, "a note of mine", 14), 14, "its fourteen bytes ride a Slip");
    TEST_ASSERT_EQ(tagfs_create_file("head-n.txt", NULL, 0, 0, &other), 0, "and another");
    TEST_ASSERT_EQ(StandWriteAt(other, 0, "another, longer note", 20), 20, "with twenty");
    TEST_ASSERT_OK(TagFSSettle(), "a cycle lays them");
    TEST_ASSERT(StandStitchRemount(), "the volume is mounted afresh, the ribbon replayed");
    TEST_ASSERT(StandHeadIs(small, "a note of mine", 14) && StandHeadIs(other, "another, longer ", 16),
                "the cards keep what the ribbon laid");
    TEST_ASSERT_OK(BinderRunWhole(), "a Tally is written, the tails laid in their blocks first");
    TEST_ASSERT(StandHeadIs(small, "a note of mine", 14), "a tail laid in its block changes no byte of the file");
    TEST_ASSERT(StandStitchRemount(), "mounted again, off the Tally");
    TEST_ASSERT(StandHeadIs(small, "a note of mine", 14) && StandHeadIs(other, "another, longer ", 16) &&
                StandHeadRead(small) && StandHeadRead(other), "the cards keep what the Tally laid");
out:
    (void)tagfs_delete_file(small, INK_WARRANTED);
    (void)tagfs_delete_file(other, INK_WARRANTED);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

/* Every live file of the volume — the image builder's, and every one the
 * tests before left — keeps what a read of it begins with. */
static TestResult TestStandHeadEveryFile(void)
{
    TestResult verdict = TEST_PASS;
    uint64_t   ids[STAND_HEAD_PAGE];
    uint64_t   from = 1;
    uint32_t   held = 0, wrong = 0, unread = 0;
    TagFSPage  page;
    do {
        TEST_ASSERT_OK(TagFSListPage(from, ids, STAND_HEAD_PAGE, false, &page), "a page of the live files");
        for (uint32_t i = 0; i < page.delivered; i++) {
            int is = StandHeadReadIs(ids[i]);
            held  += is == 1;
            unread += is < 0;
            if (is == 0) {
                wrong++;
                kprintf("[TESTS] stand_head: file %llu keeps first bytes a read of it does not begin with\n",
                        (unsigned long long)ids[i]);
            }
        }
        from = page.next;
    } while (from);
    TestEvidence("live files held to what a read of them begins with:", held);
    TestEvidence("live files no read would give, held to nothing here:", unread);
    TEST_ASSERT(held > 0 && wrong == 0, "every one read keeps them");
out:
    return verdict;
}

static const TestCase g_stand_head_tests[] = {
    { .name = "stand_head_writes", .func = TestStandHeadWrites, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_head_blocks", .func = TestStandHeadBlocks, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_head_spans", .func = TestStandHeadSpans, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_head_laid_and_mounted", .func = TestStandHeadLaidAndMounted, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_head_every_file", .func = TestStandHeadEveryFile, .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_head_suite = {
    g_stand_head_tests, sizeof(g_stand_head_tests) / sizeof(g_stand_head_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
