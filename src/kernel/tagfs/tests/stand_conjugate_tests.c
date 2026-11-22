#include "stand_binder.h"
#include "stand_fresh.h"
#include "../dedup/conjugate.h"
#include "../ink/fallow.h"
#include "../ink/commonhold.h"
#include "../integrity/gloss.h"
#include "../mount/book_mount.h"
#include "../mount/tally_read.h"
#include "../mount/under_glass.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_conjugate suite: a duplicate laid on the blocks it duplicates
//
// Two files of one content: the act lays the duplicate's blocks on the
// target's — a Shaped of a run SHARED — and the duplicate's own blocks rest in
// Fallow, then go back once the ribbon has left the Quire of the stitch that
// let them go: the volume holds the bytes once. Both files read their
// bytes, a Tally's self-check and the census of the mount after it find every
// block named as memory holds it. The act is decided at the Ink: a file
// changed between the comparison and the Ink — the duplicate or the target —
// refuses it, and so do blocks that differ, by their digests or, where the
// digests agree, by their bytes; a file the system keeps and the kernel the
// loader reads by its blocks are never laid, and a
// cabin wearing neither system nor god never asks. Every refusal leaves
// everything as it was: the duplicate on its own blocks, nothing resting, no
// block in common.
// ============================================================================

#define STAND_CONJUGATE_BLOCKS  3u
#define STAND_CONJUGATE_STREAM  0x5c00u
#define STAND_CONJUGATE_FLIP    100u     /* the byte of block 1 the duplicate holds otherwise */

static StandBinder s_conjugate_binder;

/* A file of `blocks` blocks of the suite's stream — the model's bytes — its
 * id in *fid. */
static bool StandConjugateFile(StandFreshModel* m, const char* name, uint32_t blocks, uint64_t* fid)
{
    m->size = 0;
    return tagfs_create_file(name, NULL, 0, 0, fid) == 0 &&
           StandFreshWrite(m, *fid, 0, blocks * TAGFS_BLOCK_SIZE, STAND_CONJUGATE_STREAM) ==
               (int)(blocks * TAGFS_BLOCK_SIZE);
}

/* The blocks the file's first `n` lie on, into `out`. */
static bool StandConjugateBlocks(uint64_t fid, uint32_t n, uint64_t* out)
{
    for (uint32_t k = 0; k < n; k++)
        if (!StandBlockOf(fid, k, &out[k]))
            return false;
    return true;
}

/* Whether the file's first `n` blocks are `onto`'s, each named by two. */
static bool StandConjugateOnto(uint64_t fid, const uint64_t* onto, uint32_t n)
{
    uint64_t block = 0;
    for (uint32_t k = 0; k < n; k++)
        if (!StandBlockOf(fid, k, &block) || block != onto[k] || CommonholdOwners(block) != 2u)
            return false;
    return true;
}

/* Whether a refusal left everything as it was: the duplicate on `mine`, the
 * target's `theirs` owned by no one more, and `resting` blocks in Fallow. */
static bool StandConjugateUntouched(uint64_t fid, const uint64_t* mine, const uint64_t* theirs, uint32_t n,
                                    uint64_t resting)
{
    uint64_t block = 0;
    for (uint32_t k = 0; k < n; k++)
        if (!StandBlockOf(fid, k, &block) || block != mine[k] || CommonholdOwners(theirs[k]) != 0)
            return false;
    return FallowBlocks() == resting;
}

/* Whether any of the `n` blocks is in use. */
static bool StandConjugateInUse(const uint64_t* blocks, uint32_t n)
{
    for (uint32_t k = 0; k < n; k++)
        if (StandBlockInUse(blocks[k]))
            return true;
    return false;
}

/* The digest the book holds for `block`: the Gloss's, else the Tally's. */
static bool StandConjugateDigest(uint64_t block, uint64_t* digest)
{
    error_t refused = OK;
    return GlossFind(block, digest) || TallyDigestFind(block, digest, &refused) == TALLY_DIGEST_NAMED;
}

