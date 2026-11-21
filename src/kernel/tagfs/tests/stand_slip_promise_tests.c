#include "tests.h"
#include "stand_tests.h"
#include "stand_stitch.h"
#include "stand_fresh.h"
#include "stand_binder.h"
#include "../tagfs.h"
#include "../helm/helm.h"
#include "../settle/binder.h"
#include "../settle/settle.h"
#include "../settle/sewing.h"
#include "../file_map/file_map.h"
#include "../block_map/block_map_derived.h"
#include "op_registry.h"
#include "boxos_decks.h"
#include "boxos_system_info.h"   /* SystemResources — what system.resources answers */
#include "atomics.h"
#include "cpu_calibrate.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The stand_slip_promise suite: a Slip promises its block
//
// A write that rides a Slip takes no ground now, and its tail is laid in a
// block before the next Tally (Engross) — so every file a Slip lies over keeps
// one block of the volume promised to it from the write that gave it the Slip
// to the one that lets it go: a Slip is given only while a block past the
// book's reserve and the promises standing is free, the next small write goes
// the block's road and is refused ERR_TAGFS_NO_SPACE as a block is, and on a
// volume left with no block past its reserve the Engross lays every tail on
// the block promised to it and the Tally goes — and on one left with no block
// but the promised, the reserve be what it may, the same. A Slip let go — its
// file given up, cut to nothing — gives its promise back; a mount holds every
// Slip it carries on promised again; and the free ground a program is told of
// is the free ground less what is promised. A Slip given or refused at the
// wall is weighed where the measure stands, the wall's wait on a Tally held
// down (BinderStandNoRoomWait): a Tally written there for room lays every
// tail, and the promises go with it before the test has counted them.
// ============================================================================

#define STAND_PROMISE_ROOM   3u      /* blocks past the reserve a filled volume keeps free */
#define STAND_PROMISE_FILES  (STAND_PROMISE_ROOM + 1u)
#define STAND_PROMISE_LINE   100u

/* The Spine and the ribbon as a Tally test found them: too large for a stack. */
static StandBinder s_promise_binder;

/* The free blocks a program is told the volume has (system.resources): its
 * free bytes over a block's. UINT64_MAX when the deck would not answer. */
static uint64_t StandPromiseSeen(void)
{
    const OpRegistration* reg = OpRegistryLookup(OP_KIND(DECK_SYSTEM, SYSTEM_OP_RESOURCES));
    SystemResources       res;
    ManifestOp            op   = { .op_kind = OP_KIND(DECK_SYSTEM, SYSTEM_OP_RESOURCES),
                                   .in_crate = CRATE_INDEX_NONE, .out_crate = 0 };
    Crate                 out  = { .magic = CRATE_MAGIC, .addr = (uint64_t)(uintptr_t)&res,
                                   .capacity = sizeof(res) };
    OpContext             ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (!reg || reg->handler(&op, &out, 1, &ctx) != OK || res.volume_free_counted != 1)
        return UINT64_MAX;
    return res.volume_free_bytes / TAGFS_BLOCK_SIZE;
}

/* Whether Slips lie over the file's last block. */
static bool StandPromiseSlipped(uint64_t fid)
{
    FileRecord* rec  = FileMapRecordHold(tagfs_get_state()->files, fid, NULL);
    bool        slip = rec && rec->slip != NULL;
    FileRecordRelease(rec);
    return slip;
}

/* `bytes` of the file's own pattern written at `at` through a handle of its
 * own: the write's answer — a write that moved fewer bytes than asked is a
 * defect of the write, answered as one. */
static error_t StandPromiseWrite(uint64_t fid, uint64_t at, uint32_t bytes)
{
    TagFSFileHandle* h       = tagfs_open(fid, TAGFS_HANDLE_READ | TAGFS_HANDLE_WRITE);
    uint64_t         written = 0;
    PlotSource       src     = { .kernel = g_test_buffer, .bytes = bytes };
    if (!h)
        return ERR_FILE_NOT_FOUND;
    TestFillAt(g_test_buffer, bytes, fid, at);
    h->offset  = at;
    error_t rc = tagfs_write(h, &src, &written);
    tagfs_close(h);
    return rc == OK && written != bytes ? ERR_TAGFS_EXTENT_ERROR : rc;
}

