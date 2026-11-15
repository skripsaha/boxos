#include "stand_binder.h"

#ifdef CONFIG_VOLUME_TESTS

#include "stand_fresh.h"
#include "../dedup/conjugate.h"
#include "../dedup/dedup.h"
#include "../file_map/file_map.h"
#include "../ink/ink.h"
#include "../ink/commonhold.h"
#include "../mount/book_mount.h"
#include "../mount/under_glass.h"

// ============================================================================
// The stand_dedup suite: duplicates found and laid by the pass after each
// Tally, and a defrag that keeps what is shared
//
// The pass Glean runs after a Tally weighs the blocks written since the Tally
// before: a block whose digest another's matches — one written beside it, or
// one the book's DIGESTS names — is a candidate, and the bytes decide
// (Conjugate). The copy is laid on the original, the older of the two, and
// its own blocks go back: the volume holds the bytes once, both files read
// them, the census of the mount after it finds every block named as memory
// holds it. A file the system keeps, and the kernel the loader reads by its
// blocks, are neither laid nor laid on — passed over before any act is asked,
// so the pass refuses nothing it could have known.
// Only a series is laid — 16 blocks of a copy in a row on as many of one
// original in a row — and never a file on itself. A run SHARED splits a file
// into stretches of its own: defrag gathers each, and the fragmentation score
// counts gaps inside them alone.
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

/* Whether the file's blocks [from, from + n) are `onto`'s, each named by two. */
static bool StandDedupOnto(uint64_t fid, uint32_t from, const uint64_t* onto, uint32_t n)
{
    uint64_t block = 0;
    for (uint32_t k = from; k < from + n; k++)
        if (!StandBlockOf(fid, k, &block) || block != onto[k] || CommonholdOwners(block) != 2u)
            return false;
    return true;
}

/* Whether the file's blocks [from, from + n) are still `mine`'s, its own alone. */
static bool StandDedupOwn(uint64_t fid, uint32_t from, const uint64_t* mine, uint32_t n)
{
    uint64_t block = 0;
    for (uint32_t k = from; k < from + n; k++)
        if (!StandBlockOf(fid, k, &block) || block != mine[k] || CommonholdOwners(block) != 0)
            return false;
    return true;
}

/* The file's blocks [to, to + n) written with the model's bytes of [from,
 * from + n): the file holds a stretch of itself twice. */
static bool StandDedupEcho(StandFreshModel* m, uint64_t fid, uint32_t from, uint32_t to, uint32_t n)
{
    uint64_t at = (uint64_t)to * TAGFS_BLOCK_SIZE, bytes = (uint64_t)n * TAGFS_BLOCK_SIZE;
    if (at + bytes > m->room)
        return false;
    memcpy(m->bytes + at, m->bytes + (uint64_t)from * TAGFS_BLOCK_SIZE, bytes);
    if (at + bytes > m->size)
        m->size = at + bytes;
    return StandWriteAt(fid, at, m->bytes + at, (uint32_t)bytes) == (int)bytes;
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
        said->scant   += one.scant;
    }
    return rc;
}

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
    TEST_ASSERT(StandDedupOnto(f, 0, theirs, STAND_DEDUP_BLOCKS), "B's blocks are A's, each named by two: A is older");
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
    TEST_ASSERT(StandDedupOnto(f, 0, theirs, STAND_DEDUP_BLOCKS), "B's blocks are A's still, two owners on each");
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
    TEST_ASSERT(StandDedupOnto(f, 0, theirs, STAND_DEDUP_OLD), "B's blocks are A's, each named by two");
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
                    StandKeepInRole(s1, TAGFS_FILE_ROLE_SYSTEM) == OK,
                "a file is written and kept in the system's role");
    TEST_ASSERT(StandDedupFile(&m, "dedup-kept-2.bin", STAND_DEDUP_BLOCKS, STAND_DEDUP_STREAM, &s2) &&
                    StandKeepInRole(s2, TAGFS_FILE_ROLE_BOOT) == OK,
                "a second of its very bytes after it, kept in the boot role");
    TEST_ASSERT(StandDedupFile(&m, "dedup-kept-f.bin", STAND_DEDUP_BLOCKS, STAND_DEDUP_STREAM, &f) &&
                    tagfs_add_tag_string(f, &system) == OK,
                "and a third of those bytes, wearing only the word system, which keeps nothing");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT(StandDedupBlocks(s1, STAND_DEDUP_BLOCKS, m1) && StandDedupBlocks(s2, STAND_DEDUP_BLOCKS, m2) &&
                    StandDedupBlocks(f, STAND_DEDUP_BLOCKS, mf), "each on its own ground");
    TEST_ASSERT_OK(StandDedupTallies(1u, &said), "a Tally, and the pass after it");
    TEST_ASSERT(said.laid == 0 && said.refused == 0, "nothing is laid, and no act is asked to be refused");
    TEST_ASSERT(StandDedupOwn(s1, 0, m1, STAND_DEDUP_BLOCKS) && StandDedupOwn(s2, 0, m2, STAND_DEDUP_BLOCKS),
                "the two files the system keeps keep their own blocks");
    TEST_ASSERT(StandDedupOwn(f, 0, mf, STAND_DEDUP_BLOCKS), "and the third is laid on neither: its word is no bar");
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
// A series: only 16 blocks laid on as many in a row, never a file on itself
// ----------------------------------------------------------------------------

