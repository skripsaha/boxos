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
// WIDTH — memory's widths against the narrower ones they once were carried in
//
// Memory counts a file's runs in Strips — a u64 first block, a u32 count — and
// its id in a u64, the width a program holds it in too (box/types.h). The
// storage deck is asked the way a program asks it.
//
// A tag's id is a u32 in memory: a tag past 16 bits is its own tag to a
// record, the index and Touch, and a Logbook id (bit 31) is not a volume's.
// The index and the registry are the test's own; the Touch ears are the
// test's, on ids no one else says anything on.
// ============================================================================

#define WIDTH_RUN_START      100u           /* where the record's Strip begins */
#define WIDTH_FID            7u             /* the file id the records carry */

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
 * it alone.
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
    uint64_t        found[2];
    TagFSPage       page;
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
    TestEvidence("tag ids filed under their low 16 bits", cut);
    if (cut)
        verdict = TEST_DEFECT;
out:
    FileRecordRelease(rec);
    tag_bitmap_destroy(idx);
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
        TEST_ASSERT_OK(TagRegistryIntern(scratch, TagOwnerPerson(), &one, &id), "a tag is issued past the numbers");
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
    { .name  = "width_deck_carries_wide_fid", .func = TestWidthDeckCarriesWideFid,
      .reach = TEST_READS_ONLY,
      .caveat_defect = "a file id past 32 bits was read off the storage deck's wire cut to "
                       "another file's number" },
    { .name  = "width_tag_past_16_bits", .func = TestWidthTagPast16Bits,
      .reach = TEST_READS_ONLY,
      .caveat_defect = "a tag id past 16 bits was filed as the tag its low 16 bits name" },
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
