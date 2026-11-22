#include "stand_binder.h"
#include "stand_fresh.h"
#include "../dedup/conjugate.h"
#include "../keep/keep.h"
#include "../settle/settle.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_made_by suite: a file remembers the program that made it
//
// A file made by a program of the volume carries the id of that program's
// file in its record — made_by, with the bit TAGFS_FILE_MADE_BY — from its
// birth, and no change of the file moves it: a write, a span, a tag, a rename,
// a defrag, a deletion for a term and its undoing, a duplicate laid on
// another's blocks. A file no program made says so (0, no bit). The Born of
// the ribbon and the FILES header of a Tally carry it, so a volume mounted
// again — from the Tally, from the ribbon after it — knows it as it was.
// ============================================================================

#define STAND_MADE_BY_PROGRAM  4711u
#define STAND_MADE_BY_OTHER    4712u
#define STAND_MADE_BY_TERM_S   3600ull
#define STAND_MADE_BY_BLOCKS   3u
#define STAND_MADE_BY_STREAM   0x4d42u

static StandBinder s_made_by_binder;

static TestResult TestStandMadeByRecorded(void)
{
    TestResult verdict = TEST_PASS;
    uint64_t   made = 0, none = 0;
    TEST_ASSERT_EQ(tagfs_create_file("madeby-made.bin", NULL, 0, STAND_MADE_BY_PROGRAM, &made), 0,
                   "a file is made by a program");
    TEST_ASSERT_EQ(tagfs_create_file("madeby-none.bin", NULL, 0, 0, &none), 0, "a file is made by no program");
    TEST_ASSERT(StandMadeByIs(made, STAND_MADE_BY_PROGRAM), "its record names the program, its bit on");
    TEST_ASSERT(StandMadeByIs(none, 0), "the other's names none, its bit off");
    TEST_ASSERT(!StandMadeByIs(made, 0) && !StandMadeByIs(none, STAND_MADE_BY_PROGRAM), "and neither is the other's");
    (void)tagfs_delete_file(made, INK_WARRANTED);
    (void)tagfs_delete_file(none, INK_WARRANTED);
out:
    return verdict;
}

static TestResult TestStandMadeByEveryChange(void)
{
    TestResult  verdict = TEST_PASS;
    uint64_t    fid = 0;
    uint32_t    tag = 0;
    const char* name = "madeby-change.bin";
    TEST_ASSERT_EQ(tagfs_create_file(name, NULL, 0, STAND_MADE_BY_PROGRAM, &fid), 0, "a file is made by a program");
    TEST_ASSERT_EQ(StandWriteAt(fid, 0, "abcd", 4), 4, "it is written");
    TEST_ASSERT(StandMadeByIs(fid, STAND_MADE_BY_PROGRAM), "and the program that made it is the same");
    TEST_ASSERT_OK(tagfs_span_file(fid, STAND_MADE_BY_BLOCKS * TAGFS_BLOCK_SIZE), "it is spanned");
    TEST_ASSERT(StandMadeByIs(fid, STAND_MADE_BY_PROGRAM), "the same");
    TEST_ASSERT_EQ(StandWriteAt(fid, TAGFS_BLOCK_SIZE, "efgh", 4), 4, "its second block is written");
    TEST_ASSERT_OK(tagfs_defrag_file(fid), "it is defragmented");
    TEST_ASSERT(StandMadeByIs(fid, STAND_MADE_BY_PROGRAM), "the same");
    TEST_ASSERT_OK(tagfs_tag_intern("madeby:tag", &tag), "a tag is interned");
    TEST_ASSERT_EQ(tagfs_add_tag(fid, tag), 0, "it is put on the file");
    TEST_ASSERT(StandMadeByIs(fid, STAND_MADE_BY_PROGRAM), "the same");
    TEST_ASSERT_EQ(tagfs_remove_tag(fid, tag), 0, "and taken off");
    TEST_ASSERT_EQ(tagfs_rename_file(fid, "madeby-renamed.bin"), 0, "the file is renamed");
    TEST_ASSERT(StandMadeByIs(fid, STAND_MADE_BY_PROGRAM), "the same");
    TEST_ASSERT_EQ(tagfs_keep_file(fid, STAND_MADE_BY_TERM_S, INK_WARRANTED), 0, "it is deleted for a term");
    TEST_ASSERT(StandMadeByIs(fid, STAND_MADE_BY_PROGRAM), "the same, deleted");
    TEST_ASSERT_EQ(tagfs_restore_file(fid, INK_WARRANTED), 0, "and brought back");
    TEST_ASSERT(StandMadeByIs(fid, STAND_MADE_BY_PROGRAM), "the same");
    (void)tagfs_delete_file(fid, INK_WARRANTED);
out:
    return verdict;
}

/* Every file of the test, as it must read after a mount: `made` by the
 * program, `none` by none, `kept` deleted for a term and made by the other. */
static bool StandMadeByHolds(uint64_t made, uint64_t none, uint64_t kept)
{
    return StandMadeByIs(made, STAND_MADE_BY_PROGRAM) && StandMadeByIs(none, 0) &&
           StandMadeByIs(kept, STAND_MADE_BY_OTHER);
}

