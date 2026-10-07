#include "tests.h"
#include "stand_tests.h"
#include "touch_queue.h"
#include "baton.h"
#include "scheduler.h"
#include "amp.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The touch queue's tick fires its nodes in one pass
//
// The PIT tick takes every node whose tick came out of the touch queue under
// the queue's lock, which keeps interrupts off. A tick that walked the queue
// from its head again for every node it fired held them off for the nodes it
// fired times the queue's depth. The nodes here are the test's own, each a
// knock of one Knock, and every one of them is out of the queue on the way out.
// The startup tests run before the tick does, so the test ticks in its place,
// with the tick that fires the nodes it armed, and answers the knock on its own
// core, the drain core, as the core's loop would.
// ============================================================================

#define STAND_TOUCH_FAR     2048u       /* nodes armed for a tick days ahead */
#define STAND_TOUCH_DUE     256u        /* armed before them for the next ticks */
#define STAND_TOUCH_AHEAD   2u          /* ticks from now the due nodes fire at */
#define STAND_TOUCH_DAYS    (250ull * 86400ull * 30ull)
#define STAND_TOUCH_SLACK   64u         /* nodes of the machine's own the queue may hold */

static Knock             s_touch_knock;
static volatile uint32_t s_touch_visits;

static void StandTouchVisit(void* ctx)
{
    KnockOpen((Knock*)ctx);
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
 * 256 nodes are armed for the tick after next, and 2048 for a tick a month
 * ahead after them, so every due node lies behind every far one. The tick
 * that fires the 256 looks at no more nodes than the queue holds; one that
 * walked it again from its head for each would look at half a million.
 */
TestResult TestStandTouchQueueTickOnePass(void)
{
    TestResult      verdict = TEST_PASS;
    uint32_t        nodes   = STAND_TOUCH_FAR + STAND_TOUCH_DUE;
    TouchQueueNode* node    = kmalloc(sizeof(TouchQueueNode) * nodes);
    uint8_t         core    = amp_get_core_index();

    TEST_ASSERT(node != NULL, "room for the test's nodes");
    memset(node, 0, sizeof(TouchQueueNode) * nodes);
    KnockInit(&s_touch_knock, StandTouchVisit, &s_touch_knock);
    s_touch_visits = 0;

    (void)TouchQueueTickMost();
    uint64_t now = __atomic_load_n(&g_global_tick, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < STAND_TOUCH_DUE; i++)
        TouchQueueArmKnock(&node[STAND_TOUCH_FAR + i], &s_touch_knock, now + STAND_TOUCH_AHEAD);
    for (uint32_t i = 0; i < STAND_TOUCH_FAR; i++)
        TouchQueueArmKnock(&node[i], &s_touch_knock, now + STAND_TOUCH_DAYS);

    TouchQueueTick(now + STAND_TOUCH_AHEAD);
    BatonPump(core);
    uint32_t most = TouchQueueTickMost();

    TEST_ASSERT_EQ(StandTouchStillArmed(node, STAND_TOUCH_FAR, STAND_TOUCH_DUE), 0,
                   "every node whose tick came left the queue");
    TEST_ASSERT(__atomic_load_n(&s_touch_visits, __ATOMIC_ACQUIRE) >= 1,
                "and its knock was answered");
    TEST_ASSERT_EQ(StandTouchStillArmed(node, 0, STAND_TOUCH_FAR), STAND_TOUCH_FAR,
                   "while every node a month ahead still waits");
    TestEvidence("nodes the dearest tick looked at:", most);
    TEST_ASSERT(most <= nodes + STAND_TOUCH_SLACK,
                "a tick looks at each node of the queue once, however many of them fire");
out:
    if (node) {
        for (uint32_t i = nodes; i-- > 0; )
            TouchQueueDisarm(&node[i]);
        kfree(node);
    }
    return verdict;
}

#endif /* CONFIG_VOLUME_TESTS */
