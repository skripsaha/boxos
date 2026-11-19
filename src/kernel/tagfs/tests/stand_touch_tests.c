#include "tests.h"
#include "stand_tests.h"
#include "touch_queue.h"
#include "pit.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The touch queue fires its nodes in one pass
//
// A pass takes every node whose moment came out of the touch queue under the
// queue's lock, which keeps interrupts off. A pass that walked the queue from
// its head again for every node it fired held them off for the nodes it fired
// times the queue's depth; the queue is nearest first, so a pass looks at the
// nodes it fires and the one after. The nodes here are the test's own, each a
// CALL of one counting function, and every one of them is out of the queue on
// the way out. The test fires its own pass at the moment its due nodes are
// armed for, a second ahead — long before the keeper's one-shot would.
// ============================================================================

#define STAND_TOUCH_FAR     2048u       /* nodes armed for a moment a month ahead */
#define STAND_TOUCH_DUE     256u        /* armed before them for a second ahead */
#define STAND_TOUCH_AHEAD   1000000ull  /* µs from now the due nodes fire at */
#define STAND_TOUCH_MONTH   (1000000ull * 86400ull * 30ull)
#define STAND_TOUCH_SLACK   64u         /* nodes of the machine's own the queue may hold */

static volatile uint32_t s_touch_visits;

static void StandTouchVisit(void)
{
    __atomic_add_fetch(&s_touch_visits, 1u, __ATOMIC_RELEASE);
}

static uint32_t StandTouchStillArmed(const TouchQueueNode* node, uint32_t from, uint32_t count)
{
    uint32_t armed = 0;
    for (uint32_t i = from; i < from + count; i++)
        armed += __atomic_load_n(&node[i].armed, __ATOMIC_ACQUIRE) != 0;
    return armed;
}

/*
 * 256 nodes are armed for a second ahead, and 2048 for a month ahead after
 * them. The pass that fires the 256 looks at no more nodes than the queue
 * holds; one that walked it again from its head for each would look at half a
 * million.
 */
TestResult TestStandTouchQueueFireOnePass(void)
{
    TestResult      verdict = TEST_PASS;
    uint32_t        nodes   = STAND_TOUCH_FAR + STAND_TOUCH_DUE;
    TouchQueueNode* node    = kmalloc(sizeof(TouchQueueNode) * nodes);

    TEST_ASSERT(node != NULL, "room for the test's nodes");
    memset(node, 0, sizeof(TouchQueueNode) * nodes);
    s_touch_visits = 0;

    (void)TouchQueueFireMost();
    uint64_t now = pit_get_uptime_us();
    for (uint32_t i = 0; i < STAND_TOUCH_DUE; i++)
        TouchQueueArmCall(&node[STAND_TOUCH_FAR + i], StandTouchVisit, now + STAND_TOUCH_AHEAD);
    for (uint32_t i = 0; i < STAND_TOUCH_FAR; i++)
        TouchQueueArmCall(&node[i], StandTouchVisit, now + STAND_TOUCH_MONTH);

    TouchQueueFireDue(now + STAND_TOUCH_AHEAD);
    uint32_t most = TouchQueueFireMost();

    TEST_ASSERT_EQ(StandTouchStillArmed(node, STAND_TOUCH_FAR, STAND_TOUCH_DUE), 0,
                   "every node whose moment came left the queue");
    TEST_ASSERT_EQ(__atomic_load_n(&s_touch_visits, __ATOMIC_ACQUIRE), STAND_TOUCH_DUE,
                   "and each of them was called once");
    TEST_ASSERT_EQ(StandTouchStillArmed(node, 0, STAND_TOUCH_FAR), STAND_TOUCH_FAR,
                   "while every node a month ahead still waits");
    TestEvidence("nodes the dearest pass looked at:", most);
    TEST_ASSERT(most <= nodes + STAND_TOUCH_SLACK,
                "a pass looks at each node of the queue once, however many of them fire");
out:
    if (node) {
        for (uint32_t i = nodes; i-- > 0; )
            TouchQueueDisarm(&node[i]);
        kfree(node);
    }
    return verdict;
}

#endif /* CONFIG_VOLUME_TESTS */
