#include "tests.h"
#include "../tagfs.h"
#include "../plot/plot.h"
#include "storage_deck.h"   /* StorageFileIdOut — an id as a program is handed it */

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// WIDTH — memory's widths against the narrower ones they are carried in
//
// Memory counts a file's runs in Strips — a u64 first block, a u32 count — and
// its id in a u64. A record of format v1 carries a u32 first block, a u16
// count and a u32 id, and a program is handed a u32 id (box/types.h). Where
// they meet — PackV1, and the storage deck's StorageFileIdOut — a run is cut
// where the narrower width allows it and anything else is refused, said,
// before a byte is written: never wrapped onto another block's number or
// another file's. The records are packed into the test buffer against a data
// run the test names; nothing touches a medium or the volume's state.
// ============================================================================

#define WIDTH_RUN_START      100u           /* where the long Strip begins */
#define WIDTH_RUN_BLOCKS     70000u         /* past the 0xFFFF blocks one extent of v1 counts */
#define WIDTH_FID            7u             /* the file id the records carry */
#define WIDTH_UNWRITTEN      0xA5u          /* the record's room before a pack */
#define WIDTH_UNHANDED       0xA5A5A5A5u    /* a program's id before the deck hands one */
#define WIDTH_EXTENTS_AT     36u            /* the record's u16 extent count (plot_pack.c) */

/* A data run no block of format v1 reaches the end of: what the refusals below
 * are refused for is the record's width, never the volume's size. */
#define WIDTH_RUN_PAST_V1    (1ull << 33)

static char s_width_name[] = "w";

/* A record of file `fid` naming `count` Strips, its name one byte. */
static TagFSMetadata WidthRecord(Strip* strips, uint32_t count, uint64_t fid)
{
    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    meta.file_id      = fid;
    meta.flags        = TAGFS_FILE_ACTIVE;
    meta.extents      = strips;
    meta.extent_count = count;
    meta.filename     = s_width_name;
    return meta;
}

/* How many of the record room's bytes are not what they were before a pack. */
static uint32_t WidthWritten(const uint8_t* room)
{
    uint32_t written = 0;
    for (uint32_t i = 0; i < TAGFS_MPOOL_DATA_SIZE; i++)
        written += room[i] != WIDTH_UNWRITTEN;
    return written;
}

/*
 * A Strip of 70 000 blocks is packed as format v1 counts them: an extent of
 * 0xFFFF blocks and one of the 4465 after it, back to back — and read back as
 * the one Strip it was.
 */
static TestResult TestWidthPackCutsLongStrip(void)
{
    TestResult    verdict = TEST_PASS;
    Strip         strip   = { .start = WIDTH_RUN_START, .count = WIDTH_RUN_BLOCKS,
                              .kind = STRIP_DATA, .birth = 0 };
    TagFSMetadata meta    = WidthRecord(&strip, 1, WIDTH_FID);
    TagFSMetadata back;
    uint8_t*      rec     = g_test_output;
    uint64_t      run     = (uint64_t)WIDTH_RUN_START + WIDTH_RUN_BLOCKS;
    uint32_t      bytes   = 0;
    uint16_t      extents = 0;
    bool          dropped = true;
    FileExtent    x[2];

    memset(&back, 0, sizeof(back));
    TEST_ASSERT_EQ(PackV1Extents(&strip, 1), 2, "the Strip is counted as two extents of v1");
    TEST_ASSERT_OK(PackV1(&meta, run, rec, &bytes), "a record of 70000 blocks is packed");
    TEST_ASSERT_EQ(bytes, RECORD_HEADER_SIZE + 2u * sizeof(FileExtent) + 1u,
                   "as long as two extents and a one-byte name");
    memcpy(&extents, rec + WIDTH_EXTENTS_AT, sizeof(extents));
    memcpy(x, rec + RECORD_HEADER_SIZE, sizeof(x));
    TEST_ASSERT_EQ(extents, 2, "the record says two extents");
    TEST_ASSERT(x[0].start_block == WIDTH_RUN_START && x[0].block_count == 0xFFFFu,
                "the first: 0xFFFF blocks from the Strip's first");
    TEST_ASSERT(x[1].start_block == WIDTH_RUN_START + 0xFFFFu &&
                x[1].block_count == WIDTH_RUN_BLOCKS - 0xFFFFu,
                "the second: the 4465 blocks after them");

    TEST_ASSERT_OK(UnpackV1(rec, bytes, run, &back, &dropped), "the record is read back");
    TEST_ASSERT(!dropped, "naming its blocks");
    TEST_ASSERT_EQ(back.file_id, WIDTH_FID, "as the file it was");
    TEST_ASSERT_EQ(back.extent_count, 1, "its two extents joined into one Strip");
    TEST_ASSERT(back.extents[0].start == WIDTH_RUN_START &&
                back.extents[0].count == WIDTH_RUN_BLOCKS && back.extents[0].kind == STRIP_DATA,
                "of all 70000 blocks");
out:
    tagfs_metadata_free(&back);
    return verdict;
}

/* A record PackV1 must turn away, and the refusal it is turned away with. */
typedef struct {
    Strip    strip;
    uint64_t fid;
    error_t  refused;
} WidthPastV1;

