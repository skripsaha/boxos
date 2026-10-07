#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../metadata_pool/meta_pool.h"
#include "../glean/glean.h"
#include "../ink/fallow.h"
#include "../ledger/ledger.h"
#include "../settle/settle.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// Glean: the pool's chain bounded by its live records (step 1.8)
//
// Every cycle of a file written again leaves one more record on the chain,
// and every record but the newest is dead: the chain grew without bound, and
// the mount walks all of it. Glean takes the dead blocks off the front and
// moves the live records of a block it takes into the tail (glean/glean.h).
// These drive the cycles themselves (the runner holds the cadence) and ask
// the chain in memory, the seal and the head on the medium, and the map.
// ============================================================================

/* Overwrites of one file, each settled by a cycle of its own. */
#define STAND_GLEAN_OVERWRITES  10000u

/* The bytes an overwrite writes at the start of the file. */
#define STAND_GLEAN_BYTES       64u

/* The chain's blocks and the blocks its live records fill, in memory. */
static void StandGleanChain(uint32_t* chain, uint32_t* live)
{
    TagFSState* fs = tagfs_get_state();
    TagFSLockTake(&fs->lock);
    *chain = PoolChainBlocks();
    *live  = PoolLiveBlocks();
    TagFSLockDrop(&fs->lock);
}

static uint32_t StandGleanFirst(void)
{
    TagFSState* fs = tagfs_get_state();
    TagFSLockTake(&fs->lock);
    uint32_t first = PoolChainFirst();
    TagFSLockDrop(&fs->lock);
    return first;
}

/* The Deed's block of the pool, in the data run. */
static uint32_t StandGleanHead(void)
{
    TagFSState* fs = tagfs_get_state();
    return fs->layout.metadata_pool_block - fs->layout.data_block;
}

/* The file written again at its start, and a cycle settling it. */
static bool StandGleanOverwrite(uint32_t file_id, uint32_t round)
{
    TestFillAt(g_test_buffer, STAND_GLEAN_BYTES, file_id, round);
    return StandWriteAt(file_id, 0, g_test_buffer, STAND_GLEAN_BYTES) == (int)STAND_GLEAN_BYTES &&
           TagFSSettle() == OK;
}

/* The seals of both copies of the Ledger on the medium, newest first: false
 * when the newest carries none. */
static bool StandGleanSeals(SettleMark* newest, SettleMark* other, bool* has_other)
{
    VolumeLedger led[VOLUME_LEDGER_COPIES];
    bool         ok[VOLUME_LEDGER_COPIES];
    for (uint32_t c = 0; c < VOLUME_LEDGER_COPIES; c++)
        ok[c] = tagfs_ledger_peek(c, &led[c], NULL) &&
                LedgerHas(&led[c], VOLUME_LEDGER_SEAL_END) && led[c].mark.settle != 0 &&
                led[c].mark.settle == led[c].seq;
    uint32_t n = ok[1] && (!ok[0] || led[1].seq > led[0].seq) ? 1u : 0u;
    if (!ok[n])
        return false;
    *newest    = led[n].mark;
    *has_other = ok[1u - n];
    if (*has_other)
        *other = led[1u - n].mark;
    return true;
}

/* The head as the medium holds it: its next_block, false when it does not
 * read as a block of the pool. */
static bool StandGleanHeadNext(uint32_t* next)
{
    MetaPoolBlock* blk = (MetaPoolBlock*)g_test_output;
    if (tagfs_read_block(StandGleanHead(), blk) != OK || blk->magic != TAGFS_MPOOL_MAGIC)
        return false;
    *next = blk->next_block;
    return true;
}

/*
 * Overwrites of `file_id`, a cycle each, until one cycle took a block off the
 * chain's front that is not the head: that block in *gone, the cycle's first
 * block in *first. False — nothing gleaned — after as many cycles as the chain
 * may be long past its slack, twice over.
 */
static bool StandGleanUntilTaken(uint32_t file_id, uint32_t* gone, uint32_t* first)
{
    uint32_t chain, live;
    StandGleanChain(&chain, &live);
    uint32_t bound = 2u * (GLEAN_SLACK * live + 1u) + chain + 8u;
    for (uint32_t round = 0; round < bound; round++) {
        uint32_t was = StandGleanFirst();
        if (!StandGleanOverwrite(file_id, round))
            return false;
        uint32_t now = StandGleanFirst();
        if (now != was && was != StandGleanHead()) {
            *gone  = was;
            *first = now;
            return true;
        }
    }
    return false;
}

