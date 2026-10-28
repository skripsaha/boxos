#include "tests.h"
#include "volume_ribbon.h"
#include "crypto.h"
#include "../mount/ribbon.h"

// ============================================================================
// The ribbon's codec (volume_ribbon.h) held to known answers — its golden
// stitch, every record read back as it was written, cut short and grown
//
// Arithmetic over memory and nothing else, like the book suite beside it
// (book_tests.c): no medium is touched, so the suite is in every build. The
// golden stitch is the bytes both writers of a ribbon reach — this codec here,
// stitch.py's encoders in volume-read's self-test X3 — so a record one of them
// spells otherwise is caught at its sum before a landing ever boots.
// ============================================================================

/* The golden stitch's records, as the header's comment lays them down. */
static const uint32_t g_golden_tags[]   = { 3, 5, 300 };
static const uint64_t g_golden_undone[] = { 5, 6 };
static const uint32_t g_golden_add[]    = { 9 };
static const uint32_t g_golden_drop[]   = { 3 };
static const uint32_t g_golden_scope[]  = { 2, 3 };

#define RIBBON_TEST_RECORDS  10u

/* Where each of the golden stitch's records lies in its stream: the record,
 * and its body. */
typedef struct {
    uint64_t kind;
    uint32_t at;
    uint32_t body;
    uint32_t len;
} RibbonTestRecord;

/* One record laid at `out` from its `body` of `len`, its place kept. */
static uint32_t RibbonTestLay(uint8_t *out, uint32_t at, uint64_t kind, int ignorable, const uint8_t *body,
                              uint32_t len, RibbonTestRecord *r)
{
    uint32_t n = RibbonRecordPut(out + at, kind, ignorable, body, len);
    if (r) {
        r->kind = kind;
        r->at   = at;
        r->body = at + n - len;
        r->len  = len;
    }
    return n;
}

/* The runs a record of the golden stitch puts in, laid in the ribbon's form,
 * the cursor carried: the bytes they took. */
static uint32_t RibbonTestRuns(uint8_t *out, const TallyRun *runs, uint32_t n, uint64_t *cursor)
{
    uint32_t at = 0;
    for (uint32_t i = 0; i < n; i++)
        at += RibbonRunPut(out + at, &runs[i], cursor);
    return at;
}

static uint32_t RibbonTestBorn(uint8_t *body, uint8_t *lists)
{
    static const TallyRun runs[] = { { .start = 1024, .count = 1 }, { .count = 1, .hole = 1 } };
    uint8_t  digest[8];
    uint64_t cursor = 0;
    RibbonBorn r = { .fid = 5, .flags = TALLY_FILE_ACTIVE, .size = 5000, .created_ns = 1700000000000000000ull,
                     .modified_ns = 1700000000000000007ull, .name = (const uint8_t *)"golden", .name_len = 6,
                     .ntags = 3, .nruns = 2, .ndigests = 1,
                     .slip = { .off = 100, .len = 4, .bytes = (const uint8_t *)"tail" } };
    r.tags_bytes = TallyIdsPut(lists, g_golden_tags, 3);
    r.tags       = lists;
    r.runs       = lists + r.tags_bytes;
    r.runs_bytes = RibbonTestRuns(lists + r.tags_bytes, runs, 2, &cursor);
    TallyLePut(digest, 0x1122334455667788ull, 8);
    r.digests = digest;
    return RibbonBornBodyPut(body, &r);
}

static uint32_t RibbonTestShaped(uint8_t *body, uint8_t *lists)
{
    static const TallyRun one[]  = { { .start = 4096, .count = 1 } };
    static const TallyRun hole[] = { { .count = 1, .hole = 1 } };
    uint8_t  runs[32], digest[8];
    uint64_t cursor = 0;
    uint32_t at = 0, n;
    n   = RibbonTestRuns(runs, one, 1, &cursor);
    at += RibbonSplicePut(lists + at, 1, 1, 1, runs, n);
    n   = RibbonTestRuns(runs, hole, 1, &cursor);
    at += RibbonSplicePut(lists + at, 0, 0, 1, runs, n);
    TallyLePut(digest, 0x99AABBCCDDEEFF00ull, 8);
    RibbonShaped r = { .fid = 6, .base = 1, .size = 9000, .mtime_ns = 2, .nsplice = 2, .splices = lists,
                       .splices_bytes = at, .ndigests = 1, .digests = digest };
    return RibbonShapedBodyPut(body, &r);
}

