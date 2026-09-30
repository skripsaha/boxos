#ifndef TAGFS_TEST_H
#define TAGFS_TEST_H

#include "../../lib/kernel/ktypes.h"
#include "../../lib/kernel/klib.h"
#include "../../core/error/error.h"
#include "../tagfs.h"

// ============================================================================
// TagFS startup tests
//
// A test OBSERVES and the runner JUDGES. A test says what it saw — it behaved
// (TEST_PASS), it did not (TEST_FAIL), it could not be asked here (TEST_SKIP),
// or the defect it was written for was seen, positively, with a number
// (TEST_DEFECT) — and the runner decides what that means from the TestCase:
// a defect a caveat names is held, a defect nobody named any more is a
// failure, and a caveated test that behaves has outlived its caveat.
// ============================================================================

typedef enum {
    TEST_PASS   = 0,
    TEST_FAIL   = 1,
    TEST_SKIP   = 2,
    TEST_DEFECT = 3
} TestResult;

/*
 * Does this test WRITE to the mounted volume?
 *
 * It matters because the volume a running machine has mounted belongs to
 * whoever is using the machine. A test that creates files, allocates blocks or
 * writes the journal is doing that to their data, on their medium, every
 * time they switch the computer on.
 *
 * Found on the owner's flash drive after two boots: test_file, test_rw,
 * stress_file_0..2 sitting among his own files, next_file_id grown from 57 to
 * 135, nineteen blocks gone, and the metadata pool chained into the data run.
 * None of it visible in QEMU, where the image is rebuilt by every build.
 */
typedef enum {
    TEST_READS_ONLY = 0,
    TEST_WRITES_TO_VOLUME = 1
} TestReach;

/*
 * One test, always written with designated initializers.
 *
 * caveat_defect: the defect this test stands for — every test that can see
 * one names it, and keeps the words for good. caveat_step: the step of the
 * plan that discharges it, while the test still reproduces it on purpose.
 * Both NULL for a test that stands for no defect. Lifting a caveat is deleting
 * the caveat_step line in the commit of that step — caveat_defect stays, so a
 * defect that comes back after the lift is named in the line that fails it,
 * and the test itself does not change.
 */
typedef struct {
    const char* name;
    TestResult (*func)(void);
    TestReach   reach;
    const char* caveat_step;
    const char* caveat_defect;
} TestCase;

/* A suite: `const TestSuite g_<name>_suite`, defined in a file under tests/ (a
 * file may hold more than one) and listed in TEST_SUITES below. */
typedef struct {
    const TestCase* tests;
    uint32_t        count;
} TestSuite;

typedef struct {
    uint32_t total_tests;
    uint32_t total_passed;
    uint32_t total_failed;
    uint32_t total_skipped;
    uint32_t total_held;      /* a known defect reproduced under its caveat */
} TestStats;

/*
 * Every suite the runner walks, in the order it walks them.
 *
 * One line per suite, and that one line is both the suite's declaration and
 * its place in the walk, so a suite can never be written and then silently not
 * run. A suite that exists only in a VOLUME_TESTS=on kernel — its tests reach
 * for hooks no other build carries — goes in as VOLUME_TESTS_ONLY(SUITE(x)),
 * still one line. The closing comment is part of the list, so a line added
 * above it touches no other line.
 */
#ifdef CONFIG_VOLUME_TESTS
#define VOLUME_TESTS_ONLY(line) line
#else
#define VOLUME_TESTS_ONLY(line)
#endif

#define TEST_SUITES(SUITE) \
    SUITE(core)            \
    SUITE(journal)         \
    SUITE(stress)          \
    SUITE(braid)           \
    SUITE(cow)             \
    SUITE(boxhash)         \
    SUITE(integrity)       \
    SUITE(use_context)     \
    VOLUME_TESTS_ONLY(SUITE(stand)) \
    VOLUME_TESTS_ONLY(SUITE(stand_lock)) \
    VOLUME_TESTS_ONLY(SUITE(stand_chain)) \
    VOLUME_TESTS_ONLY(SUITE(stand_spelling)) \
    VOLUME_TESTS_ONLY(SUITE(stand_roadstead)) \
    VOLUME_TESTS_ONLY(SUITE(stand_map)) \
    VOLUME_TESTS_ONLY(SUITE(stand_plot)) \
    VOLUME_TESTS_ONLY(SUITE(stand_index)) \
    /* the walk ends here; a new suite's line goes above */

#define TEST_SUITE_DECLARED(name) extern const TestSuite g_##name##_suite;
TEST_SUITES(TEST_SUITE_DECLARED)

