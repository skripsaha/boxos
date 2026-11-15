#include "tests.h"
#include "stand_tests.h"
#include "../tagfs.h"
#include "chit.h"           /* ChitAnswer — the border every answer passes */

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// Room a cycle gives back is waited for, never answered (R6)
//
// A volume short of room while a Settle cycle would give some back answers
// ERR_WOULD_BLOCK: the room is on its way, and a Settle cycle brings it. That
// word is a handler's to the guide, and a program's boxlib drops a Result that
// carries it — so an operation that met it must wait for the cycle's end and
// be done again, and never hand the word on as its answer.
// ============================================================================

// ----------------------------------------------------------------------------
// The border every answer passes
//
// R6. Whatever answers a program's submit passes one border (ChitAnswer): an
// answer of ERR_WOULD_BLOCK there is the kernel's defect — a STRICT kernel
// stops on it, and stands this test down; any other says so and answers
// ERR_TAGFS_NO_SPACE, the refusal the word stood for. Every other answer
// passes as it is.
// ----------------------------------------------------------------------------

static TestResult TestStandRoomNeverAnAnswer(void)
{
#if CONFIG_STRICT
    return test_steps_aside("a STRICT kernel stops at the border every answer passes: an answer "
                            "of ERR_WOULD_BLOCK is a defect it panics on, never one it hands on");
#else
    TestResult verdict = TEST_PASS;

    TEST_ASSERT_OK(ChitAnswer(OK, 0, 0, "the stand"), "OK passes the border as it is");
    TEST_ASSERT_EQ(ChitAnswer(ERR_TAGFS_NO_SPACE, 0, 0, "the stand"), ERR_TAGFS_NO_SPACE,
                   "and so does a refusal");
    TEST_ASSERT_EQ(ChitAnswer(ERR_DEVICE_NOT_READY, 0, 0, "the stand"), ERR_DEVICE_NOT_READY,
                   "any refusal");
    error_t said = ChitAnswer(ERR_WOULD_BLOCK, 0, 0, "the stand");
    TestEvidence("answers of ERR_WOULD_BLOCK the border let through to a program",
                 said == ERR_WOULD_BLOCK);
    if (said == ERR_WOULD_BLOCK) {
        verdict = TEST_DEFECT;
        goto out;
    }
    TEST_ASSERT_EQ(said, ERR_TAGFS_NO_SPACE,
                   "ERR_WOULD_BLOCK leaves it as ERR_TAGFS_NO_SPACE, the refusal it stood for");
out:
    return verdict;
#endif
}

static const TestCase g_stand_room_tests[] = {
    { .name  = "stand_room_never_an_answer", .func = TestStandRoomNeverAnAnswer,
      .reach = TEST_READS_ONLY,
      .caveat_defect = "an answer of ERR_WOULD_BLOCK reaches a program, whose boxlib drops it, "
                       "and the program waits for ever" },
};

const TestSuite g_stand_room_suite = {
    .tests = g_stand_room_tests, .count = ARRAY_SIZE(g_stand_room_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
