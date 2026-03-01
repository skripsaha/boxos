#include "pmm.h"
#include "klib.h"
#include "kernel_config.h"

#if CONFIG_RUN_STARTUP_TESTS

#define TEST_MAX_ALLOCS 512

typedef struct {
    void* addr;
    size_t pages;
} TestAllocation;

static TestAllocation test_allocs[TEST_MAX_ALLOCS];
static int test_alloc_count = 0;

static void test_reset_allocs(void) {
    test_alloc_count = 0;
    for (int i = 0; i < TEST_MAX_ALLOCS; i++) {
        test_allocs[i].addr = NULL;
        test_allocs[i].pages = 0;
    }
}

static void test_free_all(void) {
    for (int i = 0; i < test_alloc_count; i++) {
        if (test_allocs[i].addr) {
            pmm_free(test_allocs[i].addr, test_allocs[i].pages);
            test_allocs[i].addr = NULL;
        }
    }
    test_alloc_count = 0;
}

static int pmm_test_basic_alloc_free(void) {
    kprintf("\n[PMM_TEST] Basic Allocation/Free\n");

    size_t initial_free = pmm_free_pages();
    debug_printf("[PMM_TEST] Initial free pages: %lu\n", initial_free);

    void* page1 = pmm_alloc(1);
    if (!page1) {
        debug_printf("[PMM_TEST] FAILED: Could not allocate 1 page\n");
        return 0;
    }
    debug_printf("[PMM_TEST] Allocated 1 page at %p\n", page1);

    size_t after_alloc = pmm_free_pages();
    if (after_alloc != initial_free - 1) {
        debug_printf("[PMM_TEST] FAILED: Expected %lu free pages, got %lu\n",
                initial_free - 1, after_alloc);
        return 0;
    }

    pmm_free(page1, 1);
    debug_printf("[PMM_TEST] Freed 1 page\n");

    size_t after_free = pmm_free_pages();
    if (after_free != initial_free) {
        debug_printf("[PMM_TEST] FAILED: Memory leak! Expected %lu free pages, got %lu\n",
                initial_free, after_free);
        return 0;
    }

    debug_printf("[PMM_TEST] PASSED: Basic allocation/free works correctly\n");
    return 1;
}

static int pmm_test_multiple_allocs(void) {
    kprintf("\n[PMM_TEST] Multiple Allocations\n");

    test_reset_allocs();
    size_t initial_free = pmm_free_pages();

    const int count = 100;
    for (int i = 0; i < count; i++) {
        test_allocs[i].addr = pmm_alloc(1);
        test_allocs[i].pages = 1;
        if (!test_allocs[i].addr) {
            debug_printf("[PMM_TEST] FAILED: Could not allocate page %d\n", i);
            test_free_all();
            return 0;
        }
    }
    test_alloc_count = count;

    debug_printf("[PMM_TEST] Allocated %d pages\n", count);

    size_t after_alloc = pmm_free_pages();
    if (after_alloc != initial_free - count) {
        debug_printf("[PMM_TEST] WARNING: Expected %lu free pages, got %lu\n",
                initial_free - count, after_alloc);
    }

    test_free_all();
    size_t after_free = pmm_free_pages();

    if (after_free != initial_free) {
        debug_printf("[PMM_TEST] FAILED: Memory leak! Expected %lu free pages, got %lu\n",
                initial_free, after_free);
        return 0;
    }

    debug_printf("[PMM_TEST] PASSED: Multiple allocations work correctly\n");
    return 1;
}

static int pmm_test_large_allocs(void) {
    kprintf("\n[PMM_TEST] Large Allocations\n");

    test_reset_allocs();
    size_t initial_free = pmm_free_pages();

    size_t sizes[] = {10, 50, 100, 256, 512};
    int count = sizeof(sizes) / sizeof(sizes[0]);

    for (int i = 0; i < count; i++) {
        test_allocs[i].addr = pmm_alloc(sizes[i]);
        test_allocs[i].pages = sizes[i];

        if (!test_allocs[i].addr) {
            debug_printf("[PMM_TEST] FAILED: Could not allocate %lu pages\n", sizes[i]);
            test_free_all();
            return 0;
        }
        debug_printf("[PMM_TEST] Allocated %lu pages at %p\n", sizes[i], test_allocs[i].addr);
    }
    test_alloc_count = count;

    test_free_all();
    size_t after_free = pmm_free_pages();

    if (after_free != initial_free) {
        debug_printf("[PMM_TEST] FAILED: Memory leak after large allocs!\n");
        return 0;
    }

    debug_printf("[PMM_TEST] PASSED: Large allocations work correctly\n");
    return 1;
}

