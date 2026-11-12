#include "stand_binder.h"

#ifdef CONFIG_VOLUME_TESTS

#include "stand_fresh.h"
#include "../dedup/conjugate.h"
#include "../dedup/dedup.h"
#include "../ink/ink.h"
#include "../ink/commonhold.h"
#include "../mount/book_mount.h"
#include "../mount/under_glass.h"

// ============================================================================
// The stand_dedup suite: duplicates found and laid by the pass after each
// Tally (spec_v2 §8.2, §16.7 пп.7–9), and a defrag that keeps what is shared
//
// The pass Glean runs after a Tally weighs the blocks written since the Tally
// before: a block whose digest another's matches — one written beside it, or
// one the book's DIGESTS names — is a candidate, and the bytes decide
// (Conjugate). The copy is laid on the original, the older of the two, and
// its own blocks go back: the volume holds the bytes once, both files read
// them, the census of the mount after it finds every block named as memory
// holds it. A file wearing dedup:off, a file the system keeps, and the kernel
// the loader reads by its blocks are neither laid nor laid on — passed over
// before any act is asked, so the pass refuses nothing it could have known.
// The runner holds the pass down while the tests run (DedupHold), as it holds
// the cadence: each test here lets it go and holds it again.
// ============================================================================

#define STAND_DEDUP_BLOCKS   16u
#define STAND_DEDUP_STREAM   0xded0u
#define STAND_DEDUP_OTHER    0xded1u
#define STAND_DEDUP_TALLIES  24u      /* Tallies the walk of the book's DIGESTS may take to reach the original */

static StandBinder s_dedup_binder;

/* A file of `blocks` blocks of `stream` — the model's bytes — its id in *fid. */
static bool StandDedupFile(StandFreshModel* m, const char* name, uint32_t blocks, uint64_t stream, uint64_t* fid)
{
    m->size = 0;
    return tagfs_create_file(name, NULL, 0, fid) == 0 &&
           StandFreshWrite(m, *fid, 0, blocks * TAGFS_BLOCK_SIZE, stream) == (int)(blocks * TAGFS_BLOCK_SIZE);
}

/* The blocks the file's first `n` lie on, into `out`. */
static bool StandDedupBlocks(uint64_t fid, uint32_t n, uint64_t* out)
{
    for (uint32_t k = 0; k < n; k++)
        if (!StandBlockOf(fid, k, &out[k]))
            return false;
    return true;
}

/* Whether the file's first `n` blocks are `onto`, each named by two. */
static bool StandDedupOnto(uint64_t fid, const uint64_t* onto, uint32_t n)
{
    uint64_t block = 0;
    for (uint32_t k = 0; k < n; k++)
        if (!StandBlockOf(fid, k, &block) || block != onto[k] || CommonholdOwners(block) != 2u)
            return false;
    return true;
}

/* Whether the file's first `n` blocks are still `mine`, its own alone. */
static bool StandDedupOwn(uint64_t fid, const uint64_t* mine, uint32_t n)
{
    uint64_t block = 0;
    for (uint32_t k = 0; k < n; k++)
        if (!StandBlockOf(fid, k, &block) || block != mine[k] || CommonholdOwners(block) != 0)
            return false;
    return true;
}

/* Whether none of the `n` blocks is in use. */
static bool StandDedupFree(const uint64_t* blocks, uint32_t n)
{
    for (uint32_t k = 0; k < n; k++)
        if (StandBlockInUse(blocks[k]))
            return false;
    return true;
}

/* `n` Tallies, each with the pass after it run to its end, the counts of
 * every pass summed into *said: OK, or the first refusal. */
static error_t StandDedupTallies(uint32_t n, DedupSaid* said)
{
    DedupSaid one;
    error_t   rc = OK;
    memset(said, 0, sizeof(*said));
    for (uint32_t t = 0; rc == OK && t < n; t++) {
        rc = StandBinderTallies(1u);
        if (rc == OK && !DedupStandPassed(&one))
            rc = ERR_TIMEOUT;
        said->passes  += one.passes;
        said->weighed += one.weighed;
        said->among   += one.among;
        said->book    += one.book;
        said->laid    += one.laid;
        said->refused += one.refused;
        said->passed  += one.passed;
    }
    return rc;
}