static const WidthPastV1 s_width_past_v1[] = {
    { { 1ull << 32, 1, STRIP_DATA, 0 },                   WIDTH_FID, ERR_TAGFS_RECORD_FULL },
    { { 0xFFFFFFF0ull, 0x20, STRIP_DATA, 0 },             WIDTH_FID, ERR_TAGFS_RECORD_FULL },
    { { WIDTH_RUN_START, 8, STRIP_HOLE, 0 },              WIDTH_FID, ERR_TAGFS_RECORD_FULL },
    { { 0, 0xFFFFu * 1000u, STRIP_DATA, 0 },              WIDTH_FID, ERR_TAGFS_RECORD_FULL },
    { { WIDTH_RUN_START, 8, STRIP_DATA, 0 }, (uint64_t)UINT32_MAX + 1u, ERR_TAGFS_FID_EXHAUSTED },
};

/*
 * What a record of format v1 cannot carry — a run from block 2^32, one that
 * crosses it, a hole, a run cut into more extents than a record holds, a file
 * id past 32 bits — is refused by PackV1 as what it is, and not a byte of the
 * record's room is written; nor is a packed record given an id past 32 bits.
 */
static TestResult TestWidthPackRefusesPastV1(void)
{
    TestResult verdict = TEST_PASS;
    uint8_t*   rec     = g_test_output;
    uint32_t   cut     = 0;

    for (uint32_t i = 0; i < ARRAY_SIZE(s_width_past_v1); i++) {
        Strip         strip = s_width_past_v1[i].strip;
        TagFSMetadata meta  = WidthRecord(&strip, 1, s_width_past_v1[i].fid);
        uint32_t      bytes = 0;

        memset(rec, WIDTH_UNWRITTEN, TAGFS_MPOOL_DATA_SIZE);
        error_t rc = PackV1(&meta, WIDTH_RUN_PAST_V1, rec, &bytes);
        if (rc == OK || bytes != 0 || WidthWritten(rec) != 0) {
            cut++;
            continue;
        }
        TEST_ASSERT_EQ(rc, s_width_past_v1[i].refused, "refused as what it cannot carry");
    }

    Strip         strip = { .start = WIDTH_RUN_START, .count = 8, .kind = STRIP_DATA, .birth = 0 };
    TagFSMetadata meta  = WidthRecord(&strip, 1, WIDTH_FID);
    uint32_t      bytes = 0;
    TEST_ASSERT_OK(PackV1(&meta, WIDTH_RUN_PAST_V1, rec, &bytes), "a record v1 carries is packed");
    memcpy(g_test_buffer, rec, bytes);
    error_t refiled = PackV1Refile(rec, (uint64_t)UINT32_MAX + WIDTH_FID);
    if (refiled == OK || memcmp(g_test_buffer, rec, bytes) != 0)
        cut++;

    TestEvidence("records past what format v1 carries packed, or written into before the "
                 "refusal", cut);
    if (cut) {
        verdict = TEST_DEFECT;
        goto out;
    }
    TEST_ASSERT_EQ(refiled, ERR_TAGFS_FID_EXHAUSTED, "an id past 32 bits is not refiled");
out:
    return verdict;
}

/*
 * The storage deck hands a program the u32 its file_id_t holds: the last of
 * them whole, and an id past it refused as one — the program's u32 left as it
 * was, never another file's number cut from it.
 */
static TestResult TestWidthDeckRefusesWideFid(void)
{
    TestResult verdict = TEST_PASS;
    uint32_t   handed  = WIDTH_UNHANDED;
    error_t    rc;

    TEST_ASSERT_OK(StorageFileIdOut(UINT32_MAX, &handed), "the last id a program holds is handed");
    TEST_ASSERT_EQ(handed, UINT32_MAX, "whole");

    handed = WIDTH_UNHANDED;
    rc     = StorageFileIdOut((uint64_t)UINT32_MAX + WIDTH_FID, &handed);
    TestEvidence("ids past 32 bits handed to a program, cut", rc == OK || handed != WIDTH_UNHANDED);
    if (rc == OK || handed != WIDTH_UNHANDED) {
        verdict = TEST_DEFECT;
        goto out;
    }
    TEST_ASSERT_EQ(rc, ERR_TAGFS_FID_EXHAUSTED, "an id past 32 bits is refused as such");
out:
    return verdict;
}

static const TestCase g_width_tests[] = {
    { .name = "width_pack_cuts_long_strip", .func = TestWidthPackCutsLongStrip,
      .reach = TEST_READS_ONLY },
    { .name  = "width_pack_refuses_past_v1", .func = TestWidthPackRefusesPastV1,
      .reach = TEST_READS_ONLY,
      .caveat_defect = "a record past what format v1 carries — a block past 32 bits, a hole, more "
                       "extents than a record holds, a file id past 32 bits — was packed cut, or "
                       "its room written into before the refusal" },
    { .name  = "width_deck_refuses_wide_fid", .func = TestWidthDeckRefusesWideFid,
      .reach = TEST_READS_ONLY,
      .caveat_defect = "a file id past 32 bits was handed to a program cut to another file's "
                       "number" },
};

const TestSuite g_width_suite = {
    .tests = g_width_tests, .count = ARRAY_SIZE(g_width_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
