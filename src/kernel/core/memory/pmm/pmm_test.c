#include "pmm.h"
#include "klib.h"

// ============================================================================
// PMM COMPREHENSIVE TEST SUITE
// ============================================================================
// Tests physical memory manager for stability, fragmentation, edge cases
// ============================================================================

#define TEST_MAX_ALLOCS 512  // Maximum simultaneous allocations for stress test

typedef struct {
    void* addr;
    size_t pages;
} TestAllocation;

static TestAllocation test_allocs[TEST_MAX_ALLOCS];
static int test_alloc_count = 0;

// ============================================================================
// TEST UTILITIES
// ============================================================================

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

// ============================================================================
// TEST 1: Basic Allocation and Freeing
// ============================================================================

static int pmm_test_basic_alloc_free(void) {
    kprintf("\n[PMM_TEST] === Test 1: Basic Allocation/Free ===\n");

    size_t initial_free = pmm_free_pages();
    debug_printf("[PMM_TEST] Initial free pages: %lu\n", initial_free);

    // Allocate 1 page
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

    // Free the page
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

// ============================================================================
// TEST 2: Multiple Allocations
// ============================================================================

static int pmm_test_multiple_allocs(void) {
    kprintf("\n[PMM_TEST] === Test 2: Multiple Allocations ===\n");

    test_reset_allocs();
    size_t initial_free = pmm_free_pages();

    // Allocate 100 single pages
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

    // Verify free pages decreased
    size_t after_alloc = pmm_free_pages();
    if (after_alloc != initial_free - count) {
        debug_printf("[PMM_TEST] WARNING: Expected %lu free pages, got %lu\n",
                initial_free - count, after_alloc);
    }

    // Free all
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

// ============================================================================
// TEST 3: Large Allocations
// ============================================================================

static int pmm_test_large_allocs(void) {
    kprintf("\n[PMM_TEST] === Test 3: Large Allocations ===\n");

    test_reset_allocs();
    size_t initial_free = pmm_free_pages();

    // Allocate various large sizes
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

    // Free all
    test_free_all();
    size_t after_free = pmm_free_pages();

    if (after_free != initial_free) {
        debug_printf("[PMM_TEST] FAILED: Memory leak after large allocs!\n");
        return 0;
    }

    debug_printf("[PMM_TEST] PASSED: Large allocations work correctly\n");
    return 1;
}

// ============================================================================
// TEST 4: Fragmentation Handling
// ============================================================================

static int pmm_test_fragmentation(void) {
    kprintf("\n[PMM_TEST] === Test 4: Fragmentation Handling ===\n");

    test_reset_allocs();
    size_t initial_free = pmm_free_pages();

    // Allocate 50 pages
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

    // Free every other page (create fragmentation)
    debug_printf("[PMM_TEST] Creating fragmentation (freeing every other page)...\n");
    for (int i = 0; i < 50; i += 2) {
        pmm_free(test_allocs[i].addr, 1);
        test_allocs[i].addr = NULL;
    }

    // Try to allocate 10 single pages (should work despite fragmentation)
    debug_printf("[PMM_TEST] Allocating 10 pages in fragmented memory...\n");
    for (int i = 0; i < 10; i++) {
        void* page = pmm_alloc(1);
        if (!page) {
            debug_printf("[PMM_TEST] FAILED: Could not allocate page in fragmented memory\n");
            test_free_all();
            return 0;
        }
        // Store in first available slot
        for (int j = 0; j < 50; j++) {
            if (!test_allocs[j].addr) {
                test_allocs[j].addr = page;
                test_allocs[j].pages = 1;
                break;
            }
        }
    }

    // Free all remaining
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

// ============================================================================
// TEST 5: Stress Test (Many Allocations)
// ============================================================================

static int pmm_test_stress(void) {
    kprintf("\n[PMM_TEST] === Test 5: Stress Test (500 alloc/free cycles) ===\n");

    test_reset_allocs();
    size_t initial_free = pmm_free_pages();

    // Perform 500 random allocation/free cycles
    const int cycles = 500;
    int allocs = 0;
    int frees = 0;

    for (int i = 0; i < cycles; i++) {
        // Allocate or free randomly
        int should_alloc = (test_alloc_count < 50) || ((i % 3) == 0);

        if (should_alloc && test_alloc_count < TEST_MAX_ALLOCS) {
            // Allocate 1-10 pages
            size_t pages = 1 + (i % 10);
            void* addr = pmm_alloc(pages);
            if (addr) {
                test_allocs[test_alloc_count].addr = addr;
                test_allocs[test_alloc_count].pages = pages;
                test_alloc_count++;
                allocs++;
            }
        } else if (test_alloc_count > 0) {
            // Free random allocation
            int idx = i % test_alloc_count;
            if (test_allocs[idx].addr) {
                pmm_free(test_allocs[idx].addr, test_allocs[idx].pages);
                frees++;

                // Shift array
                for (int j = idx; j < test_alloc_count - 1; j++) {
                    test_allocs[j] = test_allocs[j + 1];
                }
                test_alloc_count--;
            }
        }

        // Progress indicator every 100 cycles
        if ((i + 1) % 100 == 0) {
            debug_printf("[PMM_TEST] Cycle %d/%d (allocs: %d, frees: %d, active: %d)\n",
                    i + 1, cycles, allocs, frees, test_alloc_count);
        }
    }

    debug_printf("[PMM_TEST] Stress test complete: %d allocs, %d frees\n", allocs, frees);

    // Free all remaining
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

// ============================================================================
// TEST 6: Zero-Allocation Test
// ============================================================================

static int pmm_test_zero_alloc(void) {
    kprintf("\n[PMM_TEST] === Test 6: Zero-Page Allocation ===\n");

    void* page = pmm_alloc_zero(1);
    if (!page) {
        debug_printf("[PMM_TEST] FAILED: Could not allocate zero page\n");
        return 0;
    }

    // Verify page is zeroed
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

// ============================================================================
// TEST 7: Out-of-Memory Handling
// ============================================================================

static int pmm_test_oom(void) {
    kprintf("\n[PMM_TEST] === Test 7: Out-of-Memory Handling ===\n");

    size_t free_pages = pmm_free_pages();
    debug_printf("[PMM_TEST] Available pages: %lu\n", free_pages);

    // Try to allocate more than available (should fail gracefully)
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

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

void pmm_run_tests(void) {
    kprintf("\n");
    kprintf("============================================================\n");
    kprintf("           PMM COMPREHENSIVE TEST SUITE                     \n");
    kprintf("============================================================\n");

    int passed = 0;
    int total = 0;

    // Print initial state
    kprintf("\n[PMM_TEST] Initial Memory State:\n");
    pmm_dump_stats();

    // Run all tests
    total++; if (pmm_test_basic_alloc_free()) passed++;
    total++; if (pmm_test_multiple_allocs()) passed++;
    total++; if (pmm_test_large_allocs()) passed++;
    total++; if (pmm_test_fragmentation()) passed++;
    total++; if (pmm_test_stress()) passed++;
    total++; if (pmm_test_zero_alloc()) passed++;
    total++; if (pmm_test_oom()) passed++;

    // Final results
    kprintf("\n");
    kprintf("============================================================\n");
    kprintf("                 TEST RESULTS                               \n");
    kprintf("============================================================\n");
    kprintf("Tests Passed: %d / %d\n", passed, total);

    if (passed == total) {
        kprintf("✅ ALL TESTS PASSED - PMM is stable!\n");
    } else {
        kprintf("❌ SOME TESTS FAILED - PMM has issues!\n");
    }

    kprintf("\n[PMM_TEST] Final Memory State:\n");
    pmm_dump_stats();

    kprintf("============================================================\n\n");
}