// ============================================================================
// Assertions
//
// Every test is written the same way:
//
//     TestResult verdict = TEST_PASS;
//     ...every resource the test will take, declared and cleared here...
//     TEST_ASSERT...;
// out:
//     ...every resource released, whatever path arrived here...
//     return verdict;
//
// A failed assertion says so with kprintf — test, file:line, what should have
// held, and the two values — and jumps to `out`. It used to be debug_printf and
// a bare `return TEST_FAIL`: silent in every build that goes on a board, and
// every file, block and snapshot the test held stayed on the volume.
//
// The two values are what was seen and what had to be seen; a truth that did
// not hold prints (0, 1).
// ============================================================================

TestResult TestFailedAt(const char* file, int line, const char* what,
                        int64_t seen, int64_t wanted);

#define TEST_ASSERT(cond, msg) do {                                          \
    if (!(cond)) {                                                           \
        verdict = TestFailedAt(__FILE__, __LINE__, (msg), 0, 1);             \
        goto out;                                                            \
    }                                                                        \
} while (0)

#define TEST_ASSERT_EQ(a, b, msg) do {                                       \
    int64_t _seen = (int64_t)(a), _wanted = (int64_t)(b);                    \
    if (_seen != _wanted) {                                                  \
        verdict = TestFailedAt(__FILE__, __LINE__, (msg), _seen, _wanted);   \
        goto out;                                                            \
    }                                                                        \
} while (0)

#define TEST_ASSERT_OK(err, msg) do {                                        \
    int64_t _err = (int64_t)(err);                                           \
    if (_err != OK) {                                                        \
        verdict = TestFailedAt(__FILE__, __LINE__, (msg), _err, OK);         \
        goto out;                                                            \
    }                                                                        \
} while (0)

// ============================================================================
// What the suites share
// ============================================================================

/* The evidence a caveated test owes on both of its outcomes: the words that
 * say what was counted, and the count. Printed as "(<words> <n>)". */
void TestEvidence(const char* words, uint64_t n);

/* What a test that writes leaves behind, as the runner counts it before and
 * after every such test (tests.c, the residue guard): the files, and the
 * blocks in use outside the volume's own chains. A test whose defect IS a
 * leak takes the same count, to measure what it leaked and to give back
 * exactly that — so the guard stays strict for everybody. */
typedef struct {
    uint64_t files;
    uint64_t held;
} VolumeTally;

bool VolumeTallyTake(VolumeTally* tally);

/* Why a test that ran decided not to run. Returns TEST_SKIP. */
TestResult test_steps_aside(const char* why);

/* Bytes that say where they are: each depends on the stream it belongs to and
 * on its absolute offset, so two streams never hold the same block and a block
 * read back from the wrong place never matches. */
void TestFillAt(void* buffer, uint32_t size, uint32_t stream, uint64_t offset);

/* The offset of the first byte where a and b differ, or n when they agree. */
uint32_t TestFirstDifference(const void* a, const void* b, uint32_t n);

/* One write or read through an open handle, from or into the kernel's own
 * memory: the byte count tagfs_write / tagfs_read answered, or -1 when it
 * answered a refusal. The handle's offset moves by what moved. */
int TestHandleWrite(TagFSFileHandle* handle, const void* data, uint32_t bytes);
int TestHandleRead(TagFSFileHandle* handle, void* buffer, uint32_t capacity);

/* A whole file written in one call through a fresh handle, and read back the
 * same way: as TestHandleWrite / TestHandleRead answer, or -1 when the file
 * would not open. The handle is closed on every path. */
int TestFileWrite(uint32_t file_id, const void* data, uint32_t bytes);
int TestFileRead(uint32_t file_id, void* buffer, uint32_t capacity);

void fill_random(void* buffer, uint32_t size);
void fill_pattern(void* buffer, uint32_t size, uint8_t pattern);

/* Four blocks each, shared by the suites. The runner is single-threaded at
 * boot, so one set is safe across tests, and a block buffer does not belong
 * on a kernel stack (the stack guard refuses frames over 1024 bytes). */
extern uint8_t g_test_buffer[16384];
extern uint8_t g_test_output[16384];

// ============================================================================
// Runner
// ============================================================================

error_t TagFS_TestsInit(void);
void TagFS_TestsShutdown(void);

/* OK exactly when nothing failed. A held caveat is not a failure. */
error_t TagFS_RunAllTests(TestStats* stats);

#endif // TAGFS_TEST_H