static const TagSpelling s_dedup_off = { .key = "dedup", .value = "off", .key_len = 5, .value_len = 3 };

// ----------------------------------------------------------------------------
// A copy written beside its original: laid on it by the pass after the Tally
// ----------------------------------------------------------------------------

static TestResult TestStandDedupCopy(void)
{
    TestResult      verdict = TEST_PASS;
    StandBinder*    b       = &s_dedup_binder;
    StandFreshModel m;
    DedupSaid       said;
    MountBookSaid   mounted;
    uint64_t        a = 0, f = 0, mine[STAND_DEDUP_BLOCKS], theirs[STAND_DEDUP_BLOCKS];
    const char*     aside = StandBinderAside(b);

    memset(&m, 0, sizeof(m));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(DedupStandPassed(&said) && DedupHold(false), "Dedup's own pass, held by the runner, is let go");
    TEST_ASSERT(StandFreshModelMake(&m, STAND_DEDUP_BLOCKS * TAGFS_BLOCK_SIZE), "room for the files' bytes");
    TEST_ASSERT(StandDedupFile(&m, "dedup-copy-a.bin", STAND_DEDUP_BLOCKS, STAND_DEDUP_STREAM, &a),
                "file A is made and written");
    TEST_ASSERT(StandDedupFile(&m, "dedup-copy-b.bin", STAND_DEDUP_BLOCKS, STAND_DEDUP_STREAM, &f),
                "file B is made and written with A's very bytes after it");
    TEST_ASSERT_OK(TagFSSettle(), "both settled");
    TEST_ASSERT(StandDedupBlocks(a, STAND_DEDUP_BLOCKS, theirs) && StandDedupBlocks(f, STAND_DEDUP_BLOCKS, mine) &&
                    mine[0] != theirs[0], "each on its own ground");
    TEST_ASSERT_OK(StandDedupTallies(1u, &said), "a Tally, and the pass Glean runs after it");
    TEST_ASSERT_EQ(said.laid, STAND_DEDUP_BLOCKS, "the pass lays every block of B on A's");
    TEST_ASSERT_EQ(said.refused, 0, "and no act it asked is refused");
    TEST_ASSERT(said.among >= STAND_DEDUP_BLOCKS, "matched among the blocks written since the Tally before");
    TEST_ASSERT(StandDedupOnto(f, theirs, STAND_DEDUP_BLOCKS), "B's blocks are A's, each named by two: A is older");
    TEST_ASSERT(StandFreshHolds(&m, f) && StandFreshHolds(&m, a), "both read their bytes, every one");
    TEST_ASSERT_OK(TagFSSettle(), "the pass's changes settled");
    TEST_ASSERT_OK(StandBookRoom(), "room is asked for: the ribbon leaves its Quire, the lag's stitches past it");
    TEST_ASSERT(StandDedupFree(mine, STAND_DEDUP_BLOCKS),
                "the blocks B wrote are free again: the volume's free ground grew by them");
    TEST_ASSERT(StandFreshSettledAndBack(&m, f) && StandFreshHolds(&m, a),
                "mounted again on it, B has the runs memory held and both read their bytes");
    MountBookStandSaid(&mounted);
    TEST_ASSERT(mounted.doubles == 0 && !TagFSIsUnderGlass(),
                "the census finds no block two owners name, and the volume is written on");
    TEST_ASSERT(StandDedupOnto(f, theirs, STAND_DEDUP_BLOCKS), "B's blocks are A's still, two owners on each");
out:
    (void)DedupHold(true);
    if (f)
        (void)tagfs_delete_file(f, INK_UNWARRANTED);
    if (a)
        (void)tagfs_delete_file(a, INK_UNWARRANTED);
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(b);
    return verdict;
}