static uint32_t RibbonTestLabeled(uint8_t *body, uint8_t *lists)
{
    RibbonLabeled r = { .fid = 7, .base = 2, .what = LABEL_NAME | LABEL_FLAGS | LABEL_TAGS,
                        .name = (const uint8_t *)"renamed", .name_len = 7, .set = 4, .clear = 2,
                        .nadd = 1, .ndrop = 1 };
    r.add_bytes  = TallyIdsPut(lists, g_golden_add, 1);
    r.add        = lists;
    r.drop       = lists + r.add_bytes;
    r.drop_bytes = TallyIdsPut(lists + r.add_bytes, g_golden_drop, 1);
    return RibbonLabeledBodyPut(body, &r);
}

static uint32_t RibbonTestFrozen(uint8_t *body)
{
    uint32_t at = BookVarintPut(body, 1);
    at += BookVarintPut(body + at, 1);
    at += RibbonIdsPut(body + at, g_golden_scope, 2);
    at += BookVarintPut(body + at, 4);
    return at + TallyBytesPut(body + at, (const uint8_t *)"snap", 4);
}

/* Every record of the golden stitch after its StitchHead, laid at `out` from
 * byte `at`, each kept in `rec`: where its Clasp goes. */
static uint32_t RibbonTestRecords(uint8_t *out, uint32_t at, RibbonTestRecord *rec)
{
    uint8_t  body[256], lists[128];
    TallyTag tag = { .flags = TALLY_TAG_HAS_VALUE, .key = (const uint8_t *)"key", .key_len = 3,
                     .value = (const uint8_t *)"val", .value_len = 3 };
    uint8_t  one = 0x07;
    uint32_t n;

    n   = RibbonUndoneBodyPut(body, 1, UNDONE_NOT_LANDED, g_golden_undone, 2);
    at += RibbonTestLay(out, at, RIBBON_UNDONE, 0, body, n, &rec[0]);
    n   = RibbonCoinedBodyPut(body, 42, &tag);
    at += RibbonTestLay(out, at, RIBBON_COINED, 0, body, n, &rec[1]);
    n   = RibbonTestBorn(body, lists);
    at += RibbonTestLay(out, at, RIBBON_BORN, 0, body, n, &rec[2]);
    n   = RibbonTestShaped(body, lists);
    at += RibbonTestLay(out, at, RIBBON_SHAPED, 0, body, n, &rec[3]);
    n   = RibbonTestLabeled(body, lists);
    at += RibbonTestLay(out, at, RIBBON_LABELED, 0, body, n, &rec[4]);
    n   = RibbonQuitclaimBodyPut(body, 8, 3);
    at += RibbonTestLay(out, at, RIBBON_QUITCLAIM, 0, body, n, &rec[5]);
    n   = TallyUsePut(body, (const uint8_t *)"work,play", 9);
    at += RibbonTestLay(out, at, RIBBON_USE_CONTEXT, 0, body, n, &rec[6]);
    n   = RibbonTestFrozen(body);
    at += RibbonTestLay(out, at, RIBBON_FROZEN, 0, body, n, &rec[7]);
    n   = BookVarintPut(body, 1);
    at += RibbonTestLay(out, at, RIBBON_THAWED, 0, body, n, &rec[8]);
    at += RibbonTestLay(out, at, 31, 1, &one, 1, &rec[9]);
    return at;
}

/* The golden stitch's stream into `out`, each record's place into `rec` and
 * every record's start into `starts` (StitchHead, the records, the Clasp):
 * its length. */