/* The file a test writes, made and settled. */
static bool StandGleanFile(const char* name, uint32_t* file_id)
{
    TestFillAt(g_test_buffer, TAGFS_BLOCK_SIZE, 0x61, 0);
    return StandMakeFile(name, g_test_buffer, TAGFS_BLOCK_SIZE, file_id) && TagFSSettle() == OK;
}

static void StandGleanLetGo(uint32_t file_id)
{
    if (file_id) {
        (void)tagfs_delete_file(file_id, INK_UNWARRANTED);
        (void)TagFSSettle();
    }
}

/*
 * The red test of step 1.8. A file written again ten thousand times, each time
 * settled by a cycle that closes the tail it lies in: without Glean the chain
 * grows a block a cycle, ten thousand blocks. With it, after every cycle the
 * chain is at most GLEAN_SLACK times the blocks the live records fill, plus
 * the one the cycle closed.
 */
static TestResult TestStandGleanBoundsTheChain(void)
{
    TestResult  verdict = TEST_PASS;
    uint32_t    file_id = 0, chain = 0, live = 0, most = 0, most_live = 0;
    GleanCounts before, after;

    TEST_ASSERT(StandGleanFile("glean_overwrites", &file_id), "a one-block file is made and settled");
    TEST_ASSERT_OK(TagFSSettle(), "a cycle gleans what the tests before this one left");
    GleanCountsRead(&before);

    for (uint32_t round = 0; round < STAND_GLEAN_OVERWRITES; round++) {
        TEST_ASSERT(StandGleanOverwrite(file_id, round), "the file is written again and settled");
        StandGleanChain(&chain, &live);
        if (chain > most) {
            most      = chain;
            most_live = live;
        }
        TEST_ASSERT(chain <= GLEAN_SLACK * live + 1u,
                    "the chain stays within GLEAN_SLACK times its live blocks, plus one");
    }
    GleanCountsRead(&after);
    TestEvidence("blocks the chain was at its longest:", most);
    TestEvidence("blocks its live records filled then:", most_live);
    TestEvidence("blocks Glean took off the chain:", after.unlinked - before.unlinked);
    TestEvidence("records Glean laid again:", after.moved_records - before.moved_records);
    TestEvidence("bytes Glean laid again:", after.moved_bytes - before.moved_bytes);
    TestEvidence("front blocks Glean read:", after.reads - before.reads);
    TestEvidence("times the head was named again:", after.head_writes - before.head_writes);
out:
    StandGleanLetGo(file_id);
    return verdict;
}

/* The files of the uniform load, and how many times each is written again. */
#define STAND_GLEAN_SPREAD      64u
#define STAND_GLEAN_ROUNDS      50u

static uint32_t s_spread[STAND_GLEAN_SPREAD];

static void StandGleanSpreadLetGo(void)
{
    for (uint32_t i = 0; i < STAND_GLEAN_SPREAD; i++) {
        if (s_spread[i])
            (void)tagfs_delete_file(s_spread[i], INK_UNWARRANTED);
        s_spread[i] = 0;
    }
    (void)TagFSSettle();
}

/*
 * The second load the slack is a policy for: every file as likely to be
 * written again as any other, where a block reaching the front still holds
 * about a fifth of its records live (glean.h). Sixty-four files written in
 * turn, a cycle after each round: the chain stays within its slack, and what
 * Glean moved to keep it there is said beside what was inked.
 */