#define STAND_DEDUP_SERIES  40u      /* B: blocks 0 to 23 A's bytes, block 24 its own, blocks 25 to 39 A's again */
#define STAND_DEDUP_BREAK   24u
#define STAND_DEDUP_ECHO    16u      /* C: blocks 16 to 31 its blocks 0 to 15 again */
#define STAND_DEDUP_SELF    0xded2u

static TestResult TestStandDedupSeries(void)
{
    TestResult      verdict = TEST_PASS;
    StandBinder*    b       = &s_dedup_binder;
    StandFreshModel ma, mb, mc;
    DedupSaid       said;
    uint64_t        a = 0, f = 0, c = 0;
    uint64_t*       theirs = kmalloc(sizeof(uint64_t) * (2u * STAND_DEDUP_SERIES + 2u * STAND_DEDUP_ECHO));
    uint64_t*       mine   = theirs ? theirs + STAND_DEDUP_SERIES : NULL;
    uint64_t*       self   = theirs ? mine + STAND_DEDUP_SERIES : NULL;
    const char*     aside  = StandBinderAside(b);

    memset(&ma, 0, sizeof(ma));
    memset(&mb, 0, sizeof(mb));
    memset(&mc, 0, sizeof(mc));
    if (aside) {
        kfree(theirs);
        return test_steps_aside(aside);
    }
    TEST_ASSERT(theirs != NULL, "room for the files' blocks");
    TEST_ASSERT(DedupStandPassed(&said) && DedupHold(false), "Dedup's own pass is let go");
    TEST_ASSERT(StandFreshModelMake(&ma, STAND_DEDUP_SERIES * TAGFS_BLOCK_SIZE) &&
                    StandFreshModelMake(&mb, STAND_DEDUP_SERIES * TAGFS_BLOCK_SIZE) &&
                    StandFreshModelMake(&mc, 2u * STAND_DEDUP_ECHO * TAGFS_BLOCK_SIZE), "room for the files' bytes");
    TEST_ASSERT(StandDedupFile(&ma, "dedup-series-a.bin", STAND_DEDUP_SERIES, STAND_DEDUP_STREAM, &a),
                "file A is made and written");
    TEST_ASSERT(StandDedupFile(&mb, "dedup-series-b.bin", STAND_DEDUP_SERIES, STAND_DEDUP_STREAM, &f) &&
                    StandFreshWrite(&mb, f, (uint64_t)STAND_DEDUP_BREAK * TAGFS_BLOCK_SIZE, TAGFS_BLOCK_SIZE,
                                    STAND_DEDUP_OTHER) == (int)TAGFS_BLOCK_SIZE,
                "file B of A's very bytes but its block 24: a series of 24 blocks A holds, and one of 15");
    TEST_ASSERT(StandDedupFile(&mc, "dedup-series-c.bin", STAND_DEDUP_ECHO, STAND_DEDUP_SELF, &c) &&
                    StandDedupEcho(&mc, c, 0, STAND_DEDUP_ECHO, STAND_DEDUP_ECHO),
                "and file C, whose blocks 16 to 31 are its blocks 0 to 15 again");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT(StandDedupBlocks(a, STAND_DEDUP_SERIES, theirs) && StandDedupBlocks(f, STAND_DEDUP_SERIES, mine) &&
                    StandDedupBlocks(c, 2u * STAND_DEDUP_ECHO, self), "each on its own ground");
    TEST_ASSERT_OK(StandDedupTallies(1u, &said), "a Tally, and the pass Glean runs after it");
    TestEvidence("blocks laid", said.laid);
    TEST_ASSERT_EQ(said.laid, STAND_DEDUP_BREAK, "the pass lays B's series of 24 on A's");
    TEST_ASSERT_EQ(said.scant, STAND_DEDUP_SERIES - STAND_DEDUP_BREAK - 1u,
                   "and leaves B's series of 15 as it is: shorter than 16 blocks in a row");
    TEST_ASSERT_EQ(said.refused, 0, "no act it asked is refused");
    TEST_ASSERT(said.passed >= STAND_DEDUP_ECHO, "C's second stretch is passed over: a file is never laid on itself");
    TEST_ASSERT(StandDedupOnto(f, 0, theirs, STAND_DEDUP_BREAK), "B's blocks 0 to 23 are A's, each named by two");
    TEST_ASSERT(StandDedupOwn(f, STAND_DEDUP_BREAK, mine, STAND_DEDUP_SERIES - STAND_DEDUP_BREAK),
                "B's blocks 24 to 39 are its own still");
    TEST_ASSERT(StandDedupOwn(c, 0, self, 2u * STAND_DEDUP_ECHO), "and C keeps every block of its own");
    TEST_ASSERT(StandFreshHolds(&ma, a) && StandFreshHolds(&mb, f) && StandFreshHolds(&mc, c),
                "all three read their bytes");
out:
    (void)DedupHold(true);
    if (c)
        (void)tagfs_delete_file(c, INK_UNWARRANTED);
    if (f)
        (void)tagfs_delete_file(f, INK_UNWARRANTED);
    if (a)
        (void)tagfs_delete_file(a, INK_UNWARRANTED);
    kfree(theirs);
    StandFreshModelLetGo(&mc);
    StandFreshModelLetGo(&mb);
    StandFreshModelLetGo(&ma);
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

// ----------------------------------------------------------------------------
// The fragmentation score: a step onto shared ground or off it is no gap
// ----------------------------------------------------------------------------

#define STAND_DEDUP_FRAG  48u        /* B's blocks 16 to 31 laid on A's: a stretch of its own each side */

static TestResult TestStandDedupFrag(void)
{
    TestResult      verdict = TEST_PASS;
    StandFreshModel m;
    ConjugateSaid   csaid;
    ConjugateAsk    ask;
    FileRecord*     rec  = NULL;
    uint64_t        a = 0, f = 0, steps = 0, own = 0, onto[STAND_DEDUP_FRAG], mine[STAND_DEDUP_FRAG];
    uint32_t        gaps = 0;

    memset(&m, 0, sizeof(m));
    TEST_ASSERT(StandFreshModelMake(&m, STAND_DEDUP_FRAG * TAGFS_BLOCK_SIZE), "room for the files' bytes");
    TEST_ASSERT(StandDedupFile(&m, "dedup-frag-a.bin", STAND_DEDUP_FRAG, STAND_DEDUP_STREAM, &a) &&
                    StandDedupFile(&m, "dedup-frag-b.bin", STAND_DEDUP_FRAG, STAND_DEDUP_STREAM, &f),
                "two files of one content are made");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    ask = (ConjugateAsk){ .fid = f, .at = 16, .onto = a, .onto_at = 16, .count = 16 };
    TEST_ASSERT_OK(TagFSConjugate(INK_WARRANTED, &ask, &csaid), "B's blocks 16 to 31 are laid on A's");
    TEST_ASSERT(StandDedupBlocks(a, STAND_DEDUP_FRAG, onto) && StandDedupBlocks(f, STAND_DEDUP_FRAG, mine) &&
                    StandDedupOnto(f, 16, onto, 16), "B's blocks 16 to 31 are A's, named by two");
    for (uint32_t k = 1; k < STAND_DEDUP_FRAG; k++)
        if ((k < 16u || k > 32u) && mine[k] != mine[k - 1u] + 1u)
            own++;
    rec = FileMapRecordHold(tagfs_get_state()->files, f, NULL);
    TEST_ASSERT(rec != NULL, "B's record is held");
    TagFSFragOfRecord(rec, &gaps, &steps);
    TestEvidence("gaps between B's own blocks", own);
    TEST_ASSERT_EQ(gaps, own, "a gap is counted between B's own blocks alone: the step onto A's and off them is none");
    TEST_ASSERT_EQ(steps, 30u, "over the steps of its two stretches of its own, 15 each");
out:
    if (rec)
        FileRecordRelease(rec);
    if (f)
        (void)tagfs_delete_file(f, INK_UNWARRANTED);
    if (a)
        (void)tagfs_delete_file(a, INK_UNWARRANTED);
    StandFreshModelLetGo(&m);
    return verdict;
}

static const TestCase g_stand_dedup_tests[] = {
    { .name = "stand_dedup_copy", .func = TestStandDedupCopy, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_dedup_old", .func = TestStandDedupOld, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_dedup_kept", .func = TestStandDedupKept, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_dedup_series", .func = TestStandDedupSeries, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_dedup_defrag", .func = TestStandDedupDefrag, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_dedup_frag", .func = TestStandDedupFrag, .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_dedup_suite = {
    g_stand_dedup_tests, sizeof(g_stand_dedup_tests) / sizeof(g_stand_dedup_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
