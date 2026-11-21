#include "tests.h"
#include "stand_tests.h"
#include "stand_stitch.h"
#include "../tagfs.h"
#include "../keep/keep.h"
#include "../settle/settle.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_keep suite: a deleted file kept for a term (keep/keep.h)
//
// A file deleted for a term is a state of its record, not a tag: no query
// meets it, the one question for the deleted ones does, its changes are
// refused, it can be brought back until its term ends, and the volume lets it
// go by itself when the term has ended — by KeepSweepNow, which the service
// and the first look after a mount run — with its name and its term as they
// were laid when the volume is mounted again.
//
// ‼ THE CLOCK IS THE WALL CLOCK, so a term is never held to a second: the
// terms here are an hour, and a sweep is asked as of two hours on — the sweep
// takes the moment it is asked as of, exactly as the service takes the
// clock's.
// ============================================================================

#define STAND_KEEP_TERM_S   3600ull
#define STAND_KEEP_PAGE     16u

static const char s_stem[] = "keptfile";
static const char s_name[] = "keptfile.dat";

/* Whether `fid` is among the files a question names: the live ones for
 * `kept` false, the deleted ones for true. */
static bool StandKeepAmong(uint64_t fid, bool kept)
{
    TagSpelling stem = { .key = s_stem, .key_len = sizeof(s_stem) - 1u };
    uint64_t    ids[STAND_KEEP_PAGE];
    TagFSPage   page;
    error_t     rc = kept ? TagFSQueryKeptPage(TagOwnerPerson(), &stem, 1, NULL, 0, 1, ids, STAND_KEEP_PAGE, false, &page)
                          : TagFSQueryPage(TagOwnerPerson(), &stem, 1, NULL, 0, 1, ids, STAND_KEEP_PAGE, false, &page);
    for (uint32_t i = 0; rc == OK && i < page.delivered; i++)
        if (ids[i] == fid)
            return true;
    return false;
}

/* Whether the volume lists `fid` among every file it has, as a question of no
 * tags does. */
static bool StandKeepListed(uint64_t fid)
{
    uint64_t  ids[STAND_KEEP_PAGE];
    uint64_t  from = 1;
    TagFSPage page;
    while (TagFSListPage(from, ids, STAND_KEEP_PAGE, false, &page) == OK) {
        for (uint32_t i = 0; i < page.delivered; i++)
            if (ids[i] == fid)
                return true;
        if (!page.next)
            break;
        from = page.next;
    }
    return false;
}

static TestResult TestStandKeepHidesFromQueries(void)
{
    TestResult  verdict = TEST_PASS;
    uint64_t    fid = 0;
    TagFSMetadata md;
    TEST_ASSERT(StandMakeFile(s_name, "kept", 4, &fid), "a file is made");
    TEST_ASSERT(StandKeepAmong(fid, false) && StandKeepListed(fid), "a live file is found by its tag and listed");
    TEST_ASSERT(!StandKeepAmong(fid, true), "and the question for the deleted ones does not meet it");
    TEST_ASSERT_EQ(tagfs_keep_file(fid, STAND_KEEP_TERM_S, INK_WARRANTED), 0, "the file is deleted for a term");
    TEST_ASSERT(!StandKeepAmong(fid, false), "no query by its tag meets it");
    TEST_ASSERT(!StandKeepListed(fid), "a question of no tags does not list it");
    TEST_ASSERT(StandKeepAmong(fid, true), "the question for the deleted ones meets it");
    TEST_ASSERT_EQ(tagfs_get_metadata(fid, &md), 0, "its record is there for whoever holds its id");
    TEST_ASSERT((md.flags & TAGFS_FILE_DELETED) && md.keep_until > KeepNowNs() &&
                md.keep_until <= KeepUntil(STAND_KEEP_TERM_S),
                "and says it is deleted, kept until the term's end");
    tagfs_metadata_free(&md);
    (void)tagfs_delete_file(fid, INK_WARRANTED);
out:
    return verdict;
}

static TestResult TestStandKeepRestore(void)
{
    TestResult    verdict = TEST_PASS;
    uint64_t      fid = 0;
    TagFSMetadata md;
    TEST_ASSERT(StandMakeFile(s_name, "kept", 4, &fid), "a file is made");
    TEST_ASSERT_EQ(tagfs_keep_file(fid, STAND_KEEP_TERM_S, INK_WARRANTED), 0, "the file is deleted for a term");
    TEST_ASSERT(StandWriteAt(fid, 0, "gone", 4) < 4, "a write to a deleted file is refused");
    TEST_ASSERT_EQ(tagfs_rename_file(fid, "keptfile.tmp"), -(int)ERR_FILE_NOT_FOUND, "and so is its rename");
    TEST_ASSERT_EQ(tagfs_restore_file(fid, INK_WARRANTED), 0, "the file is brought back");
    TEST_ASSERT(StandKeepAmong(fid, false) && !StandKeepAmong(fid, true), "queries find it again, the deleted ones' do not");
    TEST_ASSERT_EQ(tagfs_get_metadata(fid, &md), 0, "its record is there");
    TEST_ASSERT(!(md.flags & TAGFS_FILE_DELETED) && md.keep_until == 0, "and says nothing of a term");
    tagfs_metadata_free(&md);
    TEST_ASSERT_EQ(tagfs_restore_file(fid, INK_WARRANTED), 0, "a file that is not deleted is restored as nothing");
    TEST_ASSERT_EQ(StandWriteAt(fid, 0, "back", 4), 4, "and it is written again");
    (void)tagfs_delete_file(fid, INK_WARRANTED);
out:
    return verdict;
}