static uint32_t RibbonTestGoldenStream(uint8_t *out, RibbonTestRecord *rec, uint32_t *starts)
{
    uint8_t    body[64];
    StitchHead h = { .flags = STITCH_DATA_FLUSHED, .prev = 2, .prev_clasp = 0xC0FFEE11u, .tally = 4,
                     .made_ns = 1700000000123456789ull, .next_block = 3000, .next_blocks = 256,
                     .from_block = 2560, .from_blocks = 256 };
    uint32_t   n  = StitchHeadBodyPut(body, &h);
    uint32_t   at = RibbonTestLay(out, 0, RIBBON_STITCH_HEAD, 0, body, n, NULL);
    at = RibbonTestRecords(out, at, rec);
    starts[0] = 0;
    for (uint32_t i = 0; i < RIBBON_TEST_RECORDS; i++)
        starts[i + 1] = rec[i].at;
    starts[RIBBON_TEST_RECORDS + 1] = at;
    Clasp c = { .records = RIBBON_TEST_RECORDS, .fresh = 2, .crc = KCrc32c(out, at) };
    n = ClaspBodyPut(body, &c);
    return at + RibbonTestLay(out, at, RIBBON_CLASP, 0, body, n, NULL);
}

/* The golden stitch, sealed by its rule: the sum of its eight sectors is the
 * number volume_ribbon.h carries, which volume-read's self-test reaches by
 * stitch.py's own encoders — by the table and by the kernel's CRC32C. */
static TestResult TestRibbonGoldenStitch(void)
{
    TestResult       verdict = TEST_PASS;
    uint8_t          uuid[16];
    uint32_t         starts[RIBBON_TEST_RECORDS + 2];
    RibbonTestRecord rec[RIBBON_TEST_RECORDS];
    BookSectorHead   head;

    for (uint32_t i = 0; i < 16; i++)
        uuid[i] = (uint8_t)i;
    memset(g_test_output, 0, VOLUME_BLOCK_SECTORS * BOOK_PAYLOAD_BYTES);
    uint32_t len = RibbonTestGoldenStream(g_test_output, rec, starts);
    TEST_ASSERT(len <= VOLUME_BLOCK_SECTORS * BOOK_PAYLOAD_BYTES, "the golden stitch fits one block");
    TallyUnitSeal(g_test_buffer, g_test_output, len, starts, RIBBON_TEST_RECORDS + 2, BOOK_UNIT_STITCH, 7,
                  0x4000, VOLUME_BLOCK_SECTORS, uuid);
    TEST_ASSERT_EQ(~BookCrc32cOn(0xFFFFFFFFu, g_test_buffer, VOLUME_BLOCK_BYTES), RIBBON_GOLDEN_STITCH_CRC,
                   "the golden stitch's sum by the table");
    TEST_ASSERT_EQ(KCrc32c(g_test_buffer, VOLUME_BLOCK_BYTES), RIBBON_GOLDEN_STITCH_CRC,
                   "the golden stitch's sum by the kernel's own CRC32C");
    TEST_ASSERT(BookUnitCheck(g_test_buffer, VOLUME_BLOCK_SECTORS, uuid, 0x4000, &head),
                "it holds as one unit at its sectors");
    TEST_ASSERT(head.kind == BOOK_UNIT_STITCH && head.seq == 7 && head.lead == BOOK_HEAD_BYTES,
                "a STITCH of seq 7 whose first sector leads with its StitchHead");
out:
    return verdict;
}

/* A record's body read by the codec of its kind: what the read answered, the
 * bytes its known fields took in *used. */
static int RibbonTestGet(uint64_t kind, const uint8_t *b, uint32_t len, uint32_t *used)
{
    RibbonBorn    born;
    RibbonShaped  shaped;
    RibbonLabeled labeled;
    RibbonUndone  undone;
    TallyTag      tag;
    uint64_t      x, y;
    const uint8_t *text;
    switch (kind) {
    case RIBBON_UNDONE:      return RibbonUndoneGet(b, len, &undone, used);
    case RIBBON_COINED:      return RibbonCoinedGet(b, len, &x, &tag, used);
    case RIBBON_BORN:        return RibbonBornGet(b, len, 7, &born, used);
    case RIBBON_SHAPED:      return RibbonShapedGet(b, len, 7, &shaped, used);
    case RIBBON_LABELED:     return RibbonLabeledGet(b, len, &labeled, used);
    case RIBBON_QUITCLAIM:   return RibbonQuitclaimGet(b, len, &x, &y, used);
    case RIBBON_FROZEN:      return RibbonFrozenGet(b, len, &x, used);
    case RIBBON_THAWED:      return RibbonThawedGet(b, len, &x, used);
    case RIBBON_USE_CONTEXT: {
        int got = TallyUseGet(b, len, &text, &x);
        *used = got == TALLY_BODY_BROKEN ? 0 : TallyVarintBytes(x) + (uint32_t)x;
        return got;
    }
    default:                 return TALLY_BODY_BROKEN;
    }
}