static const TagSpelling s_note = { .key = "note", .key_len = 4 };

/* What the meddler does between the act's comparison and its Ink: puts a
 * user's tag on `fid` (`write` false) or writes its block 0 anew with the
 * model's bytes, its very own — the answer in `rc`. */
typedef struct {
    uint64_t               fid;
    const StandFreshModel* m;
    bool                   write;
    error_t                rc;
} StandConjugateMeddle;

static void StandConjugateMeddles(void* ctx)
{
    StandConjugateMeddle* w = ctx;
    if (!w->write)
        w->rc = tagfs_add_tag_string(TagOwnerPerson(), w->fid, &s_note);
    else
        w->rc = StandWriteAt(w->fid, 0, w->m->bytes, TAGFS_BLOCK_SIZE) == (int)TAGFS_BLOCK_SIZE ? OK : ERR_WRITE_FAILED;
}

// ----------------------------------------------------------------------------
// Laid: the volume holds the bytes once
// ----------------------------------------------------------------------------

static TestResult TestStandConjugateLaid(void)
{
    TestResult      verdict = TEST_PASS;
    StandBinder*    b       = &s_conjugate_binder;
    StandFreshModel m;
    ConjugateSaid   said;
    ConjugateAsk    ask;
    MountBookSaid   mounted;
    uint64_t        a = 0, f = 0, resting = 0, mine[STAND_CONJUGATE_BLOCKS], theirs[STAND_CONJUGATE_BLOCKS];
    const char*     aside = StandBinderAside(b);

    memset(&m, 0, sizeof(m));
    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, STAND_CONJUGATE_BLOCKS * TAGFS_BLOCK_SIZE), "room for the files' bytes");
    TEST_ASSERT(StandConjugateFile(&m, "conjugate-a.bin", STAND_CONJUGATE_BLOCKS, &a), "file A is made and written");
    TEST_ASSERT(StandConjugateFile(&m, "conjugate-b.bin", STAND_CONJUGATE_BLOCKS, &f),
                "file B is made and written with A's very bytes");
    TEST_ASSERT_OK(TagFSSettle(), "both settled");
    TEST_ASSERT(StandConjugateBlocks(a, STAND_CONJUGATE_BLOCKS, theirs) &&
                    StandConjugateBlocks(f, STAND_CONJUGATE_BLOCKS, mine) && mine[0] != theirs[0],
                "each on its own ground");
    resting = FallowBlocks();
    ask     = (ConjugateAsk){ .fid = f, .at = 0, .onto = a, .onto_at = 0, .count = STAND_CONJUGATE_BLOCKS };
    TEST_ASSERT_OK(TagFSConjugate(INK_WARRANTED, &ask, &said), "B's blocks are laid on A's");
    TEST_ASSERT_EQ(said.why, CONJUGATE_LAID, "the act says it laid them");
    TEST_ASSERT_EQ(said.blocks, STAND_CONJUGATE_BLOCKS, "all three");
    TEST_ASSERT_EQ(FallowBlocks(), resting + STAND_CONJUGATE_BLOCKS, "B's own three blocks rest in Fallow");
    TEST_ASSERT(StandConjugateOnto(f, theirs, STAND_CONJUGATE_BLOCKS), "B's blocks are A's, each named by two");
    TEST_ASSERT(StandFreshHolds(&m, f) && StandFreshHolds(&m, a), "both read their bytes, every one");
    TEST_ASSERT_OK(StandBinderTallies(1u), "a Tally is laid past it, held by its self-check to the files' ground");
    TEST_ASSERT_OK(StandBookRoom(), "room is asked for: the ribbon leaves its Quire, the lag's stitches past it");
    TEST_ASSERT(!StandConjugateInUse(mine, STAND_CONJUGATE_BLOCKS),
                "and the three blocks B let go are free: the volume holds the bytes once");
    TEST_ASSERT(StandFreshSettledAndBack(&m, f) && StandFreshHolds(&m, a),
                "mounted again on it, B has the runs memory held and both read their bytes");
    MountBookStandSaid(&mounted);
    TEST_ASSERT(mounted.doubles == 0 && !TagFSIsUnderGlass(),
                "the census finds no block two owners name, and the volume is written on");
    TEST_ASSERT(StandConjugateOnto(f, theirs, STAND_CONJUGATE_BLOCKS),
                "B's blocks are A's still, the mount counting two owners on each");