/* Whether the file is `bytes` long and reads its pattern, written from 0 on. */
static bool StandPromiseHolds(uint64_t fid, uint32_t bytes)
{
    TestFillAt(g_test_buffer, bytes, fid, 0);
    return TestFileRead(fid, g_test_output, sizeof(g_test_output)) == (int)bytes &&
           TestFirstDifference(g_test_buffer, g_test_output, bytes) == bytes;
}

/* `n` empty files made, settled, and every block Fallow rested given back,
 * then the volume cut to `room` blocks past the reserve a write of data
 * leaves — the Tally's two copies and a Quire of ribbon. */
static bool StandPromiseFilled(const StandStitch* s, uint64_t* fids, uint32_t n, uint64_t room, bool* shrunk)
{
    for (uint32_t i = 0; i < n; i++)
        if (tagfs_create_file("promise.bin", NULL, 0, 0, &fids[i]) != 0)
            return false;
    if (TagFSSettle() != OK || SettleForRoom() != OK)
        return false;
    *shrunk = true;
    return BlockMapDerivedStandShrink(EndpaperKeeps(s->at.quire_blocks) + s->at.quire_blocks + room);
}

/* The Helm's service run — here and on whichever core finds it listed first —
 * until no Slip lies over the file's last block, for STAND_FRESH_PORT_MS at
 * the most: whether none does. */
static bool StandPromiseEngrossed(uint64_t fid)
{
    uint64_t bound = rdtsc() + cpu_ms_to_tsc(STAND_FRESH_PORT_MS);
    do {
        HelmRunReady();
        if (!StandPromiseSlipped(fid))
            return true;
        cpu_pause();
    } while (rdtsc() < bound);
    return false;
}

/* A volume of three blocks past its reserve: three files are given a Slip,
 * each promised its block, and the fourth file's line is refused — no block
 * is left to promise it — as a block of it is; a program is told the free
 * ground less the three promised. */