/* Every record of the golden stitch, read back from its stream as a mount
 * reads it: all of its body known, every byte of it needed — a body cut one
 * byte short anywhere does not read — and one byte more is a newer writer's,
 * never stepped over in silence (§2.3, M-8). */
static TestResult TestRibbonRecordsCut(void)
{
    TestResult       verdict = TEST_PASS;
    uint32_t         starts[RIBBON_TEST_RECORDS + 2];
    RibbonTestRecord rec[RIBBON_TEST_RECORDS];
    uint8_t*         grown = g_test_buffer;

    RibbonTestGoldenStream(g_test_output, rec, starts);
    for (uint32_t i = 0; i + 1 < RIBBON_TEST_RECORDS; i++) {
        const uint8_t *b    = g_test_output + rec[i].body;
        uint32_t       len  = rec[i].len, used = 0;
        TEST_ASSERT_EQ(RibbonTestGet(rec[i].kind, b, len, &used), TALLY_BODY_KNOWN, "each record reads whole");
        TEST_ASSERT_EQ(used, len, "to the last byte of its body");
        for (uint32_t cut = 0; cut < len; cut++)
            TEST_ASSERT_EQ(RibbonTestGet(rec[i].kind, b, cut, &used), TALLY_BODY_BROKEN,
                           "and cut short anywhere it does not read");
        memcpy(grown, b, len);
        grown[len] = 0x01;
        TEST_ASSERT_EQ(RibbonTestGet(rec[i].kind, grown, len + 1u, &used), TALLY_BODY_LONGER,
                       "a byte past its fields is a newer writer's");
        TEST_ASSERT_EQ(used, len, "and its known fields end where they did");
    }
out:
    return verdict;
}

/* The fields a Born, a Shaped and a Labeled of the golden stitch give back:
 * numbers, name, tags in order, runs born at the stitch's seq with their
 * cursor carried across splices, digests and the Slip. */
static TestResult TestRibbonRecordsCircle(void)
{
    TestResult       verdict = TEST_PASS;
    uint32_t         starts[RIBBON_TEST_RECORDS + 2], used = 0, at = 0;
    RibbonTestRecord rec[RIBBON_TEST_RECORDS];
    RibbonBorn       born;
    RibbonShaped     shaped;
    RibbonLabeled    lab;
    RibbonSplice     sp;
    TallyRun         r;
    uint64_t         id = 0, cursor = 0, digests = 0;

    RibbonTestGoldenStream(g_test_output, rec, starts);
    TEST_ASSERT_EQ(RibbonBornGet(g_test_output + rec[2].body, rec[2].len, 7, &born, &used), TALLY_BODY_KNOWN,
                   "the Born reads");
    TEST_ASSERT(born.fid == 5 && born.size == 5000 && born.modified_ns - born.created_ns == 7 &&
                born.name_len == 6 && !memcmp(born.name, "golden", 6), "its numbers and its name");
    for (uint32_t k = 0; k < 3; k++)
        TEST_ASSERT(TallyIdNext(born.tags, born.tags_bytes, &at, &id, k == 0) && id == g_golden_tags[k],
                    "its tags, rising");
    at = 0;
    TEST_ASSERT(RibbonRunGet(born.runs, born.runs_bytes, &at, &cursor, 7, &r) && !r.hole && r.start == 1024 &&
                r.count == 1 && r.birth == 7, "a DATA run born at the stitch's seq");
    TEST_ASSERT(RibbonRunGet(born.runs, born.runs_bytes, &at, &cursor, 7, &r) && r.hole && r.count == 1,
                "then a hole");
    TEST_ASSERT(born.blocks == 2 && born.ndigests == 1 && TallyDigestAt(born.digests, 0) == 0x1122334455667788ull,
                "two blocks, one digest — the hole has none");
    TEST_ASSERT(born.slip.off == 100 && born.slip.len == 4 && !memcmp(born.slip.bytes, "tail", 4), "its Slip");
    TEST_ASSERT_EQ(RibbonShapedGet(g_test_output + rec[3].body, rec[3].len, 7, &shaped, &used), TALLY_BODY_KNOWN,
                   "the Shaped reads");
    at = 0;
    cursor = 0;
    TEST_ASSERT(RibbonSpliceNext(shaped.splices, shaped.splices_bytes, &at, &cursor, 7, &digests, &sp) &&
                sp.at == 1 && sp.remove == 1 && sp.ninsert == 1 && sp.blocks == 1, "its first splice");
    TEST_ASSERT(RibbonSpliceNext(shaped.splices, shaped.splices_bytes, &at, &cursor, 7, &digests, &sp) &&
                sp.at == 0 && sp.remove == 0 && sp.cursor == 4097, "its second, the cursor carried over");
    TEST_ASSERT(digests == 1 && shaped.ndigests == 1 && shaped.slip.len == 0, "one digest and no Slip");
    TEST_ASSERT_EQ(RibbonLabeledGet(g_test_output + rec[4].body, rec[4].len, &lab, &used), TALLY_BODY_KNOWN,
                   "the Labeled reads");
    TEST_ASSERT(lab.name_len == 7 && lab.set == 4 && lab.clear == 2 && lab.nadd == 1 && lab.ndrop == 1,
                "its name, flags and tags");
out:
    return verdict;
}