static TestResult TestStandGleanUniformLoad(void)
{
    TestResult  verdict = TEST_PASS;
    uint32_t    chain = 0, live = 0, most = 0;
    char        name[24];
    GleanCounts before, after;

    memset(s_spread, 0, sizeof(s_spread));
    TestFillAt(g_test_buffer, TAGFS_BLOCK_SIZE, 0x75, 0);
    for (uint32_t i = 0; i < STAND_GLEAN_SPREAD; i++) {
        ksnprintf(name, sizeof(name), "glean_spread_%02u", i);
        TEST_ASSERT(StandMakeFile(name, g_test_buffer, TAGFS_BLOCK_SIZE, &s_spread[i]),
                    "a one-block file of the load is made");
    }
    TEST_ASSERT_OK(TagFSSettle(), "the load's files settle");
    GleanCountsRead(&before);

    for (uint32_t round = 0; round < STAND_GLEAN_ROUNDS; round++) {
        for (uint32_t i = 0; i < STAND_GLEAN_SPREAD; i++) {
            TestFillAt(g_test_buffer, STAND_GLEAN_BYTES, s_spread[i], round);
            TEST_ASSERT_EQ(StandWriteAt(s_spread[i], 0, g_test_buffer, STAND_GLEAN_BYTES),
                           STAND_GLEAN_BYTES, "each file of the load is written again in turn");
        }
        TEST_ASSERT_OK(TagFSSettle(), "and a cycle settles the round");
        StandGleanChain(&chain, &live);
        if (chain > most)
            most = chain;
        TEST_ASSERT(chain <= GLEAN_SLACK * live + 1u,
                    "the chain stays within GLEAN_SLACK times its live blocks, plus one");
    }
    GleanCountsRead(&after);
    TestEvidence("files written again in turn, a cycle each round:", STAND_GLEAN_SPREAD);
    TestEvidence("blocks the chain was at its longest:", most);
    TestEvidence("blocks its live records fill:", live);
    TestEvidence("records Glean laid again:", after.moved_records - before.moved_records);
    TestEvidence("bytes Glean laid again:", after.moved_bytes - before.moved_bytes);
    TestEvidence("front blocks Glean read:", after.reads - before.reads);
out:
    StandGleanSpreadLetGo();
    return verdict;
}

/*
 * RT-08. The head the Deed names is never taken: once the chain begins past it,
 * it is the pool's still — taken in the map, the pool's magic on the medium —
 * and once a cycle after has named it again, a reader that knows no seal walks
 * from it through the seal's first block to the seal's open block, whole: the
 * walk of a kernel before the seal.
 */
static TestResult TestStandGleanHeadStaysLinked(void)
{
    TestResult verdict = TEST_PASS;
    uint32_t   file_id = 0, gone = 0, first = 0, next = 0, chain_len = 0, live = 0;
    uint32_t   head    = StandGleanHead();
    uint32_t   walked[2];
    SettleMark seal, other;
    bool       has_other = false;
    StandChain chain;

    TEST_ASSERT(StandGleanFile("glean_head", &file_id), "a one-block file is made and settled");
    TEST_ASSERT(StandGleanUntilTaken(file_id, &gone, &first),
                "overwrites, a cycle each, until a cycle took a block off the chain");
    TEST_ASSERT(first != head, "the chain begins past the head");
    TEST_ASSERT(StandBlockInUse(head), "and the head is still taken");

    /* With nothing inked, a cycle that takes nothing more names the head
     * again, past the blocks taken off before it. */
    StandGleanChain(&chain_len, &live);
    for (uint32_t more = 0; more <= chain_len; more++) {
        TEST_ASSERT_OK(TagFSSettle(), "a cycle with nothing inked settles");
        TEST_ASSERT(StandGleanSeals(&seal, &other, &has_other), "the newest Ledger carries a seal");
        TEST_ASSERT(StandGleanHeadNext(&next), "the head reads as a block of the pool on the medium");
        if (next == seal.chain_first)
            break;
    }
    TEST_ASSERT_EQ(next, seal.chain_first, "and names the block the seal's chain begins at");

    memset(&chain, 0, sizeof(chain));
    chain.blocks   = walked;
    chain.capacity = ARRAY_SIZE(walked);
    TEST_ASSERT(StandChainOnMedium(tagfs_get_state()->layout.metadata_pool_block,
                                   TAGFS_MPOOL_MAGIC, &chain) && !chain.broken,
                "a walk from the head, as a kernel before the seal walks, reads whole");
    TEST_ASSERT(chain.links >= 2 && walked[0] == head && walked[1] == seal.chain_first,
                "from the head through the seal's first block");
    TEST_ASSERT_EQ(chain.last, seal.pool_to_block, "to the seal's open block");
out:
    StandGleanLetGo(file_id);
    return verdict;
}

