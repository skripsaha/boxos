#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../integrity/integrity.h"
#include "../metadata_pool/meta_pool.h"
#include "../block_map/block_map.h"
#include "../glean/glean.h"
#include "../ink/ink.h"
#include "../settle/settle.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The integrity map covers no block of the volume's own (RT-22)
//
// The map's own pages, the run a bigger map is laid into, and every block of
// the pool, the chains and their stamps go through the meta doors, which
// neither record a digest nor check one (tagfs.c). A page pushed out into a
// map being laid used to record a digest of its own on the way, and push the
// next page out to make room for it — as deep as the map has slots; and a pool
// block closed in a cycle had its digest reach the medium only with the next
// cycle's map, so a stop in between left Glean a "rotten" block for good.
// ============================================================================

#define STAND_MAP_PAGE_BLOCKS 512u    /* blocks one page of the integrity map covers */
#define STAND_MAP_PAGES       17u     /* one page more than the map keeps in memory */
#define STAND_STALE_BYTES     64u     /* an overwrite that leaves a dead record behind */

static uint64_t StandIntegrityPageOf(uint64_t block)
{
    return block / STAND_MAP_PAGE_BLOCKS;
}

/*
 * A map laid into a new run, every page of the one it replaces carried — and
 * then one block written under each of seventeen pages far from that run, one
 * page more than the map keeps in memory. The seventeenth pushes a dirty page
 * out, into both runs, and the run's page is not in memory: had that write
 * recorded a digest, it would have pushed the next dirty page out for it, and
 * that one the next. Then a block of the new run written as a file's block is
 * written: the map takes no digest of it, so bytes of anybody's are not rot
 * there.
 */
static TestResult TestStandIntegrityLayPagesTakeNoDigest(void)
{
    TestResult  verdict = TEST_PASS;
    uint32_t    span    = (STAND_MAP_PAGES - 1u) * STAND_MAP_PAGE_BLOCKS + 1u;
    uint32_t    run     = tagfs_get_state()->layout.data_blocks;
    uint64_t    first   = 0;
    bool        laying  = false;
    bool        taken   = false;
    error_t     wrote   = OK;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);
    if (!IntegrityIsInitialized())
        return test_steps_aside("no integrity map stands on this volume, so none is laid over it");
    if (run < 2u * span + STAND_MAP_PAGE_BLOCKS)
        return test_steps_aside("the data run is too short for seventeen pages of the map apart "
                                "from the run a map is laid into");

    TEST_ASSERT_OK(TagFSSettle(), "a Settle cycle first, so no page of the map is owed to the medium");
    TEST_ASSERT_OK(IntegrityStandLayBegin(), "a map is laid into a new run, every page carried");
    laying = true;
    uint32_t lay = IntegrityStandLayFirst();
    TEST_ASSERT(lay != 0, "and the run is named while it is laid");

    TEST_ASSERT_OK(BlockMapTake(span, run / 2u, &first, __func__),
                   "a run under seventeen pages of the map is taken halfway along the data run");
    taken = true;
    TEST_ASSERT(StandIntegrityPageOf(first) > StandIntegrityPageOf(lay + IntegrityLayBlocks() - 1u),
                "past every page of the map that covers the run being laid");

    TestFillAt(g_test_buffer, TAGFS_BLOCK_SIZE, 0x1A70u, 0);
    (void)IntegrityStandUpdateDeepest();
    for (uint32_t k = 0; k < STAND_MAP_PAGES && wrote == OK; k++)
        wrote = tagfs_write_block(first + k * STAND_MAP_PAGE_BLOCKS, g_test_buffer);
    uint32_t deepest = IntegrityStandUpdateDeepest();
    TEST_ASSERT_OK(wrote, "one block is written under every page");
    for (uint32_t k = 0; k < STAND_MAP_PAGES; k++)
        TEST_ASSERT(IntegrityVerify(first + k * STAND_MAP_PAGE_BLOCKS, g_test_buffer),
                    "and the map holds each one's digest");

    uint32_t errors = IntegrityErrorCount();
    TestFillAt(g_test_buffer, TAGFS_BLOCK_SIZE, 0x1A71u, 0);
    TEST_ASSERT_OK(tagfs_write_block(lay, g_test_buffer),
                   "a block of the run being laid is written through a file's door");
    TestFillAt(g_test_output, TAGFS_BLOCK_SIZE, 0x1A72u, 0);
    TEST_ASSERT(IntegrityVerify(lay, g_test_output),
                "and other bytes there are not rot: the map took no digest of its own block");
    TEST_ASSERT_EQ(IntegrityErrorCount(), errors, "no rot counted");

    TestEvidence("deepest nesting of IntegrityUpdate while map pages went into a map being laid",
                 deepest);
    if (deepest > 1u)
        verdict = TEST_DEFECT;
out:
    if (laying)
        IntegrityStandLayEnd();
    if (taken)
        tagfs_free_blocks(first, span);
    return verdict;
}

/* Under the state lock: the first link from the chain's front that holds a
 * live record — the one Glean reads next — or false. */
static bool StandIntegrityLiveFront(PoolFront* out)
{
    TagFSState* fs = tagfs_get_state();
    bool        found = false;
    TagFSLockTake(&fs->lock);
    for (uint32_t i = 0; PoolFrontLink(i, out); i++) {
        if (out->live) {
            found = true;
            break;
        }
    }
    TagFSLockDrop(&fs->lock);
    return found;
}