out:
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(b);
    return verdict;
}

// ----------------------------------------------------------------------------
// Changed between the comparison and the Ink
// ----------------------------------------------------------------------------

static TestResult TestStandConjugateChanged(void)
{
    TestResult           verdict = TEST_PASS;
    StandFreshModel      m;
    ConjugateSaid        said;
    StandConjugateMeddle w;
    ConjugateAsk         ask;
    uint64_t             a = 0, f = 0, resting = 0, mine[2], theirs[2];
    error_t              rc = OK;

    memset(&m, 0, sizeof(m));
    TEST_ASSERT(StandFreshModelMake(&m, 2u * TAGFS_BLOCK_SIZE), "room for the files' bytes");
    TEST_ASSERT(StandConjugateFile(&m, "conjugate-a.bin", 2u, &a) && StandConjugateFile(&m, "conjugate-b.bin", 2u, &f),
                "two files of one content are made");
    TEST_ASSERT_OK(TagFSSettle(), "and settled");
    TEST_ASSERT(StandConjugateBlocks(a, 2u, theirs) && StandConjugateBlocks(f, 2u, mine), "each on its own ground");
    resting = FallowBlocks();
    ask     = (ConjugateAsk){ .fid = f, .at = 0, .onto = a, .onto_at = 0, .count = 2u };

    w = (StandConjugateMeddle){ .fid = f, .write = false, .rc = ERR_UNKNOWN };
    ConjugateStandBetween(StandConjugateMeddles, &w);
    rc = TagFSConjugate(INK_WARRANTED, &ask, &said);
    TEST_ASSERT_OK(w.rc, "a tag of the user's goes on B between the act's comparison and its Ink");
    TEST_ASSERT_EQ(rc, ERR_BUSY, "the act is refused: B is not the file it compared");
    TEST_ASSERT_EQ(said.why, CONJUGATE_CHANGED, "and says so");
    TEST_ASSERT(StandConjugateUntouched(f, mine, theirs, 2u, resting),
                "nothing is touched: B on its own blocks, nothing rests, no block in common");
    TEST_ASSERT(tagfs_has_tag(f, tagfs_tag_lookup("note")), "B wears the tag the change put on");
    TEST_ASSERT(StandFreshHolds(&m, f) && StandFreshHolds(&m, a), "both read their bytes");
    TEST_ASSERT_OK(tagfs_remove_tag_string(TagOwnerPerson(), f, &s_note), "the tag comes off B");

    w = (StandConjugateMeddle){ .fid = a, .m = &m, .write = true, .rc = ERR_UNKNOWN };
    ConjugateStandBetween(StandConjugateMeddles, &w);
    rc = TagFSConjugate(INK_WARRANTED, &ask, &said);
    TEST_ASSERT_OK(w.rc, "A's block 0 is written anew with its very bytes between the comparison and the Ink");
    TEST_ASSERT_EQ(rc, ERR_BUSY, "the act is refused: A is not the file it compared");
    TEST_ASSERT_EQ(said.why, CONJUGATE_CHANGED, "and says so");
    TEST_ASSERT(StandConjugateUntouched(f, mine, theirs, 2u, resting + 1u),
                "nothing is touched: B on its own blocks, A's old block 0 the one block resting — its own write's");
    TEST_ASSERT(StandConjugateBlocks(a, 2u, theirs) && CommonholdOwners(theirs[0]) == 0,
                "and A's new block 0 is A's alone");
    TEST_ASSERT(StandFreshHolds(&m, f) && StandFreshHolds(&m, a), "both read their bytes");
out:
    ConjugateStandBetween(NULL, NULL);
    if (f)
        (void)tagfs_delete_file(f, INK_UNWARRANTED);
    if (a)
        (void)tagfs_delete_file(a, INK_UNWARRANTED);
    StandFreshModelLetGo(&m);
    return verdict;
}

