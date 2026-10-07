#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../metadata_pool/meta_pool.h"
#include "../file_map/file_map.h"
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
 * A block taken off the chain rests, taken in memory, and the cycle that writes
 * the head past it settles a volume on which nothing names it: the map that
 * cycle sent called it taken, and every seal stood behind ground nothing owned
 * until the block came back a cycle later. After each cycle here, a block the
 * medium's walk from the head still reaches may be taken; the first cycle
 * after which the walk no longer reaches it sent a map calling it free.
 */
#define STAND_GLEAN_WALK 64u

static TestResult TestStandGleanUnnamedLeftFree(void)
{
    TestResult verdict = TEST_PASS;
    uint32_t   file_id = 0, gone = 0, first = 0, chain_len = 0, live = 0;
    uint32_t   walked[STAND_GLEAN_WALK];
    bool       unnamed = false, taken = true;
    StandChain chain;

    TEST_ASSERT(StandGleanFile("glean_unnamed", &file_id), "a one-block file is made and settled");
    TEST_ASSERT(StandGleanUntilTaken(file_id, &gone, &first),
                "overwrites, a cycle each, until a cycle took a block off the chain");

    StandGleanChain(&chain_len, &live);
    for (uint32_t more = 0; more <= chain_len + FallowLag() + 1u && !unnamed; more++) {
        TEST_ASSERT_OK(TagFSSettle(), "a cycle with nothing inked settles");
        memset(&chain, 0, sizeof(chain));
        chain.blocks   = walked;
        chain.capacity = ARRAY_SIZE(walked);
        TEST_ASSERT(StandChainOnMedium(tagfs_get_state()->layout.metadata_pool_block,
                                       TAGFS_MPOOL_MAGIC, &chain) && !chain.broken &&
                    chain.links <= chain.capacity,
                    "the pool is walked whole from the head on the medium");
        unnamed = true;
        for (uint32_t i = 0; i < chain.links; i++)
            unnamed = unnamed && walked[i] != gone;
    }
    TEST_ASSERT(unnamed, "a cycle wrote the head past the block taken off");
    TEST_ASSERT(BlockMapMediumSays(gone, &taken), "the map's sector is read off the medium");
    TEST_ASSERT(!taken, "and the map that cycle sent calls the block free: nothing names it");
out:
    StandGleanLetGo(file_id);
    return verdict;
}

/*
 * Z13-3. A take short of room with nothing resting lowers Glean's slack, and a
 * cycle with a waiter closes the tail Glean has just laid records in. Where
 * the live records pack no tighter than they lie — a block's worth of small
 * ones and more — the chain comes out of each such cycle as long as it went
 * in, a block of slack is born again, and the lowered slack was chased for
 * ever: the stand never came to rest, and a take short of room heard
 * ERR_WOULD_BLOCK where the truth was that the volume had none. Here such
 * cycles run over that many records, each told the allocator is short while
 * Glean says it may free: within a few, nothing rests in Fallow and Glean
 * says no slack is its to give. Then every one of those records is replaced
 * by a newer one — a block's worth of records dies, which a pass can take
 * off — and the slack is Glean's to try again, and comes to rest again.
 */
#define STAND_GLEAN_FRUITLESS_ROUNDS 16u
#define STAND_GLEAN_DENSE            (POOL_QUITCLAIMS_PER_BLOCK + 2u)

static uint32_t s_dense[STAND_GLEAN_DENSE];

/* Waiter's cycles, each telling the allocator is short while Glean says it may
 * free, until nothing rests and Glean says no slack is its: whether they came
 * to that, and in *rounds after how many; *slack whether Glean said it may. */
static bool StandGleanToRest(uint32_t* rounds, bool* slack)
{
    TagFSState* fs = tagfs_get_state();

    *slack = false;
    for (*rounds = 0; *rounds < STAND_GLEAN_FRUITLESS_ROUNDS; ) {
        if (TagFSSettle() != OK)
            return false;
        (*rounds)++;
        TagFSLockTake(&fs->lock);
        bool may  = GleanMayFree();
        bool rest = FallowBlocks() == 0 && !may;
        TagFSLockDrop(&fs->lock);
        if (rest)
            return true;
        if (may) {
            *slack = true;
            GleanShort();
        }
    }
    return false;
}

