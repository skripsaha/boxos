#include "tests.h"
#include "../tagfs.h"
#include "../file_map/file_record.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// RECORD_SPLICES — the change a stitch says was made of a file (file_record.h,
// FileRecordSplices; spec_v2 §2.8 Shaped, §3.7)
//
// The splices that make the record a file settled as into the one memory holds
// now name only what changed: a block written over in the middle of a run is
// one block taken out and one put in — the pieces of the old run on either side
// of it are the old run's still, never put in again as fresh data, which would
// ask the stitch for digests of blocks it did not write, count them fresh in
// its Clasp and give them a new birth. A cut takes the tail out and puts
// nothing in. And the splices applied to the settled record build the one
// memory holds, Strip for Strip — the record a mount's replay builds. Every
// record here is built in memory from Strips the test makes; nothing touches a
// medium or the volume's state.
// ============================================================================

#define SPLICES_MOST    4u
#define SPLICES_RUNS    600u
#define SPLICES_WRITES  24u

/* The record of the `n` Strips, as a file of id 7. */
static FileRecord* SplicesRecord(const Strip* strips, uint32_t n)
{
    static char   name[] = "splices";
    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    meta.file_id      = 7;
    meta.flags        = TAGFS_FILE_ACTIVE;
    meta.filename     = name;
    meta.extents      = (Strip*)strips;
    meta.extent_count = n;
    return FileRecordBuild(&meta);
}

/* Whether two records hold the same Strips, Strip for Strip. */
static bool SplicesSame(const FileRecord* a, const FileRecord* b)
{
    Strip   *fa = NULL, *fb = NULL;
    uint32_t na = 0, nb = 0;
    bool     same = FileRecordFlatten(a, &fa, &na) == OK && FileRecordFlatten(b, &fb, &nb) == OK && na == nb &&
                (na == 0 || memcmp(fa, fb, sizeof(Strip) * na) == 0);
    kfree(fa);
    kfree(fb);
    return same;
}

/* The splices from `was` to `now`, found and checked: applied to `was` they
 * build `now` Strip for Strip. Their count in *nsp, the blocks of DATA they
 * put in in *data, and a copy of the first `room` of them in out[] (NULL: not
 * kept). False when they were not found, or do not build `now`. */
static bool SplicesFound(const FileRecord* was, const FileRecord* now, uint64_t* nsp, uint64_t* data,
                         BookSplice* out, Strip* ins, uint32_t room)
{
    BookSplice* sp     = NULL;
    Strip*      strips = NULL;
    FileRecord* built  = NULL;
    uint64_t    count = 0, k = 0;
    bool        held   = FileRecordSplices(was, now, &sp, &count, &strips) == OK &&
                FileRecordSplice(was, sp, count, now->size, now->modified_time, &built, NULL, NULL) == OK &&
                SplicesSame(built, now);
    *nsp  = count;
    *data = 0;
    for (uint64_t i = 0; held && i < count; i++) {
        for (uint64_t j = 0; j < sp[i].ninsert; j++) {
            *data += sp[i].insert[j].kind == STRIP_DATA ? sp[i].insert[j].count : 0u;
            if (out && ins && k < room)
                ins[k++] = sp[i].insert[j];
        }
        if (out && i < room)
            out[i] = sp[i];
    }
    FileRecordRelease(built);
    kfree(sp);
    kfree(strips);
    return held;
}

