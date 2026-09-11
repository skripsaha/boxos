
#include "pmm.h"
#include "klib.h"

#define PT_CHECK(cond, label) \
    do { if (cond) { pass++; } \
         else { fail++; kprintf("[PMM TEST]   %[R]FAIL%[D]: " label "\n"); } \
    } while (0)

void PmmPoisonTest(void) {
    kprintf("[PMM TEST] Starting poison-bitmap test...\n");
    size_t pass = 0, fail = 0;

    void *clean = pmm_alloc(1, PHYS_TAG_DMA32);
    PT_CHECK(clean != NULL, "alloc DMA32 page");

    uintptr_t target = (uintptr_t)clean;
    PT_CHECK(!pmm_is_poisoned(target), "fresh phys not poisoned");

    size_t before = pmm_poisoned_page_count();
    pmm_set_poisoned(target);
    PT_CHECK(pmm_is_poisoned(target), "pmm_set_poisoned marks bit");
    PT_CHECK(pmm_poisoned_page_count() == before + 1, "count incremented");

    PT_CHECK(pmm_is_range_poisoned(target, 1),
             "pmm_is_range_poisoned detects within range");
    PT_CHECK(!pmm_is_range_poisoned(target + 0x100000, 1),
             "pmm_is_range_poisoned negative for clean range");

    pmm_free(clean, 1);
    void *retry = pmm_alloc(1, PHYS_TAG_DMA32);
    if (retry != NULL) {
        PT_CHECK((uintptr_t)retry != target,
                 "post-poison alloc skips poisoned phys");
        pmm_free(retry, 1);
    } else {
        kprintf("[PMM TEST]   note: DMA32 zone empty after leak — skip "
                "test PASSES via NULL return\n");
    }

    size_t mid = pmm_poisoned_page_count();
    pmm_set_poisoned(target);
    PT_CHECK(pmm_poisoned_page_count() == mid,
             "idempotent pmm_set_poisoned (no double-count)");

    if (fail == 0)
        kprintf("[PMM TEST] %[S]PASSED%[D]: all %zu checks OK\n", pass);
    else
        kprintf("[PMM TEST] %[R]FAILED%[D]: %zu pass, %zu fail\n", pass, fail);
}