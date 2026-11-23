#include "stand_stitch.h"
#include "stand_fresh.h"
#include "stand_binder.h"
#include "../dedup/conjugate.h"
#include "../ink/fallow.h"
#include "../ink/commonhold.h"
#include "../mount/book_mount.h"
#include "../mount/under_glass.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_commonhold suite: a block more than one file names
//
// A run SHARED names ground another run may name too. Its file reads through
// it as through DATA, each block held to the block's one digest; its stitch
// names no fresh block for it and carries no digest; the mount takes it again
// as memory held it, and the census finds no two owners where one DATA run and
// SHARED runs meet. How many name a block is memory's alone (Commonhold),
// derived by every mount: a block leaves no owner's hands while one names it,
// and rests in Fallow once — when its last owner lets it go. The run SHARED is
// laid by Dedup's act (Conjugate, dedup/conjugate.h).
// ============================================================================

#define STAND_SHARE_BLOCKS  3u
#define STAND_SHARE_STREAM  41u

/* A file of STAND_SHARE_BLOCKS blocks of the share's stream: its id in *fid. */
static bool StandShareFile(TagFSVolume* vol, StandFreshModel* m, const char* name, uint64_t* fid)
{
    m->size = 0;
    return tagfs_create_file(vol, name, NULL, 0, 0, fid) == 0 &&
           StandFreshWrite(vol, m, *fid, 0, STAND_SHARE_BLOCKS * TAGFS_BLOCK_SIZE, STAND_SHARE_STREAM) ==
               (int)(STAND_SHARE_BLOCKS * TAGFS_BLOCK_SIZE);
}

/* The census of the last mount found no block named by two owners, and left
 * the volume to be written on. */
static bool StandShareCensusClean(TagFSVolume* vol)
{
    MountBookSaid said;
    MountBookStandSaid(vol, &said);
    return said.doubles == 0 && !TagFSIsUnderGlass(vol);
}

static TestResult TestStandCommonholdOwners(void)
{
    TagFSVolume* vol = TagFSHome();
    TestResult      verdict = TEST_PASS;
    StandBinder     b;
    StandFreshModel m;
    ConjugateSaid   said;
    ConjugateAsk    ask;
    uint64_t        a = 0, f = 0, a0 = 0, b0 = 0, resting = 0, flags = 0, fresh = 0;
    const char*     aside = StandBinderAside(vol, &b);

    if (aside)
        return test_steps_aside(aside);
    TEST_ASSERT(StandFreshModelMake(&m, STAND_SHARE_BLOCKS * TAGFS_BLOCK_SIZE), "room for the files' bytes");
    TEST_ASSERT(StandShareFile(vol, &m, "commonhold-a.bin", &a), "file A is made and written");
    TEST_ASSERT(StandShareFile(vol, &m, "commonhold-b.bin", &f), "file B is made and written with A's very bytes");
    TEST_ASSERT_OK(TagFSSettle(vol), "and both settled");
    TEST_ASSERT(StandBlockOf(vol, a, 0, &a0) && StandBlockOf(vol, f, 0, &b0) && a0 != b0, "each on its own ground");

    resting = FallowBlocks(vol);
    ask     = (ConjugateAsk){ .fid = f, .at = 0, .onto = a, .onto_at = 0, .count = 1 };
    TEST_ASSERT_OK(TagFSConjugate(vol, INK_WARRANTED, &ask, &said), "B's block 0 is laid SHARED on A's block 0 by the act");
    TEST_ASSERT_EQ(CommonholdOwners(vol, a0), 2, "A's block 0 has two owners");
    TEST_ASSERT_EQ(FallowBlocks(vol), resting + 1u, "and B's own block 0 rests: the one block the share let go");
    TEST_ASSERT(StandBlockOf(vol, f, 0, &b0) && b0 == a0, "B's block 0 is A's");
    TEST_ASSERT(StandFreshHolds(vol, &m, f), "B reads A's block, held to the block's one digest");
    TEST_ASSERT(TagFSSettle(vol) == OK && StandFreshLastStitch(vol, &flags, &fresh) && fresh == 0,
                "settled, its stitch names no fresh block: a SHARED run is not fresh (§2.7)");
    TEST_ASSERT(StandFreshSettledAndBack(vol, &m, f), "mounted again, B's runs are the ones memory held, Strip for Strip");
    TEST_ASSERT(StandShareCensusClean(vol), "the census finds no block of two owners, and the volume is written on");
    TEST_ASSERT_EQ(CommonholdOwners(vol, a0), 2, "and the mount derives the two owners again");

    resting = FallowBlocks(vol);
    TEST_ASSERT_EQ(tagfs_delete_file(vol, a, INK_UNWARRANTED), 0, "A is given up");
    TestEvidence("blocks A's leaving set to rest, of its three, one of them shared", FallowBlocks(vol) - resting);
    TEST_ASSERT_EQ(FallowBlocks(vol), resting + 2u, "its blocks 1 and 2 rest, and block 0 stays B's");
    TEST_ASSERT_EQ(CommonholdOwners(vol, a0), 1, "block 0 has one owner left");
    TEST_ASSERT(StandFreshHolds(vol, &m, f), "and B reads it");
    TEST_ASSERT_OK(StandBinderTallies(vol, 1u), "a Tally is laid past it, held by its self-check to the files' ground");
    TEST_ASSERT(StandFreshSettledAndBack(vol, &m, f), "mounted again on it, B reads block 0 held to the Tally's digest");
    TEST_ASSERT(StandShareCensusClean(vol), "the census finds no block of two owners: B alone names it");
    TEST_ASSERT_EQ(CommonholdOwners(vol, a0), 1, "and the mount derives the one owner");

    resting = FallowBlocks(vol);
    TEST_ASSERT_EQ(tagfs_delete_file(vol, f, INK_UNWARRANTED), 0, "B is given up");
    TEST_ASSERT_EQ(FallowBlocks(vol), resting + STAND_SHARE_BLOCKS, "its three blocks rest, the shared one once");
    TEST_ASSERT_EQ(CommonholdOwners(vol, a0), 0, "and no one owns block 0");
out:
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(&b);
    return verdict;
}

static const TestCase g_stand_commonhold_tests[] = {
    { .name = "stand_commonhold_owners", .func = TestStandCommonholdOwners, .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_commonhold_suite = {
    g_stand_commonhold_tests, sizeof(g_stand_commonhold_tests) / sizeof(g_stand_commonhold_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
