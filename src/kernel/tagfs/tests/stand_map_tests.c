#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "../helm/helm.h"
#include "../ink/ink.h"
#include "../ink/fallow.h"
#include "../plot/plot.h"
#include "../settle/settle.h"
#include "../glean/glean.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The block map — where a take goes, a give that comes twice, the ground a
// growing file keeps in hand, a write's ground the medium hears of only with
// its record, gives waiting for their sectors, and Fallow's gives the map has
// no room to write down
//
// Each test takes its own ground through the allocator and gives back exactly
// what it took: the runner's residue guard counts the volume's free blocks
// before and after, and the next boot's roll call judges the map.
// ============================================================================

#define STAND_MAP_REGION 256u      /* blocks the take-run test lays its runs out in */
#define STAND_MAP_FID    0xFFFFFFF0u   /* a file id no file of the stand has */
#define STAND_MAP_KEPT   64u       /* blocks the Allotment test leaves free */

static uint8_t s_owned[STAND_MAP_REGION / 8];   /* the region's blocks the test holds */

static void StandMapOwn(uint32_t first, uint32_t count, bool owned)
{
    for (uint32_t b = first; b < first + count; b++) {
        if (owned)
            s_owned[b / 8] |= (uint8_t)(1u << (b % 8));
        else
            s_owned[b / 8] &= (uint8_t)~(1u << (b % 8));
    }
}

/* A take the test made: its blocks marked held when they lie in the region,
 * and otherwise kept in `far` to be given back — a take the test did not
 * expect outside the region is still the test's to return. */
typedef struct {
    uint32_t at[4];
    uint32_t n[4];
    uint32_t count;
} StandMapFar;

static void StandMapTook(uint32_t region, uint32_t start, uint32_t got, StandMapFar* far)
{
    if (start >= region && start + got <= region + STAND_MAP_REGION) {
        StandMapOwn(start - region, got, true);
    } else if (far->count < 4) {
        far->at[far->count] = start;
        far->n[far->count]  = got;
        far->count++;
    }
}

/* Every run of the region the test still holds, given back. */
static void StandMapGiveOwned(uint32_t region)
{
    for (uint32_t b = 0; b < STAND_MAP_REGION; ) {
        if (!(s_owned[b / 8] & (1u << (b % 8)))) {
            b++;
            continue;
        }
        uint32_t a = b;
        while (b < STAND_MAP_REGION && (s_owned[b / 8] & (1u << (b % 8))))
            b++;
        tagfs_free_blocks(region + a, b - a);
    }
    memset(s_owned, 0, sizeof(s_owned));
}

/*
 * TagFSTakeRun gives the longest run it can up to `want`, as near the hint as
 * a run that long lies. Three runs are opened in a region the test holds — 3
 * blocks at +100, 10 at +120, 2 at +140 — and asked in turn:
 *   2 at the free +140: the take starts at the hint;
 *   8 near +131: the run at +120 is the nearest that holds 8, and the take is
 *     placed in it as near the hint as it fits, at +122;
 *   3 near +121: the two blocks left at +120 are nearer, and the run at +100
 *     gives three — length first, so +100;
 *   12 near +121: nothing in the region holds 12, and the volume does, so all
 *     twelve are given, from outside the region.
 */