static TestResult TestStandGleanFruitlessSlackLifted(void)
{
    TestResult  verdict = TEST_PASS;
    TagFSState* fs      = tagfs_get_state();
    uint32_t    rounds  = 0;
    bool        slack   = false, rest = false, again = false;

    memset(s_dense, 0, sizeof(s_dense));
    for (uint32_t i = 0; i < STAND_GLEAN_DENSE; i++)
        TEST_ASSERT_EQ(tagfs_create_file("test_file", NULL, 0, &s_dense[i]), 0,
                       "files are made, a small record each, more than a block holds");

    rest = StandGleanToRest(&rounds, &slack);
    TestEvidence("waiter's cycles until nothing rested and Glean said no slack was its", rounds);
    TEST_ASSERT(slack, "the chain had slack Glean said it may free");
    TEST_ASSERT(rest, "and within a few cycles nothing rests and none is Glean's to give");

    for (uint32_t i = 0; i < STAND_GLEAN_DENSE; i++)
        TEST_ASSERT_EQ(tagfs_rename_file(s_dense[i], "test_file.r", INK_UNWARRANTED), 0,
                       "each file is renamed: a newer record replaces its own");
    TagFSLockTake(&fs->lock);
    again = GleanMayFree();
    TagFSLockDrop(&fs->lock);
    TEST_ASSERT(again, "the records that died make the slack Glean's to try again");

    rest = StandGleanToRest(&rounds, &slack);
    TestEvidence("waiter's cycles until it came to rest again", rounds);
    TEST_ASSERT(rest, "and it comes to rest again within a few cycles");
out:
    for (uint32_t i = 0; i < STAND_GLEAN_DENSE; i++)
        if (s_dense[i])
            (void)tagfs_delete_file(s_dense[i], INK_UNWARRANTED);
    (void)TagFSSettle();
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

/* Where a file's record lay before a cycle. */
typedef struct {
    uint32_t fid;
    uint32_t link;
} StandGleanPlace;

/* Every file the map holds a record of, and the link it lies in: into `out`,
 * room for `room`; how many. */
static uint32_t StandGleanPlaces(StandGleanPlace* out, uint32_t room)
{
    FileMap* files = tagfs_get_state()->files;
    uint32_t batch[32], from = 1, got, n = 0;
    while (n < room && (got = FileMapFiles(files, from, batch, ARRAY_SIZE(batch))) > 0) {
        for (uint32_t i = 0; i < got && n < room; i++) {
            RecordWhere where;
            FileRecord* rec = FileMapRecordHold(files, batch[i], &where);
            if (!rec)
                continue;
            FileRecordRelease(rec);
            out[n].fid    = batch[i];
            out[n].link   = where.link;
            n++;
        }
        if (batch[got - 1] == 0xFFFFFFFFu)
            break;
        from = batch[got - 1] + 1;
    }
    return n;
}

/* The block a file's record lies in now: false when the map holds none, or its
 * link is off the chain. */
static bool StandGleanRecordBlock(uint32_t fid, uint32_t* link, uint32_t* block)
{
    TagFSState* fs = tagfs_get_state();
    RecordWhere where;
    FileRecord* rec = FileMapRecordHold(fs->files, fid, &where);
    if (!rec)
        return false;
    FileRecordRelease(rec);
    *link = where.link;
    TagFSLockTake(&fs->lock);
    bool on = PoolLinkBlock(where.link, block);
    TagFSLockDrop(&fs->lock);
    return on;
}

/*
 * R12 (Z09-5 = Z13-1; PowerCut race d0). A cycle the cadence begins — nobody
 * waits on it, so it closes the tail for no anchor — in which Glean moves the
 * records of the chain's front into the tail and lets the front go. Every
 * record it moved lies in a closed block the cycle's seal counts, not in the
 * open tail the next cycles write again in place: a page of that tail torn by
 * the power — emulated as every seal whose open block it is finding it not as
 * it says — rolls back none of them, and the files nobody touched stay. Red
 * before: the moved records sat in the open tail with their source off the
 * chain, and the roll back took files written once, years before.
 */
static TestResult TestStandGleanMovedLiesClosed(void)
{
    TestResult       verdict  = TEST_PASS;
    uint32_t         file_id  = 0, chain = 0, live = 0, count = 0, moved = 0, rolled = 0;
    uint32_t         room     = 0;
    StandGleanPlace* places   = NULL;
    bool             gleaned  = false, has_other = false, fell_back = false, marked = false;
    int              stood_on = -1;
    SettleMark       seal, other;
    GleanCounts      before, after;

    TEST_ASSERT(StandGleanFile("glean_closed", &file_id), "a one-block file is made and settled");
    room   = FileMapCount(tagfs_get_state()->files) + 16u;
    places = kmalloc(sizeof(StandGleanPlace) * room);
    TEST_ASSERT(places, "memory for where every file's record lies");

    StandGleanChain(&chain, &live);
    uint32_t bound = 2u * (GLEAN_SLACK * live + 1u) + chain + 8u;
    for (uint32_t round = 0; round < bound && !gleaned; round++) {
        StandGleanChain(&chain, &live);
        TestFillAt(g_test_buffer, STAND_GLEAN_BYTES, file_id, round);
        TEST_ASSERT_EQ(StandWriteAt(file_id, 0, g_test_buffer, STAND_GLEAN_BYTES),
                       (int)STAND_GLEAN_BYTES, "the file is written again");
        if (chain < GLEAN_SLACK * live + 1u) {
            TEST_ASSERT_OK(TagFSSettle(), "a waiter's cycle closes the tail, and the chain grows");
            continue;
        }
        count = StandGleanPlaces(places, room);
        GleanCountsRead(&before);
        TEST_ASSERT_OK(TagFSSettleAsCadence(), "a cycle the cadence begins settles");
        GleanCountsRead(&after);
        gleaned = after.moved_records > before.moved_records;
    }
    TEST_ASSERT(gleaned, "a cycle the cadence began moved records of the chain's front");
    TEST_ASSERT(StandGleanSeals(&seal, &other, &has_other) && has_other,
                "both copies of the Ledger carry a seal");

    for (uint32_t i = 0; i < count; i++) {
        uint32_t link = 0, block = 0;
        if (places[i].fid == file_id)
            continue;
        TEST_ASSERT(StandGleanRecordBlock(places[i].fid, &link, &block),
                    "every file's record lies on the chain");
        if (link == places[i].link)
            continue;
        moved++;
        TEST_ASSERT(block != seal.pool_to_block,
                    "a record Glean moved lies in a closed block of the seal, not its open one");
    }
    TestEvidence("records the cadence's cycle moved:", after.moved_records - before.moved_records);
    TEST_ASSERT_EQ(moved, after.moved_records - before.moved_records,
                   "and they are the records of the files whose record moved");

    /* A page of the open block torn: each seal whose open block it is finds it
     * not as it says. */
    SettleMark torn = seal, torn_other = other;
    torn.tail_crc32 ^= 1u;
    if (other.pool_to_block == seal.pool_to_block)
        torn_other.tail_crc32 ^= 1u;
    PoolMountAsk ask = {
        .seal = { &torn, &torn_other },
        .head = StandGleanHead(),
    };
    PoolStood stood = PoolSealsJudge(&ask, &stood_on, &fell_back, &marked);
    TEST_ASSERT(stood == POOL_STOOD_TORN_TAIL && stood_on >= 0,
                "the mount rolls the torn open block back under a seal");
    rolled = (stood_on == 0 ? torn : torn_other).pool_to_block;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t link = 0, block = 0;
        if (places[i].fid == file_id ||
            !StandGleanRecordBlock(places[i].fid, &link, &block) || link == places[i].link)
            continue;
        TEST_ASSERT(block != rolled, "and no record Glean moved is in the block rolled back");
    }
out:
    kfree(places);
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
    { .name = "stand_glean_moved_lies_closed",      .func = TestStandGleanMovedLiesClosed,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a cycle the cadence began moved records of untouched files into the open "
                       "tail and let their block go, and a torn page of the tail loses the files" },
    { .name = "stand_glean_unnamed_left_free",      .func = TestStandGleanUnnamedLeftFree,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "the cycle that writes the head past a block taken off the chain sends a "
                       "map calling it taken, and its seal stands behind ground nothing owns" },
    { .name = "stand_glean_fruitless_slack_lifted", .func = TestStandGleanFruitlessSlackLifted,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a lowered slack that gives nothing back is chased for ever, and a take "
                       "short of room waits on cycles that never bring it; or, lifted, it is "
                       "not tried again once records die" },
};

const TestSuite g_stand_glean_suite = {
    .tests = g_stand_glean_tests, .count = ARRAY_SIZE(g_stand_glean_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