static TestResult TestStandKeepTermEnds(void)
{
    TestResult verdict = TEST_PASS;
    uint64_t   fid = 0;
    TEST_ASSERT(StandMakeFile(s_name, "kept", 4, &fid), "a file is made");
    TEST_ASSERT_EQ(tagfs_keep_file(fid, STAND_KEEP_TERM_S, INK_WARRANTED), 0, "the file is deleted for a term");
    TEST_ASSERT_EQ(KeepSweepNow(KeepNowNs()), 0u, "a sweep within the term lets nothing go");
    TEST_ASSERT(StandKeepAmong(fid, true), "the file is kept");
    TEST_ASSERT_EQ(tagfs_keep_file(fid, 2u * STAND_KEEP_TERM_S, INK_WARRANTED), 0, "deleted again, its term is moved");
    TEST_ASSERT_EQ(KeepSweepNow(KeepNowNs() + (STAND_KEEP_TERM_S + 60u) * 1000000000ull), 0u,
                   "a sweep past the first term and within the second lets nothing go");
    TEST_ASSERT_EQ(KeepSweepNow(KeepNowNs() + (2u * STAND_KEEP_TERM_S + 60u) * 1000000000ull), 1u,
                   "a sweep past the term lets the file go");
    TEST_ASSERT(!StandKeepAmong(fid, true) && !StandKeepAmong(fid, false), "nothing meets it");
    TEST_ASSERT_EQ(tagfs_restore_file(fid, INK_WARRANTED), -(int)ERR_FILE_NOT_FOUND, "and it cannot be brought back");
out:
    return verdict;
}

static TestResult TestStandKeepAtOnce(void)
{
    TestResult    verdict = TEST_PASS;
    uint64_t      fid = 0;
    TagFSMetadata md;
    TEST_ASSERT(StandMakeFile(s_name, "kept", 4, &fid), "a file is made");
    TEST_ASSERT_EQ(tagfs_keep_file(fid, 0, INK_WARRANTED), 0, "a term of 0 gives the file up at once");
    TEST_ASSERT(!StandKeepAmong(fid, true) && !StandKeepAmong(fid, false), "no question meets it");
    TEST_ASSERT(tagfs_get_metadata(fid, &md) != 0, "and the volume holds no record of it");
out:
    return verdict;
}

static TestResult TestStandKeepSurvivesAMount(void)
{
    TestResult    verdict = TEST_PASS;
    uint64_t      fid = 0, until = 0;
    TagFSMetadata md;
    TEST_ASSERT(StandMakeFile(s_name, "kept", 4, &fid), "a file is made");
    TEST_ASSERT_EQ(tagfs_keep_file(fid, STAND_KEEP_TERM_S, INK_WARRANTED), 0, "the file is deleted for a term");
    TEST_ASSERT_EQ(tagfs_get_metadata(fid, &md), 0, "its record is there");
    until = md.keep_until;
    tagfs_metadata_free(&md);
    TEST_ASSERT_EQ(TagFSSettle(), OK, "the change is settled: the ribbon lays it");
    TEST_ASSERT(StandStitchRemount(), "the volume is mounted afresh, its ribbon replayed");
    TEST_ASSERT(!StandKeepAmong(fid, false) && StandKeepAmong(fid, true), "the file is still deleted for the mount that read it");
    TEST_ASSERT_EQ(tagfs_get_metadata(fid, &md), 0, "its record is there");
    TEST_ASSERT((md.flags & TAGFS_FILE_DELETED) && md.keep_until == until, "with the same term");
    tagfs_metadata_free(&md);
    TEST_ASSERT_EQ(KeepSweepNow(until + 1u), 1u, "a mount that comes after the term lets the file go");
    TEST_ASSERT(!StandKeepAmong(fid, true), "nothing meets it");
    TEST_ASSERT_EQ(TagFSSettle(), OK, "the Quitclaim is settled");
    TEST_ASSERT(StandStitchRemount(), "and mounted afresh again");
    TEST_ASSERT(!StandKeepAmong(fid, true) && !StandKeepAmong(fid, false), "the volume holds no file by that name");
out:
    return verdict;
}

static const TestCase g_stand_keep_tests[] = {
    { .name = "stand_keep_hides_from_queries", .func = TestStandKeepHidesFromQueries,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_keep_restore", .func = TestStandKeepRestore,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_keep_term_ends", .func = TestStandKeepTermEnds,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_keep_at_once", .func = TestStandKeepAtOnce,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_keep_survives_a_mount", .func = TestStandKeepSurvivesAMount,
      .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_keep_suite = {
    g_stand_keep_tests, sizeof(g_stand_keep_tests) / sizeof(g_stand_keep_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
