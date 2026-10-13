#include "tests.h"
#include "../tagfs.h"
#include "../plot/plot.h"
#include "../file_map/file_map.h"
#include "../tag_registry/tag_registry.h"
#include "op_registry.h"
#include "boxos_decks.h"
#include "touch.h"
#include "logbook.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// WIDTH — memory's widths against the narrower ones they are carried in
//
// Memory counts a file's runs in Strips — a u64 first block, a u32 count — and
// its id in a u64, the width a program holds it in too (box/types.h). A record
// of format v1 carries a u32 first block, a u16 count and a u32 id. Where they
// meet — PackV1 — a run is cut where the narrower width allows it and anything
// else is refused, said, before a byte is written: never wrapped onto another
// block's number or another file's. The records are packed into the test
// buffer against a data run the test names; nothing touches a medium or the
// volume's state. The storage deck is asked the way a program asks it.
//
// A tag's id is a u32 in memory, a format v1 record's a u16 and v1's registry
// issues none past 0x7FFE: a tag past 16 bits is its own tag to a record, the
// index and Touch, a Logbook id (bit 31) is not a volume's, and a v1 volume is
// refused the id it cannot carry. The index and the registry are the test's
// own; the Touch ears are the test's, on ids no one else says anything on.
// ============================================================================

#define WIDTH_RUN_START      100u           /* where the long Strip begins */
#define WIDTH_RUN_BLOCKS     70000u         /* past the 0xFFFF blocks one extent of v1 counts */
#define WIDTH_FID            7u             /* the file id the records carry */
#define WIDTH_UNWRITTEN      0xA5u          /* the record's room before a pack */
#define WIDTH_EXTENTS_AT     36u            /* the record's u16 extent count (plot_pack.c) */

/* A data run no block of format v1 reaches the end of: what the refusals below
 * are refused for is the record's width, never the volume's size. */
#define WIDTH_RUN_PAST_V1    (1ull << 33)

#define WIDTH_TAG_PAST_16    0x10007u      /* a tag id past 16 bits */
#define WIDTH_TAG_BIT15      0x8009u       /* bit 15: a Logbook id to a kernel of 16-bit tags */
#define WIDTH_TAG_CUT        0xFFFFu       /* what cut a tag id to 16 bits */
#define WIDTH_TAG_LOW15      0x7FFFu       /* ... and to the 15 a kernel of 16-bit tags numbered by */
#define WIDTH_LIVE_TAGS      10u           /* tags with a member */
#define WIDTH_ISSUED_TAGS    100000u       /* the ids issued below and among them */
#define WIDTH_PER_TAG        256u          /* what a tag of one member may cost the index */

static char    s_width_name[] = "w";
static uint8_t s_width_op[sizeof(ManifestOp) + 8] __attribute__((aligned(8)));

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

/* storage.getinfo of `fid`, its params as a program's file_info lays them,
 * into a crate with no room: the handler's answer. A record it finds is
 * measured against the crate and refused as too big for it — never written. */
static int WidthDeckInfo(uint64_t fid)
{
    const OpRegistration* reg   = OpRegistryLookup(OP_KIND(DECK_STORAGE, STORAGE_OBJ_GET_INFO));
    ManifestOp*           op    = (ManifestOp*)s_width_op;
    Crate                 crate = { .magic = CRATE_MAGIC, .addr = 0, .capacity = 0 };

    if (!reg)
        return ERR_NOT_INITIALIZED;
    memset(s_width_op, 0, sizeof(s_width_op));
    op->op_kind    = OP_KIND(DECK_STORAGE, STORAGE_OBJ_GET_INFO);
    op->param_size = sizeof(fid);
    op->in_crate   = CRATE_INDEX_NONE;
    op->out_crate  = 0;
    memcpy(op->params, &fid, sizeof(fid));
    return reg->handler(op, &crate, 1, NULL);
}

/*
 * A program's file id rides the storage deck's wire whole, in 64 bits: asked
 * about 2^32 past a file the volume holds, the deck answers that there is no
 * such file — never that file's record, its id cut to the low 32 bits.
 */
static TestResult TestWidthDeckCarriesWideFid(void)
{
    TestResult verdict = TEST_PASS;
    uint64_t   live    = 0;
    TagFSPage  page;

    TEST_ASSERT_OK(TagFSListPage(1, &live, 1, false, &page), "the volume is asked for a file");
    TEST_ASSERT(page.delivered == 1 && live != 0 && live <= UINT32_MAX,
                "and names one, its id within 32 bits");
    TEST_ASSERT_EQ(WidthDeckInfo(live), ERR_BUFFER_TOO_SMALL, "the deck finds that file by its id");

    int wide = WidthDeckInfo(((uint64_t)1 << 32) | live);
    TestEvidence("ids past 32 bits the deck answered as another file's, cut",
                 wide == ERR_BUFFER_TOO_SMALL);
    if (wide == ERR_BUFFER_TOO_SMALL) {
        verdict = TEST_DEFECT;
        goto out;
    }
    TEST_ASSERT_EQ(wide, ERR_FILE_NOT_FOUND, "and answers an id 2^32 past it as no file at all");
out:
    return verdict;
}