static TestResult TestSplicesWrittenOver(void)
{
    TestResult  verdict = TEST_PASS;
    static const Strip was_s[] = { { 100, 10, STRIP_DATA, 7 }, { 500, 3, STRIP_DATA, 7 } };
    static const Strip now_s[] = { { 100, 5, STRIP_DATA, 7 }, { 900, 1, STRIP_DATA, 0 },
                                   { 106, 4, STRIP_DATA, 7 }, { 500, 3, STRIP_DATA, 7 } };
    BookSplice  sp[SPLICES_MOST];
    Strip       ins[SPLICES_MOST];
    uint64_t    nsp = 0, data = 0;
    FileRecord* was = SplicesRecord(was_s, 2);
    FileRecord* now = SplicesRecord(now_s, 4);

    TEST_ASSERT(was && now, "the two records are built");
    TEST_ASSERT(SplicesFound(was, now, &nsp, &data, sp, ins, SPLICES_MOST),
                "the splices between them are found, and build the new one Strip for Strip");
    TestEvidence("blocks of data the change of block 5 puts in", data);
    TEST_ASSERT(nsp == 1 && sp[0].at == 5 && sp[0].remove == 1 && sp[0].ninsert == 1,
                "block 5 written over is one splice: one block out, one in");
    TEST_ASSERT(ins[0].start == 900 && ins[0].count == 1 && data == 1,
                "and the one block in is the new one — the old run's pieces are not put in again");
out:
    FileRecordRelease(was);
    FileRecordRelease(now);
    return verdict;
}

static TestResult TestSplicesCutInside(void)
{
    TestResult  verdict = TEST_PASS;
    static const Strip was_s[] = { { 100, 10, STRIP_DATA, 7 }, { 500, 3, STRIP_DATA, 7 } };
    static const Strip now_s[] = { { 100, 5, STRIP_DATA, 7 } };
    static const Strip hole_was[] = { { 100, 2, STRIP_DATA, 7 }, { 0, 10, STRIP_HOLE, 7 } };
    static const Strip hole_now[] = { { 100, 2, STRIP_DATA, 7 }, { 0, 4, STRIP_HOLE, 7 } };
    BookSplice  sp[SPLICES_MOST];
    Strip       ins[SPLICES_MOST];
    uint64_t    nsp = 0, data = 0;
    FileRecord* was = SplicesRecord(was_s, 2);
    FileRecord* now = SplicesRecord(now_s, 1);
    FileRecord* hw  = SplicesRecord(hole_was, 2);
    FileRecord* hn  = SplicesRecord(hole_now, 2);

    TEST_ASSERT(was && now && hw && hn, "the records are built");
    TEST_ASSERT(SplicesFound(was, now, &nsp, &data, sp, ins, SPLICES_MOST), "a cut inside a run is found");
    TEST_ASSERT(nsp == 1 && sp[0].at == 5 && sp[0].remove == 8 && sp[0].ninsert == 0 && data == 0,
                "as its tail taken out, and nothing put in");
    TEST_ASSERT(SplicesFound(hw, hn, &nsp, &data, sp, ins, SPLICES_MOST), "a cut into a hole is found");
    TEST_ASSERT(nsp == 1 && sp[0].at == 6 && sp[0].remove == 6 && sp[0].ninsert == 0,
                "as the hole's tail taken out, and nothing put in");
out:
    FileRecordRelease(was);
    FileRecordRelease(now);
    FileRecordRelease(hw);
    FileRecordRelease(hn);
    return verdict;
}

static TestResult TestSplicesGrown(void)
{
    TestResult  verdict = TEST_PASS;
    static const Strip was_s[] = { { 100, 10, STRIP_DATA, 7 } };
    static const Strip now_s[] = { { 100, 10, STRIP_DATA, 7 }, { 110, 100, STRIP_DATA, 0 }, { 0, 3, STRIP_HOLE, 0 } };
    static const Strip hole_was[] = { { 100, 2, STRIP_DATA, 7 }, { 0, 5, STRIP_HOLE, 7 } };
    static const Strip hole_now[] = { { 100, 2, STRIP_DATA, 7 }, { 0, 8, STRIP_HOLE, 0 } };
    BookSplice  sp[SPLICES_MOST];
    Strip       ins[SPLICES_MOST];
    uint64_t    nsp = 0, data = 0;
    FileRecord* was = SplicesRecord(was_s, 1);
    FileRecord* now = SplicesRecord(now_s, 3);
    FileRecord* hw  = SplicesRecord(hole_was, 2);
    FileRecord* hn  = SplicesRecord(hole_now, 2);

    TEST_ASSERT(was && now && hw && hn, "the records are built");
    TEST_ASSERT(SplicesFound(was, now, &nsp, &data, sp, ins, SPLICES_MOST), "a growth is found");
    TEST_ASSERT(nsp == 1 && sp[0].at == 10 && sp[0].remove == 0 && sp[0].ninsert == 2 && data == 100,
                "as what it adds put in at the end, the run it goes on from kept");
    TEST_ASSERT(SplicesFound(hw, hn, &nsp, &data, sp, ins, SPLICES_MOST), "a hole grown longer is found");
    TEST_ASSERT(nsp == 1 && sp[0].at == 2 && sp[0].ninsert == 1 && ins[0].kind == STRIP_HOLE && data == 0,
                "as the longer hole put in, owning no ground");
out:
    FileRecordRelease(was);
    FileRecordRelease(now);
    FileRecordRelease(hw);
    FileRecordRelease(hn);
    return verdict;
}

