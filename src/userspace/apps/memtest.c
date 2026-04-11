#include "box/io.h"
#include "box/system.h"
#include "box/heap.h"
#include "box/string.h"

static int tests_passed = 0;
static int tests_failed = 0;

static void test_result(const char *name, bool passed) {
    if (passed) {
        color(COLOR_GREEN);
        printf("  [PASS] %s\n", name);
        tests_passed++;
    } else {
        color(COLOR_RED);
        printf("  [FAIL] %s\n", name);
        tests_failed++;
    }
    color(COLOR_LIGHT_GRAY);
}

static void test_basic_malloc_free(void) {
    uint8_t *buf = malloc(64);
    if (!buf) { test_result("basic malloc+free", false); return; }
    for (int i = 0; i < 64; i++) buf[i] = (uint8_t)i;
    bool ok = true;
    for (int i = 0; i < 64; i++) if (buf[i] != (uint8_t)i) { ok = false; break; }
    free(buf);
    test_result("basic malloc+free", ok);
}

static void test_calloc_zeroed(void) {
    uint8_t *buf = calloc(16, 4);
    if (!buf) { test_result("calloc zeroed", false); return; }
    bool ok = true;
    for (int i = 0; i < 64; i++) if (buf[i] != 0) { ok = false; break; }
    free(buf);
    test_result("calloc zeroed", ok);
}

static void test_multiple_allocs(void) {
    void *blocks[10];
    size_t sizes[10] = {16, 32, 48, 64, 80, 96, 112, 128, 144, 160};
    bool ok = true;

    for (int i = 0; i < 10; i++) {
        blocks[i] = malloc(sizes[i]);
        if (!blocks[i]) { ok = false; break; }
        memset(blocks[i], (uint8_t)(i + 1), sizes[i]);
    }

    if (ok) {
        for (int i = 0; i < 10; i++) {
            if (!blocks[i]) { ok = false; break; }
            uint8_t *p = (uint8_t *)blocks[i];
            for (size_t j = 0; j < sizes[i]; j++) {
                if (p[j] != (uint8_t)(i + 1)) { ok = false; break; }
            }
            if (!ok) break;
        }
    }

    for (int i = 0; i < 10; i++) if (blocks[i]) free(blocks[i]);
    test_result("multiple allocations", ok);
}

static void test_realloc_grow(void) {
    uint8_t *buf = malloc(32);
    if (!buf) { test_result("realloc grow", false); return; }
    memset(buf, 0xAA, 32);

    uint8_t *bigger = realloc(buf, 128);
    if (!bigger) { free(buf); test_result("realloc grow", false); return; }

    bool ok = true;
    for (int i = 0; i < 32; i++) if (bigger[i] != 0xAA) { ok = false; break; }
    free(bigger);
    test_result("realloc grow", ok);
}

static void test_realloc_shrink(void) {
    uint8_t *buf = malloc(256);
    if (!buf) { test_result("realloc shrink", false); return; }
    memset(buf, 0xBB, 256);

    uint8_t *smaller = realloc(buf, 64);
    if (!smaller) { free(buf); test_result("realloc shrink", false); return; }

    bool ok = true;
    for (int i = 0; i < 64; i++) if (smaller[i] != 0xBB) { ok = false; break; }
    free(smaller);
    test_result("realloc shrink", ok);
}

static void test_free_and_reuse(void) {
    void *first = malloc(128);
    if (!first) { test_result("free and reuse", false); return; }
    free(first);
    void *second = malloc(128);
    bool ok = (second != NULL);
    if (second) free(second);
    test_result("free and reuse", ok);
}

static void test_edge_cases(void) {
    bool ok = true;

    void *z = malloc(0);
    if (z != NULL) ok = false;

    free(NULL);

    void *from_null = realloc(NULL, 64);
    if (!from_null) ok = false;
    else free(from_null);

    test_result("edge cases", ok);
}

static void test_stress(void) {
    void *blocks[100];
    bool ok = true;

    for (int i = 0; i < 100; i++) {
        blocks[i] = malloc(32);
        if (!blocks[i]) { ok = false; break; }
        memset(blocks[i], (uint8_t)(i & 0xFF), 32);
    }

    if (ok) {
        for (int i = 0; i < 100; i++) {
            if (!blocks[i]) { ok = false; break; }
            uint8_t *p = (uint8_t *)blocks[i];
            for (int j = 0; j < 32; j++) {
                if (p[j] != (uint8_t)(i & 0xFF)) { ok = false; break; }
            }
            if (!ok) break;
        }
    }

    for (int i = 0; i < 100; i++) if (blocks[i]) free(blocks[i]);
    test_result("stress test (100 blocks)", ok);
}

static void test_large_alloc(void) {
    uint8_t *buf = malloc(8192);
    if (!buf) { test_result("large allocation (8KB)", false); return; }

    for (int i = 0; i < 8192; i++) buf[i] = (uint8_t)(i & 0xFF);

    bool ok = true;
    for (int i = 0; i < 8192; i++) {
        if (buf[i] != (uint8_t)(i & 0xFF)) { ok = false; break; }
    }
    free(buf);
    test_result("large allocation (8KB)", ok);
}

int main(void) {
    color(COLOR_WHITE);
    println("BoxOS Memory Test");
    color(COLOR_LIGHT_GRAY);
    println("------------------");

    test_basic_malloc_free();
    test_calloc_zeroed();
    test_multiple_allocs();
    test_realloc_grow();
    test_realloc_shrink();
    test_free_and_reuse();
    test_edge_cases();
    test_stress();
    test_large_alloc();

    println("------------------");
    if (tests_failed == 0) {
        color(COLOR_GREEN);
        printf("All %d tests passed.\n", tests_passed);
    } else {
        color(COLOR_YELLOW);
        printf("Results: %d passed, ", tests_passed);
        color(COLOR_RED);
        printf("%d failed\n", tests_failed);
    }
    color(COLOR_LIGHT_GRAY);

    exit(0);
    return 0;
}