// ----------------------------------------------------------------------------
// Blocks that differ: by their digests, and by their bytes where the digests
// agree
// ----------------------------------------------------------------------------

static TestResult TestStandConjugateDiffers(void)
{
    TestResult      verdict = TEST_PASS;
    StandFreshModel m, n;
    ConjugateSaid   said;
    ConjugateAsk    ask;
    uint64_t        a = 0, f = 0, resting = 0, mine[2], theirs[2], own = 0, lent = 0;
    bool            forged = false;
    uint8_t         flip   = 0;

    memset(&m, 0, sizeof(m));
    memset(&n, 0, sizeof(n));
    TEST_ASSERT(StandFreshModelMake(&m, 2u * TAGFS_BLOCK_SIZE) && StandFreshModelMake(&n, 2u * TAGFS_BLOCK_SIZE),
                "room for the files' bytes");
    TEST_ASSERT(StandConjugateFile(&m, "conjugate-a.bin", 2u, &a) && StandConjugateFile(&n, "conjugate-b.bin", 2u, &f),
                "two files of one content are made");
    flip = (uint8_t)(n.bytes[TAGFS_BLOCK_SIZE + STAND_CONJUGATE_FLIP] ^ 0x5au);
    n.bytes[TAGFS_BLOCK_SIZE + STAND_CONJUGATE_FLIP] = flip;
    TEST_ASSERT_EQ(StandWriteAt(f, TAGFS_BLOCK_SIZE + STAND_CONJUGATE_FLIP, &flip, 1), 1,
                   "and one byte of B's block 1 is written otherwise");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT(StandConjugateBlocks(a, 2u, theirs) && StandConjugateBlocks(f, 2u, mine), "each on its own ground");
    resting = FallowBlocks();
    ask     = (ConjugateAsk){ .fid = f, .at = 0, .onto = a, .onto_at = 0, .count = 2u };

    TEST_ASSERT_EQ(TagFSConjugate(INK_WARRANTED, &ask, &said), ERR_INVALID_ARGUMENT, "the act is refused");
    TEST_ASSERT_EQ(said.why, CONJUGATE_DIFFERS, "the digests of B's block 1 and A's differ");
    TEST_ASSERT_EQ(said.at, 1, "at block 1");
    TEST_ASSERT(StandConjugateUntouched(f, mine, theirs, 2u, resting), "nothing is touched");

    TEST_ASSERT(StandConjugateDigest(mine[1], &own) && StandConjugateDigest(theirs[1], &lent) && own != lent,
                "the book holds both digests, and they differ");
    TEST_ASSERT_OK(GlossNote(mine[1], lent), "B's block 1 is said to have A's digest — 64 bits prove nothing");
    forged = true;
    TEST_ASSERT_EQ(TagFSConjugate(INK_WARRANTED, &ask, &said), ERR_INVALID_ARGUMENT, "the act is refused");
    TEST_ASSERT_EQ(said.why, CONJUGATE_COLLIDES, "the digests agree, the bytes do not");
    TEST_ASSERT_EQ(said.at, 1, "at block 1");
    TEST_ASSERT(StandConjugateUntouched(f, mine, theirs, 2u, resting), "nothing is touched");
    TEST_ASSERT_OK(GlossNote(mine[1], own), "B's block 1 has its own digest again");
    forged = false;
    TEST_ASSERT(StandFreshHolds(&n, f) && StandFreshHolds(&m, a), "both read their own bytes");
out:
    if (forged)
        (void)GlossNote(mine[1], own);
    if (f)
        (void)tagfs_delete_file(f, INK_UNWARRANTED);
    if (a)
        (void)tagfs_delete_file(a, INK_UNWARRANTED);
    StandFreshModelLetGo(&n);
    StandFreshModelLetGo(&m);
    return verdict;
}