// ----------------------------------------------------------------------------
// A copy of a file the book already holds: found by the walk of its DIGESTS
// ----------------------------------------------------------------------------

/* The copy's blocks pay for the walk: as many Sheets of DIGESTS a pass as they
 * take on the medium, eight a block — 512 Sheets, more than the stand's whole
 * book, so a pass or two go round it. */
#define STAND_DEDUP_OLD  64u

static TestResult TestStandDedupOld(void)
{
    TestResult      verdict = TEST_PASS;
    StandBinder*    b       = &s_dedup_binder;
    StandFreshModel m;
    DedupSaid       said;
    uint64_t        a = 0, f = 0, laid = 0, book = 0;
    uint64_t*       mine   = kmalloc(sizeof(uint64_t) * STAND_DEDUP_OLD);
    uint64_t*       theirs = kmalloc(sizeof(uint64_t) * STAND_DEDUP_OLD);
    const char*     aside  = StandBinderAside(b);

    memset(&m, 0, sizeof(m));
    if (aside) {
        kfree(mine);
        kfree(theirs);
        return test_steps_aside(aside);
    }
    TEST_ASSERT(mine && theirs, "room for the files' blocks");
    TEST_ASSERT(StandFreshModelMake(&m, STAND_DEDUP_OLD * TAGFS_BLOCK_SIZE), "room for the files' bytes");
    TEST_ASSERT(StandDedupFile(&m, "dedup-old-a.bin", STAND_DEDUP_OLD, STAND_DEDUP_OTHER, &a),
                "file A is made and written");
    TEST_ASSERT_OK(StandBinderTallies(1u), "and a Tally takes it in, Dedup's pass held: A is the book's alone");
    TEST_ASSERT(DedupStandPassed(&said) && DedupHold(false), "Dedup's own pass is let go");
    TEST_ASSERT(StandDedupFile(&m, "dedup-old-b.bin", STAND_DEDUP_OLD, STAND_DEDUP_OTHER, &f),
                "file B is made and written with A's very bytes, a Tally later");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT(StandDedupBlocks(a, STAND_DEDUP_OLD, theirs) && StandDedupBlocks(f, STAND_DEDUP_OLD, mine),
                "each on its own ground");
    for (uint32_t t = 0; t < STAND_DEDUP_TALLIES && laid < STAND_DEDUP_OLD; t++) {
        TEST_ASSERT_OK(StandDedupTallies(1u, &said), "a Tally, and the pass after it");
        TEST_ASSERT_EQ(said.refused, 0, "no act the pass asked is refused");
        laid += said.laid;
        book += said.book;
    }
    TestEvidence("blocks of B laid on A", laid);
    TEST_ASSERT_EQ(laid, STAND_DEDUP_OLD, "the walk of the book's DIGESTS reaches A, and B is laid on it");
    TEST_ASSERT(book >= STAND_DEDUP_OLD, "matched to blocks the walk read off the book's DIGESTS");
    TEST_ASSERT(StandDedupOnto(f, theirs, STAND_DEDUP_OLD), "B's blocks are A's, each named by two");
    TEST_ASSERT(StandFreshHolds(&m, f) && StandFreshHolds(&m, a), "both read their bytes");
    TEST_ASSERT_OK(TagFSSettle(), "the pass's changes settled");
    TEST_ASSERT_OK(StandBookRoom(), "room is asked for");
    TEST_ASSERT(StandDedupFree(mine, STAND_DEDUP_OLD), "and the blocks B wrote are free again");
out:
    (void)DedupHold(true);
    if (f)
        (void)tagfs_delete_file(f, INK_UNWARRANTED);
    if (a)
        (void)tagfs_delete_file(a, INK_UNWARRANTED);
    kfree(mine);
    kfree(theirs);
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(b);
    return verdict;
}

// ----------------------------------------------------------------------------
// Declined: dedup:off, on the original or on the copy
// ----------------------------------------------------------------------------

