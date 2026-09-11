#ifndef TAGFS_TEST_H
#define TAGFS_TEST_H

#include "../../lib/kernel/ktypes.h"
#include "../../lib/kernel/klib.h"
#include "../../core/error/error.h"
#include "../tagfs_constants.h"


typedef enum {
    TEST_PASS = 0,
    TEST_FAIL = 1,
    TEST_SKIP = 2,
    TEST_CRASH = 3
} TestResult;

typedef enum {
    TEST_READS_ONLY = 0,
    TEST_WRITES_TO_VOLUME = 1
} TestReach;

typedef struct {
    const char* name;
    TestResult (*func)(void);
    TestResult result;
    uint64_t duration_ms;
    char error_message[256];
    TestReach reach;
} TestCase;

typedef struct {
    const char* name;
    TestCase* tests;
    uint32_t test_count;
    uint32_t passed;
    uint32_t failed;
    uint32_t skipped;
} TestSuite;

typedef struct {
    uint32_t total_tests;
    uint32_t total_passed;
    uint32_t total_failed;
    uint32_t total_skipped;
    uint64_t total_duration_ms;
    uint64_t start_time;
    uint64_t end_time;
} TestStats;


#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        debug_printf("[FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg); \
        return TEST_FAIL; \
    } \
} while(0)

#define TEST_ASSERT_EQ(a, b, msg) do { \
    if ((a) != (b)) { \
        debug_printf("[FAIL] %s:%d: %s (expected %lu, got %lu)\n", \
                     __FILE__, __LINE__, msg, (unsigned long)(b), (unsigned long)(a)); \
        return TEST_FAIL; \
    } \
} while(0)

#define TEST_ASSERT_OK(err, msg) do { \
    error_t _err = (err); \
    if (_err != OK) { \
        debug_printf("[FAIL] %s:%d: %s (error=%d)\n", __FILE__, __LINE__, msg, _err); \
        return TEST_FAIL; \
    } \
} while(0)


error_t TagFS_TestsInit(void);
void TagFS_TestsShutdown(void);

error_t TagFS_RunAllTests(TestStats* stats);

error_t TagFS_RunSuite(const char* suite_name, TestStats* stats);

TestSuite* TagFS_GetCoreTests(void);
TestSuite* TagFS_GetCompressionTests(void);
TestSuite* TagFS_GetJournalTests(void);
TestSuite* TagFS_GetSnapshotTests(void);
TestSuite* TagFS_GetBraidTests(void);
TestSuite* TagFS_GetStressTests(void);

void TagFS_PrintTestResults(const TestStats* stats);
void TagFS_DumpState(void);

#endif