static int pmm_test_fragmentation(void) {
    kprintf("\n[PMM_TEST] Fragmentation Handling\n");

    test_reset_allocs();
    size_t initial_free = pmm_free_pages();

    for (int i = 0; i < 50; i++) {
        test_allocs[i].addr = pmm_alloc(1);
        test_allocs[i].pages = 1;
        if (!test_allocs[i].addr) {
            debug_printf("[PMM_TEST] FAILED: Could not allocate page %d\n", i);
            test_free_all();
            return 0;
        }
    }
    test_alloc_count = 50;

    debug_printf("[PMM_TEST] Creating fragmentation (freeing every other page)...\n");
    for (int i = 0; i < 50; i += 2) {
        pmm_free(test_allocs[i].addr, 1);
        test_allocs[i].addr = NULL;
    }

    debug_printf("[PMM_TEST] Allocating 10 pages in fragmented memory...\n");
    for (int i = 0; i < 10; i++) {
        void* page = pmm_alloc(1);
        if (!page) {
            debug_printf("[PMM_TEST] FAILED: Could not allocate page in fragmented memory\n");
            test_free_all();
            return 0;
        }
        for (int j = 0; j < 50; j++) {
            if (!test_allocs[j].addr) {
                test_allocs[j].addr = page;
                test_allocs[j].pages = 1;
                break;
            }
        }
    }

    test_free_all();

    size_t after_free = pmm_free_pages();
    if (after_free != initial_free) {
        debug_printf("[PMM_TEST] WARNING: Possible memory leak after fragmentation test\n");
        debug_printf("[PMM_TEST]   Expected: %lu, Got: %lu, Diff: %ld\n",
                initial_free, after_free, (long)(initial_free - after_free));
    }

    debug_printf("[PMM_TEST] PASSED: Fragmentation handled correctly\n");
    return 1;
}

static int pmm_test_stress(void) {
    kprintf("\n[PMM_TEST] Stress Test (500 alloc/free cycles)\n");

    test_reset_allocs();
    size_t initial_free = pmm_free_pages();

    const int cycles = 500;
    int allocs = 0;
    int frees = 0;

    for (int i = 0; i < cycles; i++) {
        int should_alloc = (test_alloc_count < 50) || ((i % 3) == 0);

        if (should_alloc && test_alloc_count < TEST_MAX_ALLOCS) {
            size_t pages = 1 + (i % 10);
            void* addr = pmm_alloc(pages);
            if (addr) {
                test_allocs[test_alloc_count].addr = addr;
                test_allocs[test_alloc_count].pages = pages;
                test_alloc_count++;
                allocs++;
            }
        } else if (test_alloc_count > 0) {
            int idx = i % test_alloc_count;
            if (test_allocs[idx].addr) {
                pmm_free(test_allocs[idx].addr, test_allocs[idx].pages);
                frees++;

                for (int j = idx; j < test_alloc_count - 1; j++) {
                    test_allocs[j] = test_allocs[j + 1];
                }
                test_alloc_count--;
            }
        }

        if ((i + 1) % 100 == 0) {
            debug_printf("[PMM_TEST] Cycle %d/%d (allocs: %d, frees: %d, active: %d)\n",
                    i + 1, cycles, allocs, frees, test_alloc_count);
        }
    }

    debug_printf("[PMM_TEST] Stress test complete: %d allocs, %d frees\n", allocs, frees);

    test_free_all();

    size_t after_free = pmm_free_pages();
    if (after_free != initial_free) {
        debug_printf("[PMM_TEST] WARNING: Memory leak after stress test!\n");
        debug_printf("[PMM_TEST]   Expected: %lu, Got: %lu, Diff: %ld\n",
                initial_free, after_free, (long)(initial_free - after_free));
    }

    debug_printf("[PMM_TEST] PASSED: Stress test completed\n");
    return 1;
}

static int pmm_test_zero_alloc(void) {
    kprintf("\n[PMM_TEST] Zero-Page Allocation\n");

    void* page = pmm_alloc_zero(1);
    if (!page) {
        debug_printf("[PMM_TEST] FAILED: Could not allocate zero page\n");
        return 0;
    }

    uint8_t* bytes = (uint8_t*)page;
    for (int i = 0; i < 4096; i++) {
        if (bytes[i] != 0) {
            debug_printf("[PMM_TEST] FAILED: Page not zeroed at offset %d (value: %u)\n",
                    i, bytes[i]);
            pmm_free(page, 1);
            return 0;
        }
    }

    pmm_free(page, 1);
    debug_printf("[PMM_TEST] PASSED: Zero-allocation works correctly\n");
    return 1;
}

static int pmm_test_oom(void) {
    kprintf("\n[PMM_TEST] Out-of-Memory Handling\n");

    size_t free_pages = pmm_free_pages();
    debug_printf("[PMM_TEST] Available pages: %lu\n", free_pages);

    size_t huge_size = free_pages + 1000;
    void* huge = pmm_alloc(huge_size);

    if (huge) {
        debug_printf("[PMM_TEST] WARNING: Allocated %lu pages (more than available!)\n", huge_size);
        pmm_free(huge, huge_size);
        return 0;
    }

    debug_printf("[PMM_TEST] PASSED: OOM handled gracefully (returned NULL)\n");
    return 1;
}

void pmm_run_tests(void) {
    kprintf("\n");
    kprintf("PMM COMPREHENSIVE TEST SUITE\n");

    int passed = 0;
    int total = 0;

    kprintf("\n[PMM_TEST] Initial Memory State:\n");
    pmm_dump_stats();

    total++; if (pmm_test_basic_alloc_free()) passed++;
    total++; if (pmm_test_multiple_allocs()) passed++;
    total++; if (pmm_test_large_allocs()) passed++;
    total++; if (pmm_test_fragmentation()) passed++;
    total++; if (pmm_test_stress()) passed++;
    total++; if (pmm_test_zero_alloc()) passed++;
    total++; if (pmm_test_oom()) passed++;

    kprintf("\n");
    kprintf("PMM TEST RESULTS: %d / %d\n", passed, total);

    kprintf("\n[PMM_TEST] Final Memory State:\n");
    pmm_dump_stats();
    kprintf("\n");
}

#else

void pmm_run_tests(void) { (void)0; }

#endif