static TestResult TestStandDedupDeclined(void)
{
    TestResult      verdict = TEST_PASS;
    StandBinder*    b       = &s_dedup_binder;
    StandFreshModel m, n;
    DedupSaid       said;
    uint64_t        a = 0, f = 0, c = 0, e = 0;
    uint64_t        ma[STAND_DEDUP_BLOCKS], mf[STAND_DEDUP_BLOCKS], mc[STAND_DEDUP_BLOCKS], me[STAND_DEDUP_BLOCKS];
    const char*     aside = StandBinderAside(b);

    memset(&m, 0, sizeof(m));
    memset(&n, 0, sizeof(n));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(DedupStandPassed(&said) && DedupHold(false), "Dedup's own pass is let go");
    TEST_ASSERT(StandFreshModelMake(&m, STAND_DEDUP_BLOCKS * TAGFS_BLOCK_SIZE) &&
                    StandFreshModelMake(&n, STAND_DEDUP_BLOCKS * TAGFS_BLOCK_SIZE), "room for the files' bytes");
    TEST_ASSERT(StandDedupFile(&m, "dedup-off-a.bin", STAND_DEDUP_BLOCKS, STAND_DEDUP_STREAM, &a) &&
                    tagfs_add_tag_string(a, &s_dedup_off, INK_UNWARRANTED) == OK,
                "file A is written and wears dedup:off");
    TEST_ASSERT(StandDedupFile(&m, "dedup-off-b.bin", STAND_DEDUP_BLOCKS, STAND_DEDUP_STREAM, &f),
                "file B is written with A's very bytes after it");
    TEST_ASSERT(StandDedupFile(&n, "dedup-off-c.bin", STAND_DEDUP_BLOCKS, STAND_DEDUP_OTHER, &c),
                "file C is written with bytes of its own");
    TEST_ASSERT(StandDedupFile(&n, "dedup-off-e.bin", STAND_DEDUP_BLOCKS, STAND_DEDUP_OTHER, &e) &&
                    tagfs_add_tag_string(e, &s_dedup_off, INK_UNWARRANTED) == OK,
                "and file E with C's very bytes after it, wearing dedup:off");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT(StandDedupBlocks(a, STAND_DEDUP_BLOCKS, ma) && StandDedupBlocks(f, STAND_DEDUP_BLOCKS, mf) &&
                    StandDedupBlocks(c, STAND_DEDUP_BLOCKS, mc) && StandDedupBlocks(e, STAND_DEDUP_BLOCKS, me),
                "each on its own ground");
    TEST_ASSERT_OK(StandDedupTallies(1u, &said), "a Tally, and the pass after it");
    TEST_ASSERT(said.laid == 0 && said.refused == 0, "nothing is laid, and no act is asked to be refused");
    TEST_ASSERT(StandDedupOwn(a, ma, STAND_DEDUP_BLOCKS) && StandDedupOwn(f, mf, STAND_DEDUP_BLOCKS),
                "nothing is laid on A, which wears dedup:off, and B keeps its own blocks");
    TEST_ASSERT(StandDedupOwn(c, mc, STAND_DEDUP_BLOCKS) && StandDedupOwn(e, me, STAND_DEDUP_BLOCKS),
                "E, which wears dedup:off, is not laid on C");
    TEST_ASSERT(StandFreshHolds(&m, a) && StandFreshHolds(&m, f) && StandFreshHolds(&n, c) && StandFreshHolds(&n, e),
                "all four read their bytes");
out:
    (void)DedupHold(true);
    if (e)
        (void)tagfs_delete_file(e, INK_UNWARRANTED);
    if (c)
        (void)tagfs_delete_file(c, INK_UNWARRANTED);
    if (f)
        (void)tagfs_delete_file(f, INK_UNWARRANTED);
    if (a)
        (void)tagfs_delete_file(a, INK_UNWARRANTED);
    StandFreshModelLetGo(&n);
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(b);
    return verdict;
}

// ----------------------------------------------------------------------------
// Kept: files the system keeps are neither laid nor laid on
// ----------------------------------------------------------------------------