/*
 * RT-08. A block taken off the chain rests: the cycle whose seal first began
 * past it leaves it taken — the Ledger's other copy may still stand on the
 * chain it was part of, and the head may still name it — and it goes back only
 * after a cycle more has settled, and the head on the medium no longer names it.
 */
static TestResult TestStandGleanTakenRestsUntilPast(void)
{
    TestResult verdict = TEST_PASS;
    uint32_t   file_id = 0, gone = 0, first = 0, next = 0;

    TEST_ASSERT(StandGleanFile("glean_rests", &file_id), "a one-block file is made and settled");
    TEST_ASSERT(StandGleanUntilTaken(file_id, &gone, &first),
                "overwrites, a cycle each, until a cycle took a block off the chain");
    TEST_ASSERT(StandBlockInUse(gone),
                "the block taken off stays taken after the cycle that settled the new first");

    for (uint64_t more = 0; more < FallowLag() + 1u && StandBlockInUse(gone); more++)
        TEST_ASSERT_OK(TagFSSettle(), "a cycle more");
    TEST_ASSERT(!StandBlockInUse(gone), "and goes back once the cycles after it have settled");
    TEST_ASSERT(StandGleanHeadNext(&next), "the head reads as a block of the pool on the medium");
    TEST_ASSERT(next != gone, "and names no block that went back");
out:
    StandGleanLetGo(file_id);
    return verdict;
}

/*
 * A cycle that took blocks off the front, and a medium that lied about it: the
 * newest seal's chain begins at another block than the other copy's, and its
 * closed blocks do not bear it out — an open block alone not borne out would
 * roll back under the newest seal (meta_pool.h). The mount judges the other
 * copy by a walk from its own first block — the blocks taken off rest, whole —
 * and falls back onto it, where a walk from the newest first block alone would
 * not reach it.
 */
static TestResult TestStandGleanLieFallsBack(void)
{
    TestResult verdict = TEST_PASS;
    uint32_t   file_id = 0, gone = 0, first = 0;
    SettleMark seal, other, lied;
    bool       has_other = false, fell_back = false;
    int        stood_on  = -1;

    TEST_ASSERT(StandGleanFile("glean_lie", &file_id), "a one-block file is made and settled");
    TEST_ASSERT(StandGleanUntilTaken(file_id, &gone, &first),
                "overwrites, a cycle each, until a cycle took a block off the chain");
    TEST_ASSERT(StandGleanSeals(&seal, &other, &has_other) && has_other,
                "both copies of the Ledger carry a seal");
    TEST_ASSERT(seal.chain_first != other.chain_first,
                "and their chains begin at different blocks");

    lied               = seal;
    lied.closed_crc32 ^= 1u;
    PoolMountAsk ask = {
        .seal = { &lied, &other },
        .head = StandGleanHead(),
    };
    bool      marked = false;
    PoolStood stood  = PoolSealsJudge(&ask, &stood_on, &fell_back, &marked);
    TEST_ASSERT_EQ(stood, POOL_STOOD_SEAL, "the mount would stand on a seal");
    TEST_ASSERT_EQ(stood_on, 1, "the other copy's, whose chain begins before the newest one's");
    TEST_ASSERT(fell_back, "and says it fell back");
    TEST_ASSERT(marked, "marking the medium SETTLE_MEDIUM_LIED");
out:
    StandGleanLetGo(file_id);
    return verdict;
}

// ============================================================================
// The suite
// ============================================================================

static const TestCase g_stand_glean_tests[] = {
    { .name = "stand_glean_bounds_the_chain",       .func = TestStandGleanBoundsTheChain,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a file written again and settled each time leaves a pool block a cycle, "
                       "and the chain the mount walks grows without bound" },
    { .name = "stand_glean_uniform_load",           .func = TestStandGleanUniformLoad,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_glean_head_stays_linked",      .func = TestStandGleanHeadStaysLinked,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_glean_taken_rests_until_past", .func = TestStandGleanTakenRestsUntilPast,
      .reach = TEST_WRITES_TO_VOLUME },
    { .name = "stand_glean_lie_falls_back",         .func = TestStandGleanLieFallsBack,
      .reach = TEST_WRITES_TO_VOLUME },
};

const TestSuite g_stand_glean_suite = {
    .tests = g_stand_glean_tests, .count = ARRAY_SIZE(g_stand_glean_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