/* What a run's extensions do to the digests a record carries for it (§2.8):
 * a SHARED run none, a CODEC run its stored blocks, and a list of extensions
 * that does not walk makes the record not read. And the shape a stitch may
 * take (§16.3 п.8). */
static TestResult TestRibbonRunsAndShape(void)
{
    TestResult verdict = TEST_PASS;
    uint8_t    shared[] = { RUN_EXT_SHARED << 1, 0 };
    uint8_t    codec[]  = { RUN_EXT_CODEC << 1, 2, 9, 3 };
    uint8_t    broken[] = { RUN_EXT_CODEC << 1, 9 };
    TallyRun   r        = { .start = 100, .count = 5 };
    uint64_t   n        = 0;

    TEST_ASSERT(RibbonRunDigests(&r, &n) && n == 5, "a plain DATA run: a digest a block");
    r.ext = shared, r.ext_len = sizeof(shared), n = 0;
    TEST_ASSERT(RibbonRunDigests(&r, &n) && n == 0 && RibbonRunFresh(&r) == 0, "a SHARED run: none, nothing fresh");
    r.ext = codec, r.ext_len = sizeof(codec), n = 0;
    TEST_ASSERT(RibbonRunDigests(&r, &n) && n == 3 && RibbonRunFresh(&r) == 5, "a CODEC run: its stored blocks");
    r.ext = broken, r.ext_len = sizeof(broken);
    TEST_ASSERT(!RibbonRunDigests(&r, &n), "extensions that do not walk: the record does not read");
    TEST_ASSERT_EQ(RibbonShape(0, 2048, 0, 8), RIBBON_SHAPE_KEPT, "a block at a Quire's start");
    TEST_ASSERT_EQ(RibbonShape(8, 2040, 2048, 2048), RIBBON_SHAPE_KEPT, "one running on into the next Quire");
    TEST_ASSERT_EQ(RibbonShape(0, 2048, 0, 1), RIBBON_SHAPE_ATOMS, "one sector: no whole atom");
    TEST_ASSERT_EQ(RibbonShape(0, 2048, 0, 12), RIBBON_SHAPE_ATOMS, "a block and a half");
    TEST_ASSERT_EQ(RibbonShape(4, 2044, 0, 8), RIBBON_SHAPE_OFF, "begun off a block's boundary");
    TEST_ASSERT_EQ(RibbonShape(2040, 8, 0, 16), RIBBON_SHAPE_LONG, "longer than its Quire, no next named");
    TEST_ASSERT_EQ(RibbonShape(2040, 8, 8, 0xFFFFFFF0u), RIBBON_SHAPE_LONG, "longer than the two together");
    TEST_ASSERT_EQ(RibbonShape(0, 8801168, 0, 8801168), RIBBON_SHAPE_STREAM,
                   "a stream past 2^32 - 1 bytes is not read, however much room its Quires have");
    TEST_ASSERT_EQ(RibbonShape(0, 8801160, 0, 8801160), RIBBON_SHAPE_KEPT, "8801160 sectors: 4294966080 bytes");
out:
    return verdict;
}

