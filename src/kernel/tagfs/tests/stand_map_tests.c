#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The block map — where a take goes, a give that comes twice, and the ground a
// growing file keeps in hand
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
};

const TestSuite g_stand_map_suite = {
    .tests = g_stand_map_tests, .count = ARRAY_SIZE(g_stand_map_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