static TestResult TestStandDedupKept(void)
{
    TestResult        verdict = TEST_PASS;
    StandBinder*      b       = &s_dedup_binder;
    StandFreshModel   m;
    DedupSaid         said;
    uint64_t          s1 = 0, s2 = 0, f = 0;
    uint64_t          m1[STAND_DEDUP_BLOCKS], m2[STAND_DEDUP_BLOCKS], mf[STAND_DEDUP_BLOCKS];
    const TagSpelling system = { .key = "system", .key_len = 6 };
    const char*       aside  = StandBinderAside(b);

    memset(&m, 0, sizeof(m));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(DedupStandPassed(&said) && DedupHold(false), "Dedup's own pass is let go");
    TEST_ASSERT(StandFreshModelMake(&m, STAND_DEDUP_BLOCKS * TAGFS_BLOCK_SIZE), "room for the files' bytes");
    TEST_ASSERT(StandDedupFile(&m, "dedup-kept-1.bin", STAND_DEDUP_BLOCKS, STAND_DEDUP_STREAM, &s1) &&
                    tagfs_add_tag_string(s1, &system, INK_WARRANTED) == OK,
                "a file is written and wears system");
    TEST_ASSERT(StandDedupFile(&m, "dedup-kept-2.bin", STAND_DEDUP_BLOCKS, STAND_DEDUP_STREAM, &s2) &&
                    tagfs_add_tag_string(s2, &system, INK_WARRANTED) == OK,
                "a second of its very bytes after it, wearing system too");
    TEST_ASSERT(StandDedupFile(&m, "dedup-kept-f.bin", STAND_DEDUP_BLOCKS, STAND_DEDUP_STREAM, &f),
                "and a third of those bytes, wearing nothing");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT(StandDedupBlocks(s1, STAND_DEDUP_BLOCKS, m1) && StandDedupBlocks(s2, STAND_DEDUP_BLOCKS, m2) &&
                    StandDedupBlocks(f, STAND_DEDUP_BLOCKS, mf), "each on its own ground");
    TEST_ASSERT_OK(StandDedupTallies(1u, &said), "a Tally, and the pass after it");
    TEST_ASSERT(said.laid == 0 && said.refused == 0, "nothing is laid, and no act is asked to be refused");
    TEST_ASSERT(StandDedupOwn(s1, m1, STAND_DEDUP_BLOCKS) && StandDedupOwn(s2, m2, STAND_DEDUP_BLOCKS),
                "the two files the system keeps keep their own blocks");
    TEST_ASSERT(StandDedupOwn(f, mf, STAND_DEDUP_BLOCKS), "and the third is laid on neither");
    TEST_ASSERT(StandFreshHolds(&m, s1) && StandFreshHolds(&m, s2) && StandFreshHolds(&m, f), "all read their bytes");
out:
    (void)DedupHold(true);
    if (f)
        (void)tagfs_delete_file(f, INK_UNWARRANTED);
    if (s2)
        (void)tagfs_delete_file(s2, INK_WARRANTED);
    if (s1)
        (void)tagfs_delete_file(s1, INK_WARRANTED);
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(b);
    return verdict;
}

// ----------------------------------------------------------------------------
// Defrag keeps what is shared: it gathers the file's own runs alone
// ----------------------------------------------------------------------------

#define STAND_DEDUP_DEFRAG  4u