/*
 * A tag id past 16 bits is its own tag, not the one its low 16 bits name: a
 * file's record in memory carries it whole and the index files the file under
 * it alone. A record of format v1 holds a tag in 16 bits and its registry
 * issues none past 0x7FFE: PackV1 refuses a record wearing one past that, not
 * a byte of its room written, and packs one wearing 0x7FFE itself.
 */
static TestResult TestWidthTagPast16Bits(void)
{
    TestResult      verdict = TEST_PASS;
    uint32_t        tags[1] = { WIDTH_TAG_PAST_16 };
    uint32_t        low     = WIDTH_TAG_PAST_16 & WIDTH_TAG_CUT;
    Strip           strip   = { .start = WIDTH_RUN_START, .count = 1, .kind = STRIP_DATA, .birth = 0 };
    TagFSMetadata   meta    = WidthRecord(&strip, 1, WIDTH_FID);
    TagBitmapIndex* idx     = tag_bitmap_create();
    FileRecord*     rec     = NULL;
    uint8_t*        room    = g_test_output;
    uint64_t        found[2];
    TagFSPage       page;
    uint32_t        bytes   = 0;
    uint32_t        cut     = 0;

    meta.tag_ids   = tags;
    meta.tag_count = 1;
    TEST_ASSERT(idx != NULL, "an index of the test's own");
    rec = FileRecordBuild(&meta);
    TEST_ASSERT(rec != NULL && rec->tag_count == 1, "a record in memory wears the tag");
    TEST_ASSERT_EQ(rec->tags[0], WIDTH_TAG_PAST_16, "whole, past 16 bits");

    TEST_ASSERT_EQ(tag_bitmap_set(idx, WIDTH_TAG_PAST_16, WIDTH_FID), 0, "the index files the file under it");
    TEST_ASSERT_EQ(TagBitmapQueryPage(idx, tags, 1, NULL, 0, NULL, 0, 1, found, 2, true, &page), 0,
                   "asked for the tag");
    TEST_ASSERT(page.delivered == 1 && found[0] == WIDTH_FID, "it answers the file");
    TEST_ASSERT_EQ(TagBitmapQueryPage(idx, &low, 1, NULL, 0, NULL, 0, 1, found, 2, true, &page), 0,
                   "asked for the id the tag's low 16 bits name");
    cut += page.delivered;

    for (uint32_t past = 0; past < 2; past++) {
        tags[0] = past ? WIDTH_TAG_PAST_16 : TAGFS_V1_TAG_ID_MOST + 1u;
        memset(room, WIDTH_UNWRITTEN, TAGFS_MPOOL_DATA_SIZE);
        bytes = 0;
        error_t rc = PackV1(&meta, WIDTH_RUN_PAST_V1, room, &bytes);
        if (rc == OK || bytes != 0 || WidthWritten(room) != 0) {
            cut++;
            continue;
        }
        TEST_ASSERT_EQ(rc, ERR_TAGFS_RECORD_FULL, "a tag past format v1's is refused as what its record cannot carry");
    }
    TestEvidence("tag ids filed under their low 16 bits, or packed into a v1 record", cut);
    if (cut) {
        verdict = TEST_DEFECT;
        goto out;
    }
    tags[0] = TAGFS_V1_TAG_ID_MOST;
    TEST_ASSERT_OK(PackV1(&meta, WIDTH_RUN_PAST_V1, room, &bytes), "and a record wearing 0x7FFE is packed");
out:
    FileRecordRelease(rec);
    tag_bitmap_destroy(idx);
    return verdict;
}

/*
 * The registry of a format v1 volume — the load sets its highest id to
 * 0x7FFE — issues 0x7FFE and refuses the tag after it, said, with no id given
 * and the tag not in it: never an id a v1 record would have to cut.
 */
