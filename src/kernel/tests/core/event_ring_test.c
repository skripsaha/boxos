#include "event_ring_test.h"
#include "event_ring.h"
#include "events.h"
#include "klib.h"
#include "error.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            kprintf("[FAIL] %s:%d: %s\n", __func__, __LINE__, message); \
            tests_failed++; \
            return -1; \
        } \
        tests_passed++; \
    } while(0)

#define RUN_TEST(test_func) \
    do { \
        kprintf("[TEST] Running %s...\n", #test_func); \
        if (test_func() != 0) { \
            kprintf("[FAIL] %s failed\n", #test_func); \
            return -1; \
        } \
        kprintf("[PASS] %s passed\n", #test_func); \
    } while(0)

static int test_create_destroy(void) {
    EventRingBuffer* ring = event_ring_create(512);
    TEST_ASSERT(ring != NULL, "Create with capacity 512 should succeed");
    TEST_ASSERT(event_ring_capacity(ring) == 512, "Capacity should be 512");
    TEST_ASSERT(event_ring_count(ring) == 0, "Initial count should be 0");
    event_ring_destroy(ring);

    size_t capacities[] = {512, 1024, 2048, 4096};
    for (size_t i = 0; i < 4; i++) {
        size_t cap = capacities[i];
        ring = event_ring_create(cap);
        TEST_ASSERT(ring != NULL, "Create should succeed for valid capacity");
        TEST_ASSERT(event_ring_capacity(ring) == cap, "Capacity should match");
        event_ring_destroy(ring);
    }

    ring = event_ring_create(999);
    TEST_ASSERT(ring != NULL, "Create with invalid capacity should default to 512");
    TEST_ASSERT(event_ring_capacity(ring) == 512, "Should default to min capacity");
    event_ring_destroy(ring);

    event_ring_destroy(NULL);

    return 0;
}

static int test_push_pop_single(void) {
    EventRingBuffer* ring = event_ring_create(512);
    TEST_ASSERT(ring != NULL, "Create should succeed");

    Event evt;
    event_init(&evt, 100, 1);
    evt.magic = EVENT_MAGIC;
    evt.prefix_count = 1;
    evt.prefixes[0] = 0xFF01;

    boxos_error_t err = event_ring_push(ring, &evt);
    TEST_ASSERT(err == BOXOS_OK, "Push should succeed");
    TEST_ASSERT(event_ring_count(ring) == 1, "Count should be 1 after push");
    TEST_ASSERT(!event_ring_is_empty(ring), "Ring should not be empty");

    Event popped;
    err = event_ring_pop(ring, &popped);
    TEST_ASSERT(err == BOXOS_OK, "Pop should succeed");
    TEST_ASSERT(event_ring_count(ring) == 0, "Count should be 0 after pop");
    TEST_ASSERT(event_ring_is_empty(ring), "Ring should be empty");

    TEST_ASSERT(popped.magic == EVENT_MAGIC, "Magic should match");
    TEST_ASSERT(popped.pid == 100, "PID should match");
    TEST_ASSERT(popped.event_id == 1, "Event ID should match");
    TEST_ASSERT(popped.prefix_count == 1, "Prefix count should match");
    TEST_ASSERT(popped.prefixes[0] == 0xFF01, "Prefix should match");

    event_ring_destroy(ring);
    return 0;
}

static int test_push_pop_multiple(void) {
    EventRingBuffer* ring = event_ring_create(512);
    TEST_ASSERT(ring != NULL, "Create should succeed");

    const int NUM_EVENTS = 100;

    for (int i = 0; i < NUM_EVENTS; i++) {
        Event evt;
        event_init(&evt, 100 + i, 1000 + i);
        evt.magic = EVENT_MAGIC;
        evt.prefix_count = 1;
        evt.prefixes[0] = 0x0100 + i;

        boxos_error_t err = event_ring_push(ring, &evt);
        TEST_ASSERT(err == BOXOS_OK, "Push should succeed");
    }

    TEST_ASSERT(event_ring_count(ring) == NUM_EVENTS, "Count should match pushed events");

    for (int i = 0; i < NUM_EVENTS; i++) {
        Event evt;
        boxos_error_t err = event_ring_pop(ring, &evt);
        TEST_ASSERT(err == BOXOS_OK, "Pop should succeed");
        TEST_ASSERT(evt.pid == (uint32_t)(100 + i), "PID should match FIFO order");
        TEST_ASSERT(evt.event_id == (uint32_t)(1000 + i), "Event ID should match FIFO order");
    }

    TEST_ASSERT(event_ring_count(ring) == 0, "Count should be 0 after all pops");

    event_ring_destroy(ring);
    return 0;
}

static int test_push_full(void) {
    EventRingBuffer* ring = event_ring_create(512);
    TEST_ASSERT(ring != NULL, "Create should succeed");

    Event evt;
    event_init(&evt, 100, 1);
    evt.magic = EVENT_MAGIC;

    // Push exactly 409 events (just below 80% threshold)
    // At 409/512 = 79.88%, auto-grow should NOT trigger yet
    for (int i = 0; i < 409; i++) {
        evt.event_id = i;
        boxos_error_t err = event_ring_push(ring, &evt);
        TEST_ASSERT(err == BOXOS_OK, "Push should succeed");
    }

    // After 409 pushes, capacity should still be 512
    size_t capacity_before = event_ring_capacity(ring);
    TEST_ASSERT(capacity_before == 512, "Capacity should still be 512 before threshold");

    // Push 410th event - this will cross 80% and trigger auto-grow INSIDE the push
    evt.event_id = 409;
    boxos_error_t err = event_ring_push(ring, &evt);
    TEST_ASSERT(err == BOXOS_OK, "Push should succeed");

    // After 410th push with auto-grow, capacity should be exactly 1024 (doubled from 512)
    size_t capacity_after = event_ring_capacity(ring);
    TEST_ASSERT(capacity_after == 1024, "Auto-grow should have doubled capacity to 1024");

    // Verify all 410 events are still accessible
    Event popped;
    for (int i = 0; i < 410; i++) {
        err = event_ring_pop(ring, &popped);
        TEST_ASSERT(err == BOXOS_OK, "Pop should succeed after auto-grow");
        TEST_ASSERT(popped.event_id == (uint32_t)i, "Event order should be preserved (FIFO)");
    }

    event_ring_destroy(ring);
    return 0;
}

static int test_auto_grow(void) {
    EventRingBuffer* ring = event_ring_create(512);
    TEST_ASSERT(ring != NULL, "Create should succeed");
    TEST_ASSERT(event_ring_capacity(ring) == 512, "Initial capacity 512");

    Event evt;
    event_init(&evt, 100, 1);
    evt.magic = EVENT_MAGIC;

    // Push 409 events (just below threshold)
    for (int i = 0; i < 409; i++) {
        evt.event_id = i;
        boxos_error_t err = event_ring_push(ring, &evt);
        TEST_ASSERT(err == BOXOS_OK, "Push should succeed");
    }

    // Capacity should still be 512
    size_t capacity_before = event_ring_capacity(ring);
    TEST_ASSERT(capacity_before == 512, "Capacity should be 512 before threshold");

    // Push 410th event - triggers auto-grow
    evt.event_id = 409;
    boxos_error_t err = event_ring_push(ring, &evt);
    TEST_ASSERT(err == BOXOS_OK, "Push should succeed");

    // Capacity should have grown to 1024
    size_t capacity_after = event_ring_capacity(ring);
    TEST_ASSERT(capacity_after == 1024, "Capacity should double to 1024");
    TEST_ASSERT(capacity_after > capacity_before, "Capacity should increase");

    // Verify all 410 events are accessible
    Event popped;
    for (int i = 0; i < 410; i++) {
        err = event_ring_pop(ring, &popped);
        TEST_ASSERT(err == BOXOS_OK, "Pop should succeed after growth");
        TEST_ASSERT(popped.event_id == (uint32_t)i, "Event order preserved after growth");
    }

    event_ring_destroy(ring);
    return 0;
}

static int test_multi_growth(void) {
    EventRingBuffer* ring = event_ring_create(512);
    TEST_ASSERT(ring != NULL, "Create should succeed");

    Event evt;
    event_init(&evt, 100, 1);
    evt.magic = EVENT_MAGIC;

    // Growth #1: 512 → 1024 at 80% of 512 (410 events)
    // Push 409 events (just below threshold)
    for (int i = 0; i < 409; i++) {
        evt.event_id = i;
        boxos_error_t err = event_ring_push(ring, &evt);
        TEST_ASSERT(err == BOXOS_OK, "Push should succeed");
    }

    TEST_ASSERT(event_ring_capacity(ring) == 512, "Should still be 512");

    // Push 410th event - triggers growth to 1024
    evt.event_id = 409;
    event_ring_push(ring, &evt);
    TEST_ASSERT(event_ring_capacity(ring) == 1024, "Should grow to 1024");

    // Growth #2: 1024 → 2048 at 80% of 1024 (820 events)
    // We have 410 events, need 409 more to reach 819
    for (int i = 410; i < 819; i++) {
        evt.event_id = i;
        event_ring_push(ring, &evt);
    }

    TEST_ASSERT(event_ring_capacity(ring) == 1024, "Should still be 1024");

    // Push 820th event - triggers growth to 2048
    evt.event_id = 819;
    event_ring_push(ring, &evt);
    TEST_ASSERT(event_ring_capacity(ring) == 2048, "Should grow to 2048");

    // Verify FIFO ordering across both growths (820 events total)
    Event popped;
    for (int i = 0; i < 820; i++) {
        boxos_error_t err = event_ring_pop(ring, &popped);
        TEST_ASSERT(err == BOXOS_OK, "Pop should succeed");
        TEST_ASSERT(popped.event_id == (uint32_t)i, "FIFO order preserved across growths");
    }

    event_ring_destroy(ring);
    return 0;
}

static int test_max_capacity(void) {
    EventRingBuffer* ring = event_ring_create(4096);
    TEST_ASSERT(ring != NULL, "Create should succeed");
    TEST_ASSERT(event_ring_capacity(ring) == 4096, "Capacity should be max (4096)");

    boxos_error_t err = event_ring_grow(ring);
    TEST_ASSERT(err == BOXOS_ERR_BUFFER_LIMIT_EXCEEDED, "Grow should fail at max capacity");

    TEST_ASSERT(event_ring_capacity(ring) == 4096, "Capacity should remain at max");

    event_ring_destroy(ring);
    return 0;
}

static int test_push_priority(void) {
    EventRingBuffer* ring = event_ring_create(512);
    TEST_ASSERT(ring != NULL, "Create should succeed");

    Event evt;
    event_init(&evt, 100, 1);
    evt.magic = EVENT_MAGIC;

    event_push_result_t result = event_ring_push_priority(ring, &evt, EVENT_PRIORITY_USER);
    TEST_ASSERT(result == EVENT_PUSH_OK, "USER priority push should succeed");
    TEST_ASSERT(event_ring_count(ring) == 1, "Count should be 1");

    evt.event_id = 2;
    result = event_ring_push_priority(ring, &evt, EVENT_PRIORITY_SYSTEM);
    TEST_ASSERT(result == EVENT_PUSH_SYSTEM_OK, "SYSTEM priority push should succeed");
    TEST_ASSERT(event_ring_count(ring) == 2, "Count should be 2");

    event_ring_destroy(ring);
    return 0;
}

static int test_invalid_inputs(void) {
    EventRingBuffer* ring = event_ring_create(512);
    TEST_ASSERT(ring != NULL, "Create should succeed");

    Event evt;
    event_init(&evt, 100, 1);
    evt.magic = EVENT_MAGIC;

    boxos_error_t err = event_ring_push(NULL, &evt);
    TEST_ASSERT(err == BOXOS_ERR_INVALID_ARGUMENT, "Push NULL ring should fail");

    err = event_ring_push(ring, NULL);
    TEST_ASSERT(err == BOXOS_ERR_INVALID_ARGUMENT, "Push NULL event should fail");

    evt.magic = 0xDEADBEEF;
    err = event_ring_push(ring, &evt);
    TEST_ASSERT(err == BOXOS_ERR_INVALID_ARGUMENT, "Push bad magic should fail");
    evt.magic = EVENT_MAGIC;

    err = event_ring_pop(NULL, &evt);
    TEST_ASSERT(err == BOXOS_ERR_INVALID_ARGUMENT, "Pop NULL ring should fail");

    err = event_ring_pop(ring, NULL);
    TEST_ASSERT(err == BOXOS_ERR_INVALID_ARGUMENT, "Pop NULL event should fail");

    err = event_ring_pop(ring, &evt);
    TEST_ASSERT(err == BOXOS_ERR_WOULD_BLOCK, "Pop empty ring should return WOULD_BLOCK");

    err = event_ring_grow(NULL);
    TEST_ASSERT(err == BOXOS_ERR_INVALID_ARGUMENT, "Grow NULL should fail");

    event_ring_destroy(ring);
    return 0;
}

static int test_wrap_around(void) {
    EventRingBuffer* ring = event_ring_create(512);
    TEST_ASSERT(ring != NULL, "Create should succeed");

    Event evt;
    event_init(&evt, 100, 1);
    evt.magic = EVENT_MAGIC;

    for (int i = 0; i < 256; i++) {
        evt.event_id = i;
        boxos_error_t err = event_ring_push(ring, &evt);
        TEST_ASSERT(err == BOXOS_OK, "Push should succeed");
    }

    Event popped;
    for (int i = 0; i < 128; i++) {
        boxos_error_t err = event_ring_pop(ring, &popped);
        TEST_ASSERT(err == BOXOS_OK, "Pop should succeed");
    }

    for (int i = 256; i < 512; i++) {
        evt.event_id = i;
        boxos_error_t err = event_ring_push(ring, &evt);
        TEST_ASSERT(err == BOXOS_OK, "Push should succeed during wrap");
    }

    for (int i = 128; i < 512; i++) {
        boxos_error_t err = event_ring_pop(ring, &popped);
        TEST_ASSERT(err == BOXOS_OK, "Pop should succeed");
        TEST_ASSERT(popped.event_id == (uint32_t)i, "Event ID should match FIFO");
    }

    event_ring_destroy(ring);
    return 0;
}

static int test_available(void) {
    EventRingBuffer* ring = event_ring_create(512);
    TEST_ASSERT(ring != NULL, "Create should succeed");

    TEST_ASSERT(event_ring_available(ring) == 512, "Should have 512 slots available");

    Event evt;
    event_init(&evt, 100, 1);
    evt.magic = EVENT_MAGIC;

    for (int i = 0; i < 100; i++) {
        event_ring_push(ring, &evt);
    }

    TEST_ASSERT(event_ring_available(ring) == 412, "Should have 412 slots available");

    event_ring_destroy(ring);
    return 0;
}

int test_event_ring_dynamic_all(void) {
    kprintf("\n");
    kprintf("====================================================================\n");
    kprintf("EVENT RING DYNAMIC UNIT TESTS\n");
    kprintf("====================================================================\n");

    tests_passed = 0;
    tests_failed = 0;

    RUN_TEST(test_create_destroy);
    RUN_TEST(test_push_pop_single);
    RUN_TEST(test_push_pop_multiple);
    RUN_TEST(test_push_full);
    RUN_TEST(test_auto_grow);
    RUN_TEST(test_multi_growth);
    RUN_TEST(test_max_capacity);
    RUN_TEST(test_push_priority);
    RUN_TEST(test_invalid_inputs);
    RUN_TEST(test_wrap_around);
    RUN_TEST(test_available);

    kprintf("\n");
    kprintf("====================================================================\n");
    kprintf("TEST RESULTS: %d passed, %d failed\n", tests_passed, tests_failed);
    kprintf("====================================================================\n");

    return tests_failed > 0 ? -1 : 0;
}