// ----------------------------------------------------------------------------
// Unwarranted: a cabin wearing neither system nor god
// ----------------------------------------------------------------------------

static TestResult TestStandConjugateUnwarranted(void)
{
    TestResult      verdict = TEST_PASS;
    StandFreshModel m;
    ConjugateSaid   said;
    ConjugateAsk    ask;
    uint64_t        a = 0, f = 0, resting = 0, mine[1], theirs[1];

    memset(&m, 0, sizeof(m));
    TEST_ASSERT(StandFreshModelMake(&m, TAGFS_BLOCK_SIZE), "room for the files' bytes");
    TEST_ASSERT(StandConjugateFile(&m, "conjugate-a.bin", 1u, &a) && StandConjugateFile(&m, "conjugate-b.bin", 1u, &f),
                "two files of one content are made");
    TEST_ASSERT_OK(TagFSSettle(), "and settled");
    TEST_ASSERT(StandConjugateBlocks(a, 1u, theirs) && StandConjugateBlocks(f, 1u, mine), "each on its own ground");
    resting = FallowBlocks();
    ask     = (ConjugateAsk){ .fid = f, .at = 0, .onto = a, .onto_at = 0, .count = 1u };

    TEST_ASSERT_EQ(TagFSConjugate(INK_UNWARRANTED, &ask, &said), ERR_PERMISSION_DENIED,
                   "a cabin wearing neither system nor god is refused");
    TEST_ASSERT_EQ(said.why, CONJUGATE_UNWARRANTED, "and told so");
    TEST_ASSERT(StandConjugateUntouched(f, mine, theirs, 1u, resting), "nothing is touched");

    TEST_ASSERT_OK(TagFSConjugate(INK_WARRANTED, &ask, &said), "and with the warrant, B is laid on A");
    TEST_ASSERT(said.why == CONJUGATE_LAID && StandConjugateOnto(f, theirs, 1u), "B's block is A's");
    TEST_ASSERT(StandFreshHolds(&m, f) && StandFreshHolds(&m, a), "both read their bytes");
out:
    if (f)
        (void)tagfs_delete_file(f, INK_UNWARRANTED);
    if (a)
        (void)tagfs_delete_file(a, INK_UNWARRANTED);
    StandFreshModelLetGo(&m);
    return verdict;
}

// ----------------------------------------------------------------------------
// Kept: the kernel's file never shares, nor a file the system keeps
// ----------------------------------------------------------------------------

/* Block 0 of the kernel's file into `out`. */
static bool StandConjugateKernelBlock(uint64_t boot, uint8_t* out)
{
    TagFSFileHandle* h = tagfs_open(boot, TAGFS_HANDLE_READ);
    if (!h)
        return false;
    int got = TestHandleRead(h, out, TAGFS_BLOCK_SIZE);
    tagfs_close(h);
    return got == (int)TAGFS_BLOCK_SIZE;
}