/* What a unit's StitchHead names before it (ribbon_unit.c RibbonUnitNames,
 * stitch.py Unit.Names): a seq below its own — or nothing at all, where its
 * prev passes its own seq and the subtraction would wrap round to a seq above
 * it, so that two units past the end could name each other for ever. */
static TestResult TestRibbonUnitNames(void)
{
    TestResult verdict = TEST_PASS;
    RibbonUnit u;
    uint64_t   named = 99;
    memset(&u, 0, sizeof(u));
    u.seq = 10;
    TEST_ASSERT(!RibbonUnitNames(&u, &named), "a StitchHead that does not read names nothing");
    u.head_read = true;
    u.head.prev = 3;
    TEST_ASSERT(RibbonUnitNames(&u, &named) && named == 7, "prev 3 of seq 10 names seq 7");
    u.head.prev = 0;
    TEST_ASSERT(RibbonUnitNames(&u, &named) && named == 0, "prev 0 names none: the volume's first stitch");
    u.head.prev = 10;
    TEST_ASSERT(RibbonUnitNames(&u, &named) && named == 0, "prev 10 of seq 10 names seq 0");
    u.head.prev = 15;
    TEST_ASSERT(!RibbonUnitNames(&u, &named), "a prev past its own seq names no stitch at all");
    u.head.prev = UINT64_MAX;
    TEST_ASSERT(!RibbonUnitNames(&u, &named), "nor 2^64 - 1, whose wrap would name seq 11, above its own");
out:
    return verdict;
}

/* An Undone names a file by all 64 bits of its id (§16.5 п.5): over fids 5
 * and 2^32 + 7, the bytes volume-read's own codec lays
 * (tools/volumeread/selftest_book_errata.py, UNDONE_WIDE_GOLDEN) — and read
 * back as they were laid. */
static TestResult TestRibbonUndoneWide(void)
{
    static const uint64_t fids[2] = { 5, (1ull << 32) + 7u };
    static const uint8_t  golden[] = { 0x06, 0x09, 0x01, 0x01, 0x02, 0x05, 0x81, 0x80, 0x80, 0x80, 0x10 };
    TestResult   verdict = TEST_PASS;
    uint8_t      body[64], out[80];
    RibbonUndone d;
    uint32_t     used = 0, at = 0, len = RibbonUndoneBodyPut(body, 1, UNDONE_NOT_LANDED, fids, 2);
    uint32_t     n = RibbonRecordPut(out, RIBBON_UNDONE, 0, body, len);
    uint64_t     id = 0;
    TEST_ASSERT(n == sizeof(golden) && memcmp(out, golden, n) == 0, "the record is volume-read's, byte for byte");
    TEST_ASSERT(RibbonUndoneGet(body, len, &d, &used) != TALLY_BODY_BROKEN && d.nfid == 2, "it reads back");
    TEST_ASSERT(TallyIdNext(d.fids, d.fids_bytes, &at, &id, 1) && id == 5, "its first fid is 5");
    TEST_ASSERT(TallyIdNext(d.fids, d.fids_bytes, &at, &id, 0) && id == fids[1], "and its second 2^32 + 7");
out:
    return verdict;
}

static const TestCase g_ribbon_tests[] = {
    { .name = "golden_stitch",   .func = TestRibbonGoldenStitch,  .reach = TEST_READS_ONLY },
    { .name = "records_cut",     .func = TestRibbonRecordsCut,    .reach = TEST_READS_ONLY },
    { .name = "records_circle",  .func = TestRibbonRecordsCircle, .reach = TEST_READS_ONLY },
    { .name = "runs_and_shape",  .func = TestRibbonRunsAndShape,  .reach = TEST_READS_ONLY },
    { .name = "unit_names",      .func = TestRibbonUnitNames,     .reach = TEST_READS_ONLY },
    { .name = "undone_wide",     .func = TestRibbonUndoneWide,    .reach = TEST_READS_ONLY },
};

const TestSuite g_ribbon_suite = {
    .tests = g_ribbon_tests, .count = ARRAY_SIZE(g_ribbon_tests),
};