static TestResult TestStandMapTakeRunNearestLongest(void)
{
    TestResult  verdict = TEST_PASS;
    uint32_t    region  = 0, start = 0, got = 0;
    StandMapFar far     = { .count = 0 };
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT_EQ(tagfs_alloc_blocks(STAND_MAP_REGION, &region), 0,
                   "a region of 256 blocks is taken whole");
    StandMapOwn(0, STAND_MAP_REGION, true);
    TEST_ASSERT_EQ(tagfs_free_blocks(region + 100, 3), 0, "three blocks at +100 are given back");
    StandMapOwn(100, 3, false);
    TEST_ASSERT_EQ(tagfs_free_blocks(region + 120, 10), 0, "ten at +120");
    StandMapOwn(120, 10, false);
    TEST_ASSERT_EQ(tagfs_free_blocks(region + 140, 2), 0, "two at +140");
    StandMapOwn(140, 2, false);

    TEST_ASSERT_OK(TagFSTakeRun(2, region + 140, &start, &got), "two blocks at a free hint");
    StandMapTook(region, start, got, &far);
    TEST_ASSERT_EQ(start, region + 140, "start at the hint");
    TEST_ASSERT_EQ(got, 2, "and are both given");

    TEST_ASSERT_OK(TagFSTakeRun(8, region + 131, &start, &got), "eight blocks near +131");
    StandMapTook(region, start, got, &far);
    TEST_ASSERT_EQ(got, 8, "are all given");
    TEST_ASSERT_EQ(start, region + 122, "from the run at +120, as near the hint as eight fit");

    TEST_ASSERT_OK(TagFSTakeRun(3, region + 121, &start, &got), "three blocks near +121");
    StandMapTook(region, start, got, &far);
    TEST_ASSERT_EQ(got, 3, "are all given");
    TEST_ASSERT_EQ(start, region + 100, "from the run of three, not the nearer run of two");

    TEST_ASSERT_OK(TagFSTakeRun(12, region + 121, &start, &got), "twelve blocks near +121");
    StandMapTook(region, start, got, &far);
    TEST_ASSERT_EQ(got, 12, "are all given, since the volume holds a run of twelve");
    TEST_ASSERT(start + got <= region || start >= region + STAND_MAP_REGION,
                "from outside the region, whose two blocks left are not enough");
out:
    for (uint32_t i = 0; i < far.count; i++)
        tagfs_free_blocks(far.at[i], far.n[i]);
    if (region)
        StandMapGiveOwned(region);
    return verdict;
}

/*
 * CW-D3. A run given back while a block of it is already free is refused
 * whole — through the door and under the state lock alike — and the map, the
 * free count and their agreement are left exactly as they were.
 */
static TestResult TestStandMapGiveBackTwiceRefused(void)
{
    TestResult  verdict = TEST_PASS;
    TagFSState* fs      = tagfs_get_state();
    uint32_t    run     = 0;
    bool        held    = false;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT_EQ(tagfs_alloc_blocks(4, &run), 0, "four blocks are taken");
    held = true;
    TEST_ASSERT_EQ(tagfs_free_blocks(run + 1, 2), 0, "the middle two are given back");
    TEST_ASSERT_EQ(TagFSAllocatorRederive(), 0, "the free count agrees with the map");

    TagFSLockTake(&fs->lock);
    uint64_t free_before = fs->ledger.free_blocks;
    TagFSLockDrop(&fs->lock);

    TEST_ASSERT_EQ(tagfs_free_blocks(run, 4), -(int)ERR_TAGFS_EXTENT_ERROR,
                   "all four given back again are refused whole");
    TEST_ASSERT(StandBlockInUse(run) && StandBlockInUse(run + 3),
                "the two still taken stay taken");
    TEST_ASSERT(!StandBlockInUse(run + 1) && !StandBlockInUse(run + 2),
                "the two already free stay free");

    TagFSLockTake(&fs->lock);
    error_t locked = BlockMapGiveLocked(run + 1, 1);
    uint64_t free_after = fs->ledger.free_blocks;
    TagFSLockDrop(&fs->lock);
    TEST_ASSERT_EQ(locked, ERR_TAGFS_EXTENT_ERROR,
                   "a free block given back under the state lock is refused as well");
    TEST_ASSERT_EQ(free_after, free_before, "the free count did not move");
    TEST_ASSERT_EQ(TagFSAllocatorRederive(), 0, "and still agrees with the map");
out:
    if (held) {
        tagfs_free_blocks(run, 1);
        tagfs_free_blocks(run + 3, 1);
    }
    return verdict;
}

/* Every run of the volume taken, longest first, into a growing list. */
typedef struct {
    uint32_t* at;
    uint32_t* n;
    uint32_t  count;
    uint32_t  cap;
} StandMapRuns;