static TestResult TestWidthTagRegistryV1(void)
{
    TestResult   verdict = TEST_PASS;
    TagRegistry* scratch = kmalloc(sizeof(TagRegistry));
    bool         built   = false;
    uint32_t     last    = TAGFS_INVALID_TAG_ID;
    uint32_t     past    = TAGFS_INVALID_TAG_ID;
    TagSpelling  a       = TagSpellingOfKey("width.v1.last");
    TagSpelling  b       = TagSpellingOfKey("width.v1.past");

    TEST_ASSERT(scratch != NULL, "memory for a registry of the test's own");
    TEST_ASSERT_EQ(tag_registry_init(scratch), 0, "a registry of the test's own is built");
    built = true;
    scratch->id_most = TAGFS_V1_TAG_ID_MOST;   /* as tag_registry_load sets it for a v1 volume */
    TagRegistryNumbersFrom(scratch, TAGFS_V1_TAG_ID_MOST);
    TEST_ASSERT_OK(TagRegistryIntern(scratch, &a, &last), "the last id format v1 carries is issued");
    TEST_ASSERT_EQ(last, TAGFS_V1_TAG_ID_MOST, "and it is 0x7FFE");

    error_t rc = TagRegistryIntern(scratch, &b, &past);
    TestEvidence("ids past 0x7FFE issued by the registry of a format v1 volume", rc == OK);
    if (rc == OK) {
        verdict = TEST_DEFECT;
        goto out;
    }
    TEST_ASSERT_EQ(rc, ERR_TAGFS_REGISTRY_FULL, "the next tag is refused as a full registry");
    TEST_ASSERT_EQ(past, TAGFS_INVALID_TAG_ID, "with no id given");
    TEST_ASSERT_EQ(TagRegistryLookup(scratch, &b), TAGFS_INVALID_TAG_ID, "and the tag is not in it");
out:
    if (built)
        tag_registry_destroy(scratch);
    kfree(scratch);
    return verdict;
}

/*
 * What who wears a tag costs is the tags that have members, never the highest
 * id issued: ten tags of one member each up to id 100 000 cost the index what
 * ten side by side do, and no more than 256 bytes a tag where an array by id
 * paid for every one of the 100 000. The registry's hash of ids, its numbers
 * moved to 99 990 and ten tags issued past them, holds them in the buckets it
 * began with.
 */
static TestResult TestWidthTagIndexCostsTheLive(void)
{
    TestResult      verdict = TEST_PASS;
    TagBitmapIndex* near    = tag_bitmap_create();
    TagBitmapIndex* far     = tag_bitmap_create();
    TagRegistry*    scratch = kmalloc(sizeof(TagRegistry));
    bool            built   = false;
    uint32_t        step    = WIDTH_ISSUED_TAGS / WIDTH_LIVE_TAGS;
    uint32_t        id      = TAGFS_INVALID_TAG_ID;
    char            name[24];

    TEST_ASSERT(near && far && scratch, "two indexes and a registry of the test's own");
    uint64_t empty = TagBitmapBytes(far);
    for (uint32_t k = 0; k < WIDTH_LIVE_TAGS; k++) {
        TEST_ASSERT_EQ(tag_bitmap_set(near, 1u + k, WIDTH_FID), 0, "a file wears ten tags side by side");
        TEST_ASSERT_EQ(tag_bitmap_set(far, (k + 1u) * step, WIDTH_FID), 0,
                       "and ten tags up to id 100 000");
    }
    uint64_t taken = TagBitmapBytes(far) - empty;
    TestEvidence("bytes the index took for ten tags up to id 100 000:", taken);
    if (TagBitmapBytes(far) != TagBitmapBytes(near) ||
        taken > (uint64_t)WIDTH_LIVE_TAGS * WIDTH_PER_TAG) {
        verdict = TEST_DEFECT;
        goto out;
    }

    TEST_ASSERT_EQ(tag_registry_init(scratch), 0, "a registry of the test's own is built");
    built = true;
    uint32_t buckets = scratch->bucket_count;
    TagRegistryNumbersFrom(scratch, WIDTH_ISSUED_TAGS - WIDTH_LIVE_TAGS);
    for (uint32_t k = 0; k < WIDTH_LIVE_TAGS; k++) {
        ksnprintf(name, sizeof(name), "width.live.%u", k);
        TagSpelling one = TagSpellingOfKey(name);
        TEST_ASSERT_OK(TagRegistryIntern(scratch, &one, &id), "a tag is issued past the numbers");
    }
    TEST_ASSERT_EQ(id, WIDTH_ISSUED_TAGS - 1u, "the tenth at 99 999");
    TEST_ASSERT_EQ(scratch->bucket_count, buckets,
                   "and the hash of ids holds the ten in the buckets it began with");
out:
    if (built)
        tag_registry_destroy(scratch);
    kfree(scratch);
    tag_bitmap_destroy(near);
    tag_bitmap_destroy(far);
    return verdict;
}