/* SPLICES_RUNS Strips of 2 to 5 blocks, none meeting another on the medium. */
static void SplicesManyRuns(Strip* out)
{
    uint64_t at = 1000;
    for (uint32_t i = 0; i < SPLICES_RUNS; i++) {
        uint32_t n = 2u + (i * 7u) % 4u;
        out[i]     = (Strip){ .start = at, .count = n, .kind = STRIP_DATA, .birth = 3 };
        at        += n + 2u;
    }
}

/* SPLICES_WRITES blocks, each inside a Strip of its own, written over by
 * FileRecordSplice one after another from `rec`: the record at the end, or
 * NULL. */
static FileRecord* SplicesWrittenOver(FileRecord* rec)
{
    uint64_t k = 1;
    for (uint32_t i = 0; i < SPLICES_WRITES && rec; i++, k += 7u * 3u) {
        Strip       fresh = { .start = 800000u + i, .count = 1, .kind = STRIP_DATA };
        BookSplice  one   = { .at = k, .remove = 1, .ninsert = 1, .insert = &fresh };
        FileRecord* next  = NULL;
        if (FileRecordSplice(rec, &one, 1, rec->size, rec->modified_time, &next, NULL, NULL) != OK)
            next = NULL;
        FileRecordRelease(rec);
        rec = next;
    }
    return rec;
}

static TestResult TestSplicesManyRuns(void)
{
    TestResult  verdict = TEST_PASS;
    static Strip runs[SPLICES_RUNS];
    uint64_t    nsp = 0, data = 0;
    FileRecord* was = NULL;
    FileRecord* now = NULL;

    SplicesManyRuns(runs);
    was = SplicesRecord(runs, SPLICES_RUNS);
    now = SplicesRecord(runs, SPLICES_RUNS);
    TEST_ASSERT(was && now && was->strip_count == SPLICES_RUNS, "a file of six hundred runs is built");
    now = SplicesWrittenOver(now);
    TEST_ASSERT(now != NULL, "and 24 blocks of it, each inside a run, written over one by one");
    TEST_ASSERT(SplicesFound(was, now, &nsp, &data, NULL, NULL, 0),
                "the splices from the file before to the file after build it Strip for Strip");
    TestEvidence("blocks of data 24 blocks written over put in", data);
    TEST_ASSERT(nsp == SPLICES_WRITES && data == SPLICES_WRITES,
                "one splice a block written over, each putting in that block alone");
out:
    FileRecordRelease(was);
    FileRecordRelease(now);
    return verdict;
}

static const TestCase g_record_splices_tests[] = {
    { .name = "record_splices_written_over", .func = TestSplicesWrittenOver, .reach = TEST_READS_ONLY },
    { .name = "record_splices_cut_inside",   .func = TestSplicesCutInside,   .reach = TEST_READS_ONLY },
    { .name = "record_splices_grown",        .func = TestSplicesGrown,       .reach = TEST_READS_ONLY },
    { .name = "record_splices_many_runs",    .func = TestSplicesManyRuns,    .reach = TEST_READS_ONLY },
};

const TestSuite g_record_splices_suite = {
    g_record_splices_tests, sizeof(g_record_splices_tests) / sizeof(g_record_splices_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