static bool StandMapTakeEverything(StandMapRuns* runs)
{
    uint32_t start = 0, got = 0;
    uint32_t all   = tagfs_get_state()->layout.data_blocks;

    while (TagFSTakeRun(all, 0, &start, &got) == OK) {
        if (runs->count == runs->cap) {
            uint32_t  cap = runs->cap ? runs->cap * 2 : 64;
            uint32_t* at  = kmalloc(sizeof(uint32_t) * cap);
            uint32_t* n   = kmalloc(sizeof(uint32_t) * cap);
            if (!at || !n) {
                kfree(at);
                kfree(n);
                tagfs_free_blocks(start, got);
                return false;
            }
            if (runs->count) {
                memcpy(at, runs->at, sizeof(uint32_t) * runs->count);
                memcpy(n, runs->n, sizeof(uint32_t) * runs->count);
            }
            kfree(runs->at);
            kfree(runs->n);
            runs->at  = at;
            runs->n   = n;
            runs->cap = cap;
        }
        runs->at[runs->count] = start;
        runs->n[runs->count]  = got;
        runs->count++;
    }
    return true;
}

/*
 * RT-16. The ground a growing file keeps in hand is taken back before anybody
 * is told there is no room. Every run of the volume is taken, and the first 64
 * blocks of the longest given back; a file grows by one block into them and
 * keeps the other 63 in hand, so no block is left that anybody else may have.
 * A take by anybody else must then be answered from the kept ground, and the
 * file's window must be gone.
 */
static TestResult TestStandMapAllotmentTakenBack(void)
{
    TestResult   verdict = TEST_PASS;
    StandMapRuns runs    = { 0 };
    uint32_t     own_at  = 0, own_n = 0, kept_at = 0, kept_n = 0;
    uint32_t     other   = 0, other_n = 0;
    uint32_t     freed   = 0;
    const char*  aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT(StandMapTakeEverything(&runs), "every run of the volume is taken");
    TEST_ASSERT(runs.count > 0 && runs.n[0] >= 2, "and the longest holds two blocks at least");

    uint32_t front = runs.n[0] < STAND_MAP_KEPT ? runs.n[0] : STAND_MAP_KEPT;
    TEST_ASSERT_EQ(tagfs_free_blocks(runs.at[0], front), 0,
                   "the front of the longest run is the only free ground");
    freed = front;

    TEST_ASSERT_OK(AllotmentTake(STAND_MAP_FID, 1, freed, runs.at[0], &own_at, &own_n),
                   "a file of that many blocks grows by one");
    TEST_ASSERT_EQ(own_n, 1, "by the one block it asked for");
    TEST_ASSERT(AllotmentHeld(STAND_MAP_FID, &kept_at, &kept_n) && kept_n == freed - 1,
                "and keeps the rest of the ground in hand");

    TEST_ASSERT_OK(TagFSTakeRun(1, 0, &other, &other_n),
                   "a take by anybody else is answered, not refused");
    TEST_ASSERT(other >= kept_at && other < kept_at + kept_n,
                "from the ground the file had kept");
    TEST_ASSERT(!AllotmentHeld(STAND_MAP_FID, &kept_at, &kept_n),
                "whose window was taken back first");
out:
    TagFSLockTake(&tagfs_get_state()->lock);
    AllotmentRelease(STAND_MAP_FID);
    TagFSLockDrop(&tagfs_get_state()->lock);
    if (other_n)
        tagfs_free_blocks(other, other_n);
    if (own_n)
        tagfs_free_blocks(own_at, own_n);
    for (uint32_t i = 0; i < runs.count; i++) {
        uint32_t skip = i == 0 ? freed : 0;
        if (runs.n[i] > skip)
            tagfs_free_blocks(runs.at[i] + skip, runs.n[i] - skip);
    }
    kfree(runs.at);
    kfree(runs.n);
    return verdict;
}

// ----------------------------------------------------------------------------
// A write's ground reaches the medium with its record
//
// A cycle writes the map at S1 as memory has it, and memory holds a write's
// new block taken from the moment it is taken — while the record that names
// it is inked after that cycle's snapshot, or never: the cycle's seal stood
// behind a map with ground nothing owned. Here a file grows by a block through
// its Plot, under its Helm, and a cycle settles in the middle of the write:
// the sector of the map that cycle sent calls the block free. The write is
// inked, and the cycle whose snapshot holds the Ink calls it taken.
// ----------------------------------------------------------------------------

static HelmWaiter s_map_helm;