static bool StandIntegrityChainHolds(uint32_t block, uint32_t* chain, uint32_t* live)
{
    TagFSState* fs = tagfs_get_state();
    TagFSLockTake(&fs->lock);
    bool holds = PoolChainHolds(block);
    *chain = PoolChainBlocks();
    *live  = PoolLiveBlocks();
    TagFSLockDrop(&fs->lock);
    return holds;
}

/*
 * X2-1. A closed block of the pool whose digest on the map is not the digest
 * of what it holds — what a stop between the cycle that closed it and the next
 * one's map used to leave — is read by Glean all the same, and taken off the
 * chain once its live records lie further on. The digest is put there by hand,
 * and a check of the block against the map shows it stands. Then a file is
 * written again, a cycle each time, until the chain is past its slack and
 * Glean comes to the block. Red before: Glean's read was refused as rot every
 * cycle, the block stayed on the chain for good, and the chain grew.
 */
static TestResult TestStandIntegrityPoolBlockReadsPastAStaleDigest(void)
{
    TestResult  verdict = TEST_PASS;
    uint64_t    cold    = 0;
    uint64_t    writer  = 0;
    uint32_t    rounds  = 0;
    PoolFront   front;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);
    if (!IntegrityIsInitialized())
        return test_steps_aside("no integrity map stands on this volume, so no digest can go stale");

    TestFillAt(g_test_buffer, STAND_STALE_BYTES, 0x5A1Eu, 0);
    TEST_ASSERT(StandMakeFile("stale_cold", g_test_buffer, STAND_STALE_BYTES, &cold),
                "a file whose record stays live is made");
    TEST_ASSERT(StandMakeFile("stale_writer", g_test_buffer, STAND_STALE_BYTES, &writer),
                "and one to write again");
    TEST_ASSERT_OK(TagFSSettle(), "a cycle closes the block their records lie in");
    TEST_ASSERT(StandIntegrityLiveFront(&front), "a closed block with live records leads the chain");

    MetaPoolBlock* image = (MetaPoolBlock*)g_test_output;
    TEST_ASSERT_OK(TagFSMetaBlockRead(front.block, image), "the block reads through the meta door");
    TestFillAt(g_test_buffer, TAGFS_BLOCK_SIZE, 0x5A1Fu, 0);
    IntegrityUpdate(front.block, g_test_buffer);
    TEST_ASSERT(!IntegrityVerify(front.block, image),
                "its digest on the map is now another block's: checked against it, it is rot");
    uint32_t errors = IntegrityErrorCount();

    uint32_t chain = 0, live = 0;
    (void)StandIntegrityChainHolds(front.block, &chain, &live);
    uint32_t bound = 2u * (GLEAN_SLACK * live + 1u) + chain + 8u;
    while (StandIntegrityChainHolds(front.block, &chain, &live) && rounds < bound) {
        TestFillAt(g_test_buffer, STAND_STALE_BYTES, writer, rounds);
        TEST_ASSERT_EQ(StandWriteAt(writer, 0, g_test_buffer, STAND_STALE_BYTES),
                       (int)STAND_STALE_BYTES, "the file is written again");
        TEST_ASSERT_OK(TagFSSettle(), "and a cycle settles it");
        rounds++;
    }
    TEST_ASSERT_EQ(IntegrityErrorCount(), errors, "Glean's reads counted no rot");

    bool stayed = StandIntegrityChainHolds(front.block, &chain, &live);
    TestEvidence("the pool block with a stale digest still on the chain after this many cycles",
                 stayed ? rounds : 0);
    if (stayed)
        verdict = TEST_DEFECT;
    TestFillAt(g_test_buffer, STAND_STALE_BYTES, 0x5A1Eu, 0);
    TEST_ASSERT_EQ(TestFileRead(cold, g_test_output, STAND_STALE_BYTES), (int)STAND_STALE_BYTES,
                   "the file whose record was moved reads");
    TEST_ASSERT_EQ(TestFirstDifference(g_test_output, g_test_buffer, STAND_STALE_BYTES),
                   STAND_STALE_BYTES, "byte for byte");
out:
    if (cold)
        (void)tagfs_delete_file(cold, INK_UNWARRANTED);
    if (writer)
        (void)tagfs_delete_file(writer, INK_UNWARRANTED);
    (void)TagFSSettle();
    return verdict;
}

static const TestCase g_stand_integrity_tests[] = {
    { .name  = "stand_integrity_lay_pages_take_no_digest",
      .func  = TestStandIntegrityLayPagesTakeNoDigest, .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a page of the integrity map pushed out into a map being laid records a "
                       "digest of its own block, which pushes the next dirty page out for it — "
                       "as deep as the map has slots, on the deepest road to the medium" },
    { .name  = "stand_integrity_pool_block_reads_past_a_stale_digest",
      .func  = TestStandIntegrityPoolBlockReadsPastAStaleDigest, .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a closed pool block whose digest went stale in a stop between two cycles "
                       "is read by Glean as rot every cycle, stays on the chain for good, and the "
                       "chain grows without bound" },
};

const TestSuite g_stand_integrity_suite = {
    .tests = g_stand_integrity_tests, .count = ARRAY_SIZE(g_stand_integrity_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