static TestResult TestStandConjugateKept(void)
{
    TestResult        verdict = TEST_PASS;
    ConjugateSaid     said;
    ConjugateAsk      ask;
    uint64_t          boot = g_book.boot_fid, a = 0, f = 0, resting = 0, mine[1], theirs[1], kernel[1];
    const TagSpelling system = { .key = "system", .key_len = 6 };

    TEST_ASSERT(boot != 0 && StandConjugateBlocks(boot, 1u, kernel), "the book names the kernel's file");
    TEST_ASSERT(StandConjugateKernelBlock(boot, g_test_buffer), "its block 0 is read");
    TEST_ASSERT(tagfs_create_file("conjugate-kernel-a.bin", NULL, 0, 0, &a) == 0 &&
                    StandWriteAt(a, 0, g_test_buffer, TAGFS_BLOCK_SIZE) == (int)TAGFS_BLOCK_SIZE &&
                    tagfs_create_file("conjugate-kernel-b.bin", NULL, 0, 0, &f) == 0 &&
                    StandWriteAt(f, 0, g_test_buffer, TAGFS_BLOCK_SIZE) == (int)TAGFS_BLOCK_SIZE,
                "two files of its very bytes are made");
    TEST_ASSERT_OK(TagFSSettle(), "and settled");
    TEST_ASSERT(StandConjugateBlocks(a, 1u, theirs) && StandConjugateBlocks(f, 1u, mine), "each on its own ground");
    resting = FallowBlocks();

    ask = (ConjugateAsk){ .fid = f, .at = 0, .onto = boot, .onto_at = 0, .count = 1u };
    TEST_ASSERT_EQ(TagFSConjugate(INK_WARRANTED, &ask, &said), ERR_PERMISSION_DENIED,
                   "nothing is laid on the kernel's blocks, though B holds their very bytes");
    TEST_ASSERT_EQ(said.why, CONJUGATE_KEPT, "the kernel's file is kept");
    TEST_ASSERT(StandConjugateUntouched(f, mine, kernel, 1u, resting), "nothing is touched");

    ask = (ConjugateAsk){ .fid = boot, .at = 0, .onto = f, .onto_at = 0, .count = 1u };
    TEST_ASSERT_EQ(TagFSConjugate(INK_WARRANTED, &ask, &said), ERR_PERMISSION_DENIED,
                   "nor is the kernel's file laid on B's");
    TEST_ASSERT_EQ(said.why, CONJUGATE_KEPT, "the kernel's file is kept");
    TEST_ASSERT(StandConjugateUntouched(boot, kernel, mine, 1u, resting), "nothing is touched");

    TEST_ASSERT_OK(StandKeepInRole(f, TAGFS_FILE_ROLE_SYSTEM), "B is kept in the system's role");
    ask = (ConjugateAsk){ .fid = f, .at = 0, .onto = a, .onto_at = 0, .count = 1u };
    TEST_ASSERT_EQ(TagFSConjugate(INK_WARRANTED, &ask, &said), ERR_PERMISSION_DENIED,
                   "a file the system keeps is never laid, warrant or none");
    TEST_ASSERT_EQ(said.why, CONJUGATE_KEPT, "for the role it is kept in");
    TEST_ASSERT(StandConjugateUntouched(f, mine, theirs, 1u, resting), "nothing is touched");
    TEST_ASSERT_OK(StandKeepInRole(f, TAGFS_FILE_ROLE_ORDINARY), "B's role is let go");
    TEST_ASSERT_OK(tagfs_add_tag_string(TagOwnerPerson(), f, &system), "and B wears the word system, a tag like any other");
    TEST_ASSERT_OK(TagFSConjugate(INK_WARRANTED, &ask, &said), "B is laid on A's, the word keeping nothing");
out:
    if (f)
        (void)tagfs_delete_file(f, INK_WARRANTED);
    if (a)
        (void)tagfs_delete_file(a, INK_UNWARRANTED);
    return verdict;
}

static const TestCase g_stand_conjugate_tests[] = {
    { .name = "stand_conjugate_laid", .func = TestStandConjugateLaid, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_conjugate_changed", .func = TestStandConjugateChanged, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_conjugate_differs", .func = TestStandConjugateDiffers, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_conjugate_unwarranted", .func = TestStandConjugateUnwarranted, .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_conjugate_kept", .func = TestStandConjugateKept, .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_conjugate_suite = {
    g_stand_conjugate_tests, sizeof(g_stand_conjugate_tests) / sizeof(g_stand_conjugate_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
