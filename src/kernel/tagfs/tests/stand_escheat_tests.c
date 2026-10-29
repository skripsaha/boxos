#include "stand_binder.h"
#include "stand_fresh.h"
#include "../settle/binder.h"
#include "../settle/escheat.h"
#include "../settle/settle.h"
#include "../file_map/file_map.h"
#include "block_fail.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_escheat suite: the act that lets go what a book can never give
// back (spec_v2 §16.5 п.20)
//
// Escheat is asked only with the warrant a cabin wearing system or god
// carries; with nothing to let go it says so. A Slip's tail whose block cannot
// be read holds every Tally back for that file alone, said, until the act:
// then the tail is lost — the file cut to the end of its last whole block —
// and the Tally goes on. The ranges of FILES the act lets go are the landings'
// (logcheck book_escheat B4, B6c). Every test puts the Spine and the ribbon
// back as it found them.
// ============================================================================

#define STAND_ESCHEAT_ROOM  (4u * TAGFS_BLOCK_SIZE)

static StandBinder s_escheat_binder;

/* The size the file's record holds now. */
static uint64_t StandEscheatSize(uint64_t fid)
{
    FileRecord* rec  = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    uint64_t    size = rec ? rec->size : UINT64_MAX;
    FileRecordRelease(rec);
    return size;
}

static TestResult TestStandEscheatWarrant(void)
{
    TestResult  verdict = TEST_PASS;
    EscheatSaid said;

    TEST_ASSERT_EQ(TagFSEscheat(INK_UNWARRANTED, &said), ERR_PERMISSION_DENIED,
                   "a caller wearing neither system nor god is refused");
    TEST_ASSERT_EQ(TagFSEscheat(INK_WARRANTED, &said), ERR_OBJECT_NOT_FOUND,
                   "with no rotten range and no tail held back there is nothing to let go");
out:
    return verdict;
}

static TestResult TestStandEscheatTail(void)
{
    TestResult      verdict = TEST_PASS;
    StandBinder*    b = &s_escheat_binder;
    StandFreshModel m;
    EscheatSaid     said;
    FileSpot        spot = { 0 };
    FileRecord*     rec = NULL;
    uint64_t        fid = 0;
    bool            found = false;
    const char*     aside = StandBinderAside(b);

    if (aside)
        return test_steps_aside(aside);
    memset(&m, 0, sizeof(m));
    TEST_ASSERT(StandFreshModelMake(&m, STAND_ESCHEAT_ROOM), "room for the file's bytes");
    TEST_ASSERT_EQ(tagfs_create_file("escheat-tail.bin", NULL, 0, &fid), 0, "a file is made");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, 0, TAGFS_BLOCK_SIZE + 100u, 0x6e00u), TAGFS_BLOCK_SIZE + 100,
                   "a block and 100 bytes are written to it");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT_EQ(StandFreshWrite(&m, fid, m.size, 50, 0x6e01u), 50, "50 bytes ride a Slip at its end");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT(StandStitchRemount(), "the volume is mounted again: nothing of the file held in memory");
    rec   = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    found = rec && FileRecordFind(rec, 1, &spot) && spot.kind == STRIP_DATA;
    FileRecordRelease(rec);
    TEST_ASSERT(found, "the file's last block is DATA");
    BlockFailArmBadSectors(b->s.seat, tagfs_block_to_sector(spot.block), TAGFS_BLOCK_SECTORS);
    BinderDemanded();
    TEST_ASSERT_OK(TagFSSettle(), "its block cannot be read; a Tally is demanded, and the Helm's service says the "
                   "tail it cannot lay");
    TEST_ASSERT(BinderRunWhole() != OK, "a Tally asked for waits for that tail");
    TEST_ASSERT_EQ(TagFSEscheat(INK_UNWARRANTED, &said), ERR_PERMISSION_DENIED,
                   "Escheat asked without the warrant is refused, and the tail is held still");
    TEST_ASSERT(StandEscheatSize(fid) == TAGFS_BLOCK_SIZE + 150u, "nothing of the file changed");
    TEST_ASSERT_OK(TagFSEscheat(INK_WARRANTED, &said), "Escheat asked with the warrant");
    TEST_ASSERT(said.tails >= 1, "it names the tail held back");
    TEST_ASSERT_OK(BinderRunWhole(), "the Tally goes on: the tail is lost under the act");
    TEST_ASSERT_EQ(StandEscheatSize(fid), TAGFS_BLOCK_SIZE, "the file ends at its last whole block");
    BlockFailDisarm();
    m.size = TAGFS_BLOCK_SIZE;
    TEST_ASSERT(StandFreshHolds(&m, fid), "and reads the block before the tail");
    TEST_ASSERT(StandStitchRemount() && StandEscheatSize(fid) == TAGFS_BLOCK_SIZE && StandFreshHolds(&m, fid),
                "mounted again, the same");
out:
    BlockFailDisarm();
    StandFreshModelLetGo(&m);
    STAND_BINDER_PUT_BACK(b);
    return verdict;
}

// ----------------------------------------------------------------------------

static const TestCase g_stand_escheat_tests[] = {
    { .name = "stand_escheat_warrant", .func = TestStandEscheatWarrant,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
    { .name = "stand_escheat_tail", .func = TestStandEscheatTail,
      .reach = TEST_WRITES_TO_VOLUME, .format = DEED_FORMAT_V2 },
};

const TestSuite g_stand_escheat_suite = {
    g_stand_escheat_tests, sizeof(g_stand_escheat_tests) / sizeof(g_stand_escheat_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