static TestResult TestStandDedupDefrag(void)
{
    TestResult      verdict = TEST_PASS;
    StandFreshModel m;
    ConjugateSaid   csaid;
    ConjugateAsk    ask;
    uint64_t        a = 0, f = 0, gen = 0, onto[STAND_DEDUP_DEFRAG], mine[STAND_DEDUP_DEFRAG];

    memset(&m, 0, sizeof(m));
    TEST_ASSERT(StandFreshModelMake(&m, STAND_DEDUP_DEFRAG * TAGFS_BLOCK_SIZE), "room for the files' bytes");
    TEST_ASSERT(StandDedupFile(&m, "dedup-defrag-a.bin", STAND_DEDUP_DEFRAG, STAND_DEDUP_STREAM, &a) &&
                    StandDedupFile(&m, "dedup-defrag-b.bin", STAND_DEDUP_DEFRAG, STAND_DEDUP_STREAM, &f),
                "two files of one content are made");
    TEST_ASSERT_EQ(StandFreshWrite(&m, f, TAGFS_BLOCK_SIZE, TAGFS_BLOCK_SIZE, STAND_DEDUP_STREAM),
                   (int)TAGFS_BLOCK_SIZE, "B's block 1 is written again, on fresh ground");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT(StandDedupBlocks(a, STAND_DEDUP_DEFRAG, onto) && StandDedupBlocks(f, STAND_DEDUP_DEFRAG, mine) &&
                    mine[1] != mine[0] + 1u, "B's blocks 0 and 1 lie apart");
    ask = (ConjugateAsk){ .fid = f, .at = 2, .onto = a, .onto_at = 2, .count = 1 };
    TEST_ASSERT_OK(TagFSConjugate(INK_WARRANTED, &ask, &csaid), "B's block 2 is laid on A's");
    TEST_ASSERT(StandBlockOf(f, 2, &mine[2]) && mine[2] == onto[2] && CommonholdOwners(onto[2]) == 2u,
                "B's block 2 is A's, named by two");
    TEST_ASSERT_OK(TagFSSettle(), "settled");

    TEST_ASSERT_OK(tagfs_defrag_file(f), "B is defragmented");
    TEST_ASSERT(StandDedupBlocks(f, STAND_DEDUP_DEFRAG, mine), "its blocks are read off its record");
    TEST_ASSERT(mine[1] == mine[0] + 1u, "its own blocks 0 and 1 are gathered into one run");
    TEST_ASSERT(mine[2] == onto[2] && CommonholdOwners(onto[2]) == 2u,
                "and its block 2 stays on A's, named by two: a block in common is not taken apart");
    TEST_ASSERT(StandFreshHolds(&m, f) && StandFreshHolds(&m, a), "both read their bytes");
    TEST_ASSERT_OK(TagFSSettle(), "settled");

    gen = InkGenOf(f);
    TEST_ASSERT_OK(tagfs_defrag_file(f), "B is defragmented again");
    TEST_ASSERT_EQ(InkGenOf(f), gen, "and nothing is inked: a run on another's blocks is no cause to move");
    TEST_ASSERT(StandBlockOf(f, 2, &mine[2]) && mine[2] == onto[2] && CommonholdOwners(onto[2]) == 2u,
                "B's block 2 is A's still");
out:
    if (f)
        (void)tagfs_delete_file(f, INK_UNWARRANTED);
    if (a)
        (void)tagfs_delete_file(a, INK_UNWARRANTED);
    StandFreshModelLetGo(&m);
    return verdict;
}

static const TestCase g_stand_dedup_tests[] = {
    { .name = "stand_dedup_copy", .func = TestStandDedupCopy, .reach = TEST_WRITES_TO_VOLUME,
      .format = DEED_FORMAT_V2 },
    { .name = "stand_dedup_old", .func = TestStandDedupOld, .reach = TEST_WRITES_TO_VOLUME,
      .format = DEED_FORMAT_V2 },
    { .name = "stand_dedup_declined", .func = TestStandDedupDeclined, .reach = TEST_WRITES_TO_VOLUME,
      .format = DEED_FORMAT_V2 },
    { .name = "stand_dedup_kept", .func = TestStandDedupKept, .reach = TEST_WRITES_TO_VOLUME,
      .format = DEED_FORMAT_V2 },
    { .name = "stand_dedup_defrag", .func = TestStandDedupDefrag, .reach = TEST_WRITES_TO_VOLUME,
      .format = DEED_FORMAT_V2 },
};

const TestSuite g_stand_dedup_suite = {
    g_stand_dedup_tests, sizeof(g_stand_dedup_tests) / sizeof(g_stand_dedup_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