typedef struct {
    volatile uint32_t heard;
} WidthEar;

static void WidthEarHeard(TouchTag tag_id, const void* payload, uint32_t plen,
                          uint32_t source_pid, void* ctx)
{
    (void)tag_id;
    (void)payload;
    (void)plen;
    (void)source_pid;
    __atomic_add_fetch(&((WidthEar*)ctx)->heard, 1u, __ATOMIC_RELEASE);
}

/*
 * Touch tells three ids from their twins: a volume's tag past 16 bits from
 * the one its low 16 bits name; a volume's tag of bit 15, which a kernel of
 * 16-bit tags read as its Logbook's, from the id its low 15 bits name; and a
 * Logbook id — bit 31 — from the volume's id of the same number. Each is
 * said once and heard once, by its own ear alone.
 */
static TestResult TestWidthTouchTagPast16Bits(void)
{
    TestResult  verdict = TEST_PASS;
    TouchTag    kernel  = TouchLogbookIntern("width:kernel");
    TouchTag    said[3] = { WIDTH_TAG_PAST_16, WIDTH_TAG_BIT15, kernel };
    TouchTag    twin[3] = { WIDTH_TAG_PAST_16 & WIDTH_TAG_CUT, WIDTH_TAG_BIT15 & WIDTH_TAG_LOW15,
                            kernel & ~TOUCH_TAG_KERNEL_BIT };
    WidthEar    own[3]   = { { 0 }, { 0 }, { 0 } };
    WidthEar    other[3] = { { 0 }, { 0 }, { 0 } };
    TouchWatch* ear[6]   = { NULL, NULL, NULL, NULL, NULL, NULL };
    uint32_t    stray    = 0;
    uint32_t    payload  = 0;

    TEST_ASSERT(kernel != TOUCH_TAG_INVALID && TouchTagIsKernel(kernel), "a Logbook id wears bit 31");
    TEST_ASSERT(!TouchTagIsKernel(WIDTH_TAG_BIT15) && !TouchTagIsKernel(WIDTH_TAG_PAST_16),
                "and a volume's id of bit 15 or past 16 bits does not");
    for (uint32_t i = 0; i < 3; i++) {
        ear[2 * i]     = TouchWatchSet(said[i], WidthEarHeard, &own[i]);
        ear[2 * i + 1] = TouchWatchSet(twin[i], WidthEarHeard, &other[i]);
        TEST_ASSERT(ear[2 * i] && ear[2 * i + 1], "an ear on the id and one on its twin");
    }
    for (uint32_t i = 0; i < 3; i++)
        (void)TouchPublishId(said[i], &payload, sizeof(payload), 0, 0);
    for (uint32_t i = 0; i < 3; i++)
        stray += other[i].heard;
    TestEvidence("events heard by the ear of an id they were not said on", stray);
    if (stray) {
        verdict = TEST_DEFECT;
        goto out;
    }
    for (uint32_t i = 0; i < 3; i++)
        TEST_ASSERT_EQ(own[i].heard, 1, "each id said once is heard once by its own ear");
out:
    for (uint32_t i = 0; i < 6; i++)
        if (ear[i])
            TouchWatchClear(ear[i]);
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
    { .name  = "width_deck_carries_wide_fid", .func = TestWidthDeckCarriesWideFid,
      .reach = TEST_READS_ONLY,
      .caveat_defect = "a file id past 32 bits was read off the storage deck's wire cut to "
                       "another file's number" },
    { .name  = "width_tag_past_16_bits", .func = TestWidthTagPast16Bits,
      .reach = TEST_READS_ONLY,
      .caveat_defect = "a tag id past 16 bits was filed as the tag its low 16 bits name, or "
                       "packed into a record of format v1 that cannot carry it" },
    { .name  = "width_tag_registry_v1", .func = TestWidthTagRegistryV1,
      .reach = TEST_READS_ONLY,
      .caveat_defect = "the registry of a format v1 volume issued a tag id past 0x7FFE" },
    { .name  = "width_tag_index_costs_the_live", .func = TestWidthTagIndexCostsTheLive,
      .reach = TEST_READS_ONLY,
      .caveat_defect = "the index of who wears a tag cost the highest id issued, not the tags "
                       "with members" },
    { .name  = "width_touch_tag_past_16_bits", .func = TestWidthTouchTagPast16Bits,
      .reach = TEST_READS_ONLY,
      .caveat_defect = "Touch handed an event said on one tag id to the ear of another — its "
                       "low 16 bits, or the Logbook's and the volume's ids taken for each other" },
};

const TestSuite g_width_suite = {
    .tests = g_width_tests, .count = ARRAY_SIZE(g_width_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
