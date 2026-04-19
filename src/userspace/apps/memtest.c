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

// ── Tagged malloc tests ───────────────────────────────────────────────────────

static void test_tagged_basic(void) {
    void *p = malloc(64, "mt:buf");
    if (!p) { test_result("tagged malloc alloc", false); return; }

    // Verify tag registered and counted
    bool ok = (heap_lookup_tag("mt:buf") != HEAP_TAG_NONE) &&
              (heap_count_tag("mt:buf") == 1);

    // Write and verify data still works normally
    memset(p, 0xCC, 64);
    uint8_t *b = (uint8_t *)p;
    for (int i = 0; i < 64 && ok; i++)
        if (b[i] != 0xCC) ok = false;

    free(p);

    // Tag count must drop to 0 after free
    ok = ok && (heap_count_tag("mt:buf") == 0);

    test_result("tagged malloc: alloc+count+free", ok);
}

static void test_tagged_multi(void) {
    void *a = malloc(32,  "mt:net");
    void *b = malloc(64,  "mt:net");
    void *c = malloc(128, "mt:ui");
    bool ok = (a && b && c);

    if (ok) {
        ok = (heap_count_tag("mt:net") == 2) &&
             (heap_count_tag("mt:ui")  == 1);
    }

    if (a) free(a);
    if (b) free(b);
    if (c) free(c);

    ok = ok && (heap_count_tag("mt:net") == 0) &&
               (heap_count_tag("mt:ui")  == 0);

    test_result("tagged malloc: multiple tags + counts", ok);
}

// Callback used by test_tagged_iterate
static size_t g_iter_count = 0;
static void iter_cb(void *ptr, size_t size, const char *tag_name, void *userdata) {
    (void)ptr; (void)size; (void)tag_name; (void)userdata;
    g_iter_count++;
}

static void test_tagged_iterate(void) {
    void *p1 = malloc(16, "mt:iter");
    void *p2 = malloc(32, "mt:iter");
    void *p3 = malloc(48, "mt:iter");
    bool ok = (p1 && p2 && p3);

    if (ok) {
        g_iter_count = 0;
        heap_iterate_tag("mt:iter", iter_cb, NULL);
        ok = (g_iter_count == 3);
    }

    if (p1) free(p1);
    if (p2) free(p2);
    if (p3) free(p3);

    // After free, iteration must yield 0
    if (ok) {
        g_iter_count = 0;
        heap_iterate_tag("mt:iter", iter_cb, NULL);
        ok = (g_iter_count == 0);
    }

    test_result("tagged malloc: heap_iterate_tag", ok);
}

static void test_tagged_realloc_preserves(void) {
    void *p = malloc(32, "mt:realloc");
    if (!p) { test_result("tagged realloc preserves tag", false); return; }

    memset(p, 0xAB, 32);

    // Grow — forces relocation, tag must survive
    void *q = realloc(p, 512);
    if (!q) { free(p); test_result("tagged realloc preserves tag", false); return; }

    bool ok = (heap_count_tag("mt:realloc") == 1);

    // Data integrity
    uint8_t *b = (uint8_t *)q;
    for (int i = 0; i < 32 && ok; i++)
        if (b[i] != 0xAB) ok = false;

    free(q);
    ok = ok && (heap_count_tag("mt:realloc") == 0);

    test_result("tagged realloc preserves tag", ok);
}

static void test_tagged_name_lookup(void) {
    void *p = malloc(16, "mt:named");
    if (!p) { test_result("heap_tag_name lookup", false); return; }

    uint8_t id = heap_lookup_tag("mt:named");
    bool ok = (id != HEAP_TAG_NONE);

    if (ok) {
        const char *name = heap_tag_name(id);
        ok = (name != NULL) && (strncmp(name, "mt:named", 8) == 0);
    }

    free(p);
    test_result("heap_tag_name lookup", ok);
}

static void test_tagged_dump(void) {
    void *p1 = malloc(64,  "mt:dump_a");
    void *p2 = malloc(128, "mt:dump_b");

    // Just verify dump doesn't crash
    heap_dump_tags();

    if (p1) free(p1);
    if (p2) free(p2);

    test_result("heap_dump_tags no crash", true);
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

    color(COLOR_WHITE);
    println("-- Tagged malloc --");
    color(COLOR_LIGHT_GRAY);
    test_tagged_basic();
    test_tagged_multi();
    test_tagged_iterate();
    test_tagged_realloc_preserves();
    test_tagged_name_lookup();
    test_tagged_dump();

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