static TestResult TestStandSlipPromiseRefused(void)
{
    TestResult  verdict = TEST_PASS;
    StandStitch s;
    uint64_t    fids[STAND_PROMISE_FILES] = { 0 };
    bool        shrunk = false;
    const char* aside  = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    (void)BinderStandNoRoomWait(true);
    TEST_ASSERT(StandPromiseFilled(&s, fids, STAND_PROMISE_FILES, STAND_PROMISE_ROOM, &shrunk),
                "four files are made, and the volume cut to three blocks past its reserve");
    for (uint32_t i = 0; i < STAND_PROMISE_ROOM; i++) {
        TEST_ASSERT_OK(StandPromiseWrite(fids[i], 0, STAND_PROMISE_LINE), "a line is written to a file");
        TEST_ASSERT(StandPromiseSlipped(fids[i]), "and rides a Slip, its block promised");
    }
    TEST_ASSERT_EQ(StandPromiseWrite(fids[STAND_PROMISE_ROOM], 0, STAND_PROMISE_LINE), ERR_TAGFS_NO_SPACE,
                   "the fourth file's line is refused: no block is left to promise its Slip");
    TEST_ASSERT(!StandPromiseSlipped(fids[STAND_PROMISE_ROOM]), "and no Slip lies over it");
    TEST_ASSERT_EQ(StandPromiseWrite(fids[STAND_PROMISE_ROOM], 0, TAGFS_BLOCK_SIZE), ERR_TAGFS_NO_SPACE,
                   "as a write of a whole block is");
    TEST_ASSERT_EQ(StandPromiseSeen(), tagfs_get_state()->free_blocks - STAND_PROMISE_ROOM,
                   "a program is told the free ground less the three blocks promised");
out:
    (void)BinderStandNoRoomWait(false);
    if (shrunk)
        BlockMapDerivedStandGrow();
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

/* Slips on a volume left with no block past its reserve, and a Tally asked:
 * the Helm's service lays every tail on the block promised to it, and the
 * Tally is written. A volume that gave a Slip to a file it had no block for
 * leaves that tail with no ground, and the Tally waiting for it. */
static TestResult TestStandSlipPromiseTally(void)
{
    TestResult   verdict = TEST_PASS;
    StandBinder* b = &s_promise_binder;
    uint64_t     fids[STAND_PROMISE_FILES] = { 0 };
    uint32_t     lines = 0;
    bool         shrunk = false, slipped[STAND_PROMISE_FILES] = { false };
    const char*  aside = StandBinderAside(b);

    if (aside)
        return test_steps_aside(aside);
    (void)BinderStandNoRoomWait(true);
    TEST_ASSERT(StandPromiseFilled(&b->s, fids, STAND_PROMISE_FILES, STAND_PROMISE_ROOM, &shrunk),
                "four files are made, and the volume cut to three blocks past its reserve");
    for (uint32_t i = 0; i < STAND_PROMISE_FILES; i++) {
        error_t rc = StandPromiseWrite(fids[i], 0, STAND_PROMISE_LINE);
        TEST_ASSERT(rc == OK || rc == ERR_TAGFS_NO_SPACE, "a line is written to each file, or refused no space");
        slipped[i] = rc == OK && StandPromiseSlipped(fids[i]);
        TEST_ASSERT(rc != OK || slipped[i], "a line written rides a Slip");
        lines += rc == OK;
    }
    TEST_ASSERT(lines >= STAND_PROMISE_ROOM, "every block past the reserve gave a Slip");
    (void)BinderStandNoRoomWait(false);
    TEST_ASSERT_OK(TagFSSettle(), "the Slips settle");
    BinderDemanded();
    TEST_ASSERT_OK(TagFSSettle(), "a Tally is demanded, and a cycle weighs it");
    for (uint32_t i = 0; i < STAND_PROMISE_FILES; i++)
        TEST_ASSERT(!slipped[i] || StandPromiseEngrossed(fids[i]),
                    "the Helm's service lays each tail in a block — the one promised to it");
    TEST_ASSERT_OK(TagFSSettle(), "the cycle that carries them settles");
    TEST_ASSERT_OK(BinderRunWhole(), "and the Tally is written on the volume left with no block past its reserve");
    for (uint32_t i = 0; i < STAND_PROMISE_FILES; i++)
        TEST_ASSERT(!slipped[i] || StandPromiseHolds(fids[i], STAND_PROMISE_LINE), "every file reads its line");
    BlockMapDerivedStandGrow();
    shrunk = false;
    TEST_ASSERT(StandStitchRemount(), "the volume is mounted again on the Tally");
    for (uint32_t i = 0; i < STAND_PROMISE_FILES; i++)
        TEST_ASSERT(!slipped[i] || (StandPromiseHolds(fids[i], STAND_PROMISE_LINE) && !StandPromiseSlipped(fids[i])),
                    "and every file reads its line out of its block");
out:
    (void)BinderStandNoRoomWait(false);
    if (shrunk)
        BlockMapDerivedStandGrow();
    STAND_BINDER_PUT_BACK(b);
    return verdict;
}

/* Slips on a volume then left with no block free but the three promised to
 * them — as one whose reserve's measure grew past its ground since they were
 * given: the Helm's service lays every tail on the block promised to it, the
 * reserve be what it may, and the volume given its ground back, the Tally is
 * written. A volume that gave a Slip it had no block for, or held the block
 * promised behind its reserve, leaves a tail with no ground and the Tally
 * waiting for it. */
static TestResult TestStandSlipPromiseOwn(void)
{
    TestResult   verdict = TEST_PASS;
    StandBinder* b = &s_promise_binder;
    uint64_t     fids[STAND_PROMISE_FILES] = { 0 };
    uint32_t     lines = 0;
    bool         shrunk = false, slipped[STAND_PROMISE_FILES] = { false };
    const char*  aside = StandBinderAside(b);

    if (aside)
        return test_steps_aside(aside);
    (void)BinderStandNoRoomWait(true);
    TEST_ASSERT(StandPromiseFilled(&b->s, fids, STAND_PROMISE_FILES, STAND_PROMISE_ROOM, &shrunk),
                "four files are made, and the volume cut to three blocks past its reserve");
    for (uint32_t i = 0; i < STAND_PROMISE_FILES; i++) {
        error_t rc = StandPromiseWrite(fids[i], 0, STAND_PROMISE_LINE);
        TEST_ASSERT(rc == OK || rc == ERR_TAGFS_NO_SPACE, "a line is written to each file, or refused no space");
        slipped[i] = rc == OK && StandPromiseSlipped(fids[i]);
        TEST_ASSERT(rc != OK || slipped[i], "a line written rides a Slip");
        lines += rc == OK;
    }
    TEST_ASSERT(lines >= STAND_PROMISE_ROOM, "every block past the reserve gave a Slip");
    (void)BinderStandNoRoomWait(false);
    TEST_ASSERT_OK(TagFSSettle(), "the Slips settle");
    TEST_ASSERT(BlockMapDerivedStandShrink(STAND_PROMISE_ROOM),
                "the volume is cut to three free blocks: the reserve's measure stands past all its ground");
    BinderDemanded();
    TEST_ASSERT_OK(TagFSSettle(), "a Tally is demanded, and a cycle weighs it");
    for (uint32_t i = 0; i < STAND_PROMISE_FILES; i++)
        TEST_ASSERT(!slipped[i] || StandPromiseEngrossed(fids[i]),
                    "the Helm's service lays each tail in a block — the one promised to it, past no reserve");
    TEST_ASSERT_EQ(tagfs_get_state()->free_blocks, 0u, "the three blocks promised are the three laid");
    BlockMapDerivedStandGrow();
    shrunk = false;
    TEST_ASSERT_OK(TagFSSettle(), "the volume given its ground back, the cycle that carries them settles");
    TEST_ASSERT_OK(BinderRunWhole(), "and the Tally is written");
    for (uint32_t i = 0; i < STAND_PROMISE_FILES; i++)
        TEST_ASSERT(!slipped[i] || StandPromiseHolds(fids[i], STAND_PROMISE_LINE), "every file reads its line");
out:
    (void)BinderStandNoRoomWait(false);
    if (shrunk)
        BlockMapDerivedStandGrow();
    STAND_BINDER_PUT_BACK(b);
    return verdict;
}

/* Two files with Slips on a volume of two blocks past its reserve: a block of
 * a third is refused. The first file given up, the second cut to nothing,
 * each gives its promise back — a program is told one block more free each
 * time — and the third is given two blocks, and no more. */
static TestResult TestStandSlipPromiseGivenBack(void)
{
    TestResult  verdict = TEST_PASS;
    StandStitch s;
    uint64_t    fids[3] = { 0 }, seen = 0;
    bool        shrunk  = false;
    const char* aside   = StandStitchAside(&s);

    if (aside)
        return test_steps_aside(aside);
    (void)BinderStandNoRoomWait(true);
    TEST_ASSERT(StandPromiseFilled(&s, fids, 3, 2, &shrunk), "three files are made, and the volume cut to two "
                                                             "blocks past its reserve");
    TEST_ASSERT(StandPromiseWrite(fids[0], 0, STAND_PROMISE_LINE) == OK && StandPromiseSlipped(fids[0]) &&
                StandPromiseWrite(fids[1], 0, STAND_PROMISE_LINE) == OK && StandPromiseSlipped(fids[1]),
                "a line rides a Slip on each of the first two");
    TEST_ASSERT_EQ(StandPromiseWrite(fids[2], 0, TAGFS_BLOCK_SIZE), ERR_TAGFS_NO_SPACE,
                   "a block of the third is refused: both blocks are promised");
    seen = StandPromiseSeen();
    TEST_ASSERT_EQ(seen, tagfs_get_state()->free_blocks - 2u, "a program is told the free ground less both");
    TEST_ASSERT_EQ(tagfs_delete_file(fids[0], INK_UNWARRANTED), 0, "the first file is given up");
    TEST_ASSERT_EQ(StandPromiseSeen(), seen + 1u, "and a program is told the block promised to it is free");
    TEST_ASSERT_OK(tagfs_span_file(fids[1], 0), "the second is cut to nothing");
    TEST_ASSERT(!StandPromiseSlipped(fids[1]), "its Slip is let go");
    TEST_ASSERT_EQ(StandPromiseSeen(), seen + 2u, "and its promise given back");
    TEST_ASSERT_OK(StandPromiseWrite(fids[2], 0, TAGFS_BLOCK_SIZE), "a block of the third is had now");
    TEST_ASSERT_OK(StandPromiseWrite(fids[2], TAGFS_BLOCK_SIZE, TAGFS_BLOCK_SIZE), "and a second");
    TEST_ASSERT_EQ(StandPromiseWrite(fids[2], 2u * TAGFS_BLOCK_SIZE, TAGFS_BLOCK_SIZE), ERR_TAGFS_NO_SPACE,
                   "and no more: nothing came back but the two promises");
out:
    (void)BinderStandNoRoomWait(false);
    if (shrunk)
        BlockMapDerivedStandGrow();
    STAND_STITCH_PUT_BACK(&s);
    return verdict;
}

/* Two files with Slips, settled, and the volume mounted again: both blocks
 * are promised again. Cut then to one block past its reserve and the two
 * promised, the volume gives one Slip more and refuses the next; a Tally asked
 * lays all three tails on the blocks promised to them, and is written. */
static TestResult TestStandSlipPromiseRemount(void)
{
    TestResult   verdict = TEST_PASS;
    StandBinder* b = &s_promise_binder;
    uint64_t     fids[4] = { 0 };
    bool         shrunk  = false;
    const char*  aside   = StandBinderAside(b);

    if (aside)
        return test_steps_aside(aside);
    for (uint32_t i = 0; i < 4u; i++)
        TEST_ASSERT_EQ(tagfs_create_file("promise.bin", NULL, 0, 0, &fids[i]), 0, "a file is made");
    TEST_ASSERT(StandPromiseWrite(fids[0], 0, STAND_PROMISE_LINE) == OK &&
                StandPromiseWrite(fids[1], 0, STAND_PROMISE_LINE) == OK, "a line is written to each of two");
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    TEST_ASSERT(StandStitchRemount(), "the volume is mounted again");
    TEST_ASSERT(StandPromiseSlipped(fids[0]) && StandPromiseSlipped(fids[1]), "both Slips are carried on");
    TEST_ASSERT_EQ(StandPromiseSeen(), tagfs_get_state()->free_blocks - 2u,
                   "and a program is told the free ground less a block promised to each");
    TEST_ASSERT_OK(SettleForRoom(), "nothing rests in Fallow");
    shrunk = true;
    (void)BinderStandNoRoomWait(true);
    TEST_ASSERT(BlockMapDerivedStandShrink(EndpaperKeeps(b->s.at.quire_blocks) + b->s.at.quire_blocks + 3u),
                "the volume is cut to three blocks past its reserve: two promised, and one");
    TEST_ASSERT(StandPromiseWrite(fids[2], 0, STAND_PROMISE_LINE) == OK && StandPromiseSlipped(fids[2]),
                "one Slip more is given: the one block past the reserve and the promises");
    TEST_ASSERT_EQ(StandPromiseWrite(fids[3], 0, STAND_PROMISE_LINE), ERR_TAGFS_NO_SPACE,
                   "and the next refused: the two promised before the mount stand");
    (void)BinderStandNoRoomWait(false);
    TEST_ASSERT_OK(TagFSSettle(), "settled");
    BinderDemanded();
    TEST_ASSERT_OK(TagFSSettle(), "a Tally is demanded, and a cycle weighs it");
    for (uint32_t i = 0; i < 3u; i++)
        TEST_ASSERT(StandPromiseEngrossed(fids[i]), "each tail is laid on the block promised to it");
    TEST_ASSERT_OK(TagFSSettle(), "the cycle that carries them settles");
    TEST_ASSERT_OK(BinderRunWhole(), "and the Tally is written");
    for (uint32_t i = 0; i < 3u; i++)
        TEST_ASSERT(StandPromiseHolds(fids[i], STAND_PROMISE_LINE), "every file reads its line");
out:
    (void)BinderStandNoRoomWait(false);
    if (shrunk)
        BlockMapDerivedStandGrow();
    STAND_BINDER_PUT_BACK(b);
    return verdict;
}

static const TestCase g_stand_slip_promise_tests[] = {
    { .name = "stand_slip_promise_refused", .func = TestStandSlipPromiseRefused,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_slip_promise_tally", .func = TestStandSlipPromiseTally,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_slip_promise_own", .func = TestStandSlipPromiseOwn,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_slip_promise_given_back", .func = TestStandSlipPromiseGivenBack,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_slip_promise_remount", .func = TestStandSlipPromiseRemount,
      .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_slip_promise_suite = {
    g_stand_slip_promise_tests, sizeof(g_stand_slip_promise_tests) / sizeof(g_stand_slip_promise_tests[0])
};

#endif /* CONFIG_VOLUME_TESTS */