static TestResult TestStandMadeByMounted(void)
{
    TestResult   verdict = TEST_PASS;
    StandBinder* b       = &s_made_by_binder;
    uint64_t     made = 0, none = 0, kept = 0, late = 0;
    const char*  aside = StandBinderAside(b);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT_EQ(tagfs_create_file("madeby-one.bin", NULL, 0, STAND_MADE_BY_PROGRAM, &made), 0,
                   "a file is made by a program");
    TEST_ASSERT_EQ(tagfs_create_file("madeby-two.bin", NULL, 0, 0, &none), 0, "another by none");
    TEST_ASSERT_EQ(tagfs_create_file("madeby-three.bin", NULL, 0, STAND_MADE_BY_OTHER, &kept), 0,
                   "a third by another program");
    TEST_ASSERT_EQ(tagfs_keep_file(kept, STAND_MADE_BY_TERM_S, INK_WARRANTED), 0, "and the third is deleted for a term");
    TEST_ASSERT_OK(TagFSSettle(), "the ribbon lays their Borns and the Labeled KEEP");
    TEST_ASSERT(StandStitchRemount(), "the volume is mounted afresh, its ribbon replayed");
    TEST_ASSERT(StandMadeByHolds(made, none, kept), "each file is made by the program it was");
    TEST_ASSERT_OK(StandBinderTallies(1u), "a Tally is laid: the headers carry the programs, one of them past its term");
    TEST_ASSERT(StandStitchRemount(), "the volume is mounted afresh on that Tally");
    TEST_ASSERT(StandMadeByHolds(made, none, kept), "each file is made by the program it was");
    TEST_ASSERT_EQ(tagfs_create_file("madeby-late.bin", NULL, 0, STAND_MADE_BY_OTHER, &late), 0,
                   "a file is made after the Tally");
    TEST_ASSERT_EQ(tagfs_rename_file(made, "madeby-first.bin"), 0, "the first is renamed");
    TEST_ASSERT_EQ(tagfs_restore_file(kept, INK_WARRANTED), 0, "the third is brought back");
    TEST_ASSERT_OK(TagFSSettle(), "the ribbon lays them past the Tally");
    TEST_ASSERT(StandStitchRemount(), "the volume is mounted afresh: the Tally read, the ribbon replayed over it");
    TEST_ASSERT(StandMadeByHolds(made, none, kept) && StandMadeByIs(late, STAND_MADE_BY_OTHER),
                "each file is made by the program it was, the late one by its own");
out:
    STAND_BINDER_PUT_BACK(b);
    return verdict;
}

static TestResult TestStandMadeByLaidOn(void)
{
    TestResult      verdict = TEST_PASS;
    StandBinder*    b       = &s_made_by_binder;
    StandFreshModel m;
    ConjugateSaid   said;
    ConjugateAsk    ask;
    uint64_t        a = 0, f = 0;
    const char*     aside = StandBinderAside(b);

    memset(&m, 0, sizeof(m));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, STAND_MADE_BY_BLOCKS * TAGFS_BLOCK_SIZE), "room for the files' bytes");
    TEST_ASSERT_EQ(tagfs_create_file("madeby-orig.bin", NULL, 0, STAND_MADE_BY_OTHER, &a), 0, "the original is made");
    TEST_ASSERT_EQ(StandFreshWrite(&m, a, 0, STAND_MADE_BY_BLOCKS * TAGFS_BLOCK_SIZE, STAND_MADE_BY_STREAM),
                   (int)(STAND_MADE_BY_BLOCKS * TAGFS_BLOCK_SIZE), "and written");
    m.size = 0;
    TEST_ASSERT_EQ(tagfs_create_file("madeby-copy.bin", NULL, 0, STAND_MADE_BY_PROGRAM, &f), 0, "the copy is made");
    TEST_ASSERT_EQ(StandFreshWrite(&m, f, 0, STAND_MADE_BY_BLOCKS * TAGFS_BLOCK_SIZE, STAND_MADE_BY_STREAM),
                   (int)(STAND_MADE_BY_BLOCKS * TAGFS_BLOCK_SIZE), "and written with the original's very bytes");
    TEST_ASSERT_OK(TagFSSettle(), "both settled");
    ask = (ConjugateAsk){ .fid = f, .at = 0, .onto = a, .onto_at = 0, .count = STAND_MADE_BY_BLOCKS };
    TEST_ASSERT_OK(TagFSConjugate(INK_WARRANTED, &ask, &said), "the copy's blocks are laid on the original's");
    TEST_ASSERT_EQ(said.why, CONJUGATE_LAID, "the act says it laid them");
    TEST_ASSERT(StandMadeByIs(f, STAND_MADE_BY_PROGRAM) && StandMadeByIs(a, STAND_MADE_BY_OTHER),
                "each is made by the program that made it still");
    TEST_ASSERT(StandFreshSettledAndBack(&m, f), "mounted again, the copy reads its bytes");
    TEST_ASSERT(StandMadeByIs(f, STAND_MADE_BY_PROGRAM) && StandMadeByIs(a, STAND_MADE_BY_OTHER),
                "and each is made by the program that made it");
out:
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(b);
    return verdict;
}

static const TestCase g_stand_made_by_tests[] = {
    { .name = "stand_made_by_recorded", .func = TestStandMadeByRecorded,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_made_by_every_change", .func = TestStandMadeByEveryChange,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_made_by_mounted", .func = TestStandMadeByMounted,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_made_by_laid_on", .func = TestStandMadeByLaidOn,
      .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_made_by_suite = {
    g_stand_made_by_tests, sizeof(g_stand_made_by_tests) / sizeof(g_stand_made_by_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