static TestResult TestStandMapWriteInFlightLeftFree(void)
{
    TestResult       verdict = TEST_PASS;
    uint32_t         file_id = 0;
    TagFSFileHandle *handle  = NULL;
    bool             holding = false;
    bool             loaded  = false;
    bool             taken   = false;
    uint32_t         block   = 0;
    uint64_t         at      = 0;
    Plot             plot;
    const uint32_t   B       = TAGFS_BLOCK_SIZE;
    const char      *aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TestFillAt(g_test_buffer, 2 * B, 0x4D415031u, 0);
    TEST_ASSERT(StandMakeFile("test_file", g_test_buffer, B, &file_id), "a one-block file is made");
    TEST_ASSERT_OK(TagFSSettle(), "and settled");

    handle = tagfs_open(file_id, TAGFS_HANDLE_WRITE | TAGFS_HANDLE_APPEND);
    TEST_ASSERT(handle, "the writer's handle opens");
    memset(&s_map_helm, 0, sizeof(s_map_helm));
    TEST_ASSERT(HelmAsk(handle->ofe, &s_map_helm), "and it holds the file's Helm");
    holding = true;
    TEST_ASSERT_OK(PlotLoad(&plot, file_id, INK_UNWARRANTED), "the write loads the record");
    loaded    = true;
    plot.helm = &handle->ofe->helm;

    TEST_ASSERT_OK(PlotBeginWrite(&plot, plot.size, false, B, &at), "and takes a block to grow by");
    TEST_ASSERT(plot.stake_count > 0, "a Stake of the write's");
    block = plot.stakes[plot.stake_count - 1].start;
    TEST_ASSERT(StandBlockInUse(block), "memory holds its block taken");

    TEST_ASSERT_OK(TagFSSettle(), "a cycle settles in the middle of the write");
    TEST_ASSERT(BlockMapMediumSays(block, &taken), "the map's sector is read off the medium");
    TEST_ASSERT(!taken, "and calls the block free: no settled record names it");

    PlotSource src  = { .kernel = g_test_buffer + B, .bytes = B };
    uint64_t   done = 0;
    InkReceipt inked;
    TEST_ASSERT_OK(PlotWriteBlocks(&plot, at, &src, &done), "the write's block lands");
    TEST_ASSERT_OK(PlotEndWrite(&plot, at, done), "and is made ready to be the file's");
    TEST_ASSERT_OK(TagFSInkPlot(&plot, INK_MAKES_SPARES, &inked), "the write is inked");
    PlotRelease(&plot);
    loaded = false;
    HelmLetGo(handle->ofe, &s_map_helm);
    holding = false;

    TEST_ASSERT_OK(TagFSSettle(), "the cycle whose snapshot holds the Ink settles");
    TEST_ASSERT(BlockMapMediumSays(block, &taken), "the map's sector is read off the medium again");
    TEST_ASSERT(taken, "and calls the block taken: the record that names it is settled");
out:
    if (loaded)
        PlotRelease(&plot);
    if (holding)
        HelmLetGo(handle->ofe, &s_map_helm);
    tagfs_close(handle);
    if (file_id)
        tagfs_delete_file(file_id, INK_UNWARRANTED);
    return verdict;
}

// ----------------------------------------------------------------------------
// Gives that wait for their sectors are room on its way
//
// A give whose sector is not in memory is written down as a delta: the block
// is free, and no count holds it until the sector comes in — the way Fallow
// gives back under the state lock at a cycle's end. A take under the lock that
// found nothing else was told the volume had no room at all. It is told the
// room is on its way (ERR_WOULD_BLOCK) — after the volume was brought to rest,
// so neither Fallow nor Glean answers for it — and the asker for room
// (SettleForRoom) has the cycle run that lays the give on: the block is free
// on the medium, and memory's again to hand out. An asker that judged room by
// Fallow and Glean alone answered at once with no cycle, and the take asked
// again was short again — a caller going round for ever.
// ----------------------------------------------------------------------------

#define STAND_MAP_REST_ROUNDS 16u

/* Cycles with a waiter — each closes the tail, so a delete's ground the tests
 * before left rests from a closed block, and ripens — until nothing rests in
 * Fallow and the pool's chain has no slack Glean could take off, the pass told
 * the allocator is short as a take that met no room would tell it. True at
 * rest. */
static bool StandMapAtRest(void)
{
    TagFSState* fs = tagfs_get_state();

    for (uint32_t round = 0; round < STAND_MAP_REST_ROUNDS; round++) {
        if (TagFSSettle() != OK)
            return false;
        TagFSLockTake(&fs->lock);
        bool slack = GleanMayFree();
        bool rest  = FallowBlocks() == 0 && !slack;
        TagFSLockDrop(&fs->lock);
        if (rest)
            return true;
        if (slack)
            GleanShort();
    }
    return false;
}

static TestResult TestStandMapGivesAwayAreRoomOnItsWay(void)
{
    TestResult   verdict = TEST_PASS;
    TagFSState*  fs      = tagfs_get_state();
    StandMapRuns runs    = { 0 };
    uint32_t     block   = 0;
    uint32_t     other   = 0;
    bool         given   = false;
    bool         took    = false;
    bool         taken   = true;
    const char*  aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT(StandMapAtRest(), "cycles bring the volume to rest: nothing in Fallow, no slack to glean");
    TEST_ASSERT(StandMapTakeEverything(&runs) && runs.count > 0, "every run of the volume is taken");

    block = runs.at[runs.count - 1] + runs.n[runs.count - 1] - 1;
    BlockMapSectorAway(block);

    TagFSLockTake(&fs->lock);
    bool    rest  = FallowBlocks() == 0 && !GleanMayFree();
    error_t gave  = BlockMapGiveLocked(block, 1);
    error_t short_by = gave == OK ? BlockMapTakeLocked(1, 0, &other) : gave;
    TagFSLockDrop(&fs->lock);
    given = gave == OK;
    took  = short_by == OK;
    TEST_ASSERT(rest, "still at rest, with every block taken");
    TEST_ASSERT_OK(gave, "the last block taken is given back with its sector away");
    TEST_ASSERT_EQ(short_by, ERR_WOULD_BLOCK,
                   "a take under the lock that finds nothing else hears the room is on its way");

    TEST_ASSERT_OK(SettleForRoom(), "the asker for room is answered once a cycle laid the give on");
    TagFSLockTake(&fs->lock);
    short_by = BlockMapTakeLocked(1, 0, &other);
    TagFSLockDrop(&fs->lock);
    took = short_by == OK;
    TEST_ASSERT_OK(short_by, "and the take asked again has the block");
    TEST_ASSERT(BlockMapMediumSays(block, &taken), "the map's sector is read off the medium");
    TEST_ASSERT(!taken, "and calls the block free");
out:
    if (took)
        tagfs_free_blocks(other, 1);
    for (uint32_t i = 0; i < runs.count; i++) {
        uint32_t n = runs.n[i] - (given && i == runs.count - 1 ? 1u : 0u);
        if (n)
            tagfs_free_blocks(runs.at[i], n);
    }
    kfree(runs.at);
    kfree(runs.n);
    return verdict;
}

// ----------------------------------------------------------------------------
// Fallow keeps what the map has no room to write down
//
// Fallow gives its ripe ground back under the state lock at a cycle's end,
// and a give into a sector not in memory is a delta the map must have room
// for. The room was asked of the heap there, under the lock; refused, the
// node was let go of all the same and its blocks stayed taken for good. The
// room is made before the lock now (FallowZeroDigests), and a run it does not
// cover rests on. Here a node of one-block runs ripens with its sector away
// while the heap refuses that room: what the room did not cover still rests,
// and the next cycle gives back every block of it.
// ----------------------------------------------------------------------------

#define STAND_MAP_ROOM_RUNS 1024u    /* deltas past any the stand has made room for */

static TestResult TestStandMapFallowKeepsWhatTheMapCannotWrite(void)
{
    TestResult  verdict = TEST_PASS;
    TagFSState* fs      = tagfs_get_state();
    FileExtent* runs    = NULL;
    FallowNode* node    = NULL;
    uint32_t    start   = 0, got = 0, still = 0;
    bool        held    = false;
    bool        resting = false;
    bool        fired   = false;
    const char* aside   = StandStepsAsideFor();

    if (aside)
        return test_steps_aside(aside);

    TEST_ASSERT_OK(TagFSTakeRun(STAND_MAP_ROOM_RUNS, fs->layout.data_blocks / 2u, &start, &got),
                   "a run is taken near the middle of the volume");
    held = true;
    TEST_ASSERT_EQ(got, STAND_MAP_ROOM_RUNS, "as long as asked");
    TEST_ASSERT_OK(TagFSSettleAsCadence(), "a cycle writes the take");

    runs = kmalloc(sizeof(FileExtent) * STAND_MAP_ROOM_RUNS);
    TEST_ASSERT(runs, "room for the node's runs");
    for (uint32_t i = 0; i < STAND_MAP_ROOM_RUNS; i++) {
        runs[i].start_block = start + i;
        runs[i].block_count = 1;
    }
    node = FallowNodeMake(runs, STAND_MAP_ROOM_RUNS, FALLOW_BY_LEDGER, 0);
    TEST_ASSERT(node, "a node of one-block runs");

    BlockMapSectorAway(start);
    BlockMapGiveRoomRefused(true);
    /* Freed by the cycle Fallow's lag before the next one: ripe at the next —
     * one cycle after the last on a medium that keeps its flush, two on one
     * marked as having lied (SETTLE_MEDIUM_LIED, which any roll back leaves). */
    TagFSLockTake(&fs->lock);
    FallowAdd(node, SettledCycles() + 1u - FallowLag());
    TagFSLockDrop(&fs->lock);
    node    = NULL;
    held    = false;
    resting = true;

    TEST_ASSERT_OK(TagFSSettleAsCadence(), "the cycle it ripens in settles");
    fired = HeapFailFired();
    HeapFailDisarm();
    BlockMapGiveRoomRefused(false);
    TEST_ASSERT(fired, "and the room for its gives was refused by the heap");

    for (uint32_t i = 0; i < STAND_MAP_ROOM_RUNS; i++)
        still += StandBlockInUse(start + i);
    TEST_ASSERT(still > 0, "what the room did not cover is still taken, resting");

    for (uint32_t round = 0; round < STAND_MAP_REST_ROUNDS && still; round++) {
        TEST_ASSERT_OK(TagFSSettleAsCadence(), "a cycle more settles");
        still = 0;
        for (uint32_t i = 0; i < STAND_MAP_ROOM_RUNS; i++)
            still += StandBlockInUse(start + i);
    }
    TEST_ASSERT_EQ(still, 0, "and every block of the node comes back: none was let go of");
    resting = false;
out:
    HeapFailDisarm();
    BlockMapGiveRoomRefused(false);
    FallowNodeLetGo(node);
    kfree(runs);
    if (held)
        tagfs_free_blocks(start, got);
    if (resting && still)
        kprintf("[TESTS] %u block(s) of the test's node are left taken: they are the defect's\n",
                still);
    return verdict;
}

static const TestCase g_stand_map_tests[] = {
    { .name  = "stand_map_take_run_nearest_longest", .func = TestStandMapTakeRunNearestLongest,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a take near a hint is placed in the first run that fits, however far "
                       "from the hint, or given less than a run the volume holds" },
    { .name  = "stand_map_give_back_twice_refused", .func = TestStandMapGiveBackTwiceRefused,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a block given back while it is already free is counted free again and "
                       "handed to a second owner" },
    { .name  = "stand_map_allotment_taken_back", .func = TestStandMapAllotmentTakenBack,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a growing file's kept ground is not taken back before a take is refused "
                       "for want of room" },
    { .name  = "stand_map_write_in_flight_left_free", .func = TestStandMapWriteInFlightLeftFree,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a cycle settled under a write in flight sends the medium a map with the "
                       "write's block taken, and its seal stands behind ground nothing owns" },
    { .name  = "stand_map_gives_away_are_room_on_its_way",
      .func  = TestStandMapGivesAwayAreRoomOnItsWay,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "a take under the state lock is told the volume has no room while gives "
                       "wait for their sectors" },
    { .name  = "stand_map_fallow_keeps_what_the_map_cannot_write",
      .func  = TestStandMapFallowKeepsWhatTheMapCannotWrite,
      .reach = TEST_WRITES_TO_VOLUME,
      .caveat_defect = "Fallow lets go of a node whose gives the map had no room to write down, "
                       "and its blocks stay taken for good" },
};

const TestSuite g_stand_map_suite = {
    .tests = g_stand_map_tests, .count = ARRAY_SIZE(g_stand_map_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
