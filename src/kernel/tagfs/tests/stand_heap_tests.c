#include "tests.h"
#include "stand_tests.h"
#include "kheap.h"
#include "pmm.h"
#include "vmm.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// The kernel heap grows by regions (Quill, decision 4)
//
// What the volume's writers now stand on when the machine runs short: a heap
// as big as the machine lets it be, pieces of whole pages that go back to
// the page allocator, a class of cabins refused out loud before it holds more
// than it leaves, and a reserve the kernel's own paths are served from while
// that class is refused. Each test takes what it proves on the kernel's own
// account or on one class's, and gives every byte back on every path out.
//
// The blocks a test holds are chained through their own first word, so no
// list of them has to live anywhere else.
// ============================================================================

static void StandHeapGiveChain(void **chain)
{
    while (*chain) {
        void *next = *(void **)*chain;
        kfree(*chain);
        *chain = next;
    }
}

/* Blocks of the pool, 24 KiB each — under a large piece's eight pages — until
 * the heap has taken two regions more than it had; then every one of them
 * back, and every region they grew it by back to the page allocator but the
 * one it keeps in hand. At most half of what the machine has free is asked,
 * so the test cannot be what runs the machine out. */
TestResult TestStandHeapGrowsByRegionsAndGivesBack(void)
{
    static const size_t piece = 24u * 1024u;

    TestResult verdict = TEST_PASS;
    KHeapScope outer   = KHeapScopeEnter(KHEAP_CHARGE_KERNEL, 0);
    KHeapShape before, grown, after;
    void      *chain   = NULL;
    uint32_t   blocks  = 0;
    size_t     budget  = PmmFreePagesNow() * VMM_PAGE_SIZE / 2;

    KHeapShapeNow(&before);
    grown = before;
    while (grown.regions < before.regions + 2 && (size_t)blocks * piece < budget) {
        void **block = kmalloc(piece);
        if (!block)
            break;
        *block = chain;
        chain  = block;
        blocks++;
        KHeapShapeNow(&grown);
    }
    TEST_ASSERT(grown.regions >= before.regions + 2,
                "the heap took two regions more than it had, under blocks of the pool");
    TEST_ASSERT(grown.region_bytes > before.region_bytes,
                "and what it takes from the machine grew with them");

    StandHeapGiveChain(&chain);
    KHeapShapeNow(&after);
    kprintf("[STAND] heap: %u block(s) of %zu bytes took it from %u region(s) of %zu KB "
            "to %u of %zu KB; given back, %u region(s) of %zu KB\n",
            blocks, piece, before.regions, before.region_bytes / 1024, grown.regions,
            grown.region_bytes / 1024, after.regions, after.region_bytes / 1024);
    TEST_ASSERT(after.regions <= before.regions + 1,
                "every region the blocks grew the heap by went back but the one kept in hand");
    TEST_ASSERT(after.region_bytes < grown.region_bytes,
                "and the pages of those regions went back to the page allocator");
out:
    StandHeapGiveChain(&chain);
    KHeapScopeLeave(outer);
    return verdict;
}

/* One piece of 1 MiB and 100 bytes, and one a page longer than the buddy's
 * largest block: each exactly the pages it needs, the second a run of blocks
 * side by side, every byte of both the caller's, and every page back. */
TestResult TestStandHeapLargeTakesPagesAndGivesBack(void)
{
    const size_t small_bytes = (1u << 20) + 100u;
    const size_t long_bytes  = pmm_max_alloc_pages() * VMM_PAGE_SIZE + VMM_PAGE_SIZE;
    const size_t head        = 64u + sizeof(uint64_t);    /* KHeapLargeHead + the end canary */
    const size_t small_pages = (small_bytes + head + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
    const size_t long_pages  = (long_bytes + head + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;

    TestResult verdict = TEST_PASS;
    KHeapScope outer   = KHeapScopeEnter(KHEAP_CHARGE_KERNEL, 0);
    KHeapShape s0, s1, s2, s3;
    uint8_t   *small   = NULL;
    uint8_t   *longer  = NULL;

    KHeapShapeNow(&s0);
    small = kmalloc(small_bytes);
    KHeapShapeNow(&s1);
    TEST_ASSERT(small != NULL, "a piece of 1 MiB and 100 bytes is served");
    TEST_ASSERT_EQ(s1.large, s0.large + 1, "as a large piece of its own");
    TEST_ASSERT_EQ(s1.large_bytes - s0.large_bytes, small_pages * VMM_PAGE_SIZE,
                   "of exactly the pages it needs, not the power of two above them");
    memset(small, 0xA5, small_bytes);

    longer = kmalloc(long_bytes);
    KHeapShapeNow(&s2);
    TEST_ASSERT(longer != NULL,
                "a piece a page longer than the buddy's largest block is served from a run of them");
    TEST_ASSERT_EQ(s2.large_bytes - s1.large_bytes, long_pages * VMM_PAGE_SIZE,
                   "of exactly the pages it needs");
    memset(longer, 0x5A, long_bytes);

    kfree(longer);
    longer = NULL;
    kfree(small);
    small = NULL;
    KHeapShapeNow(&s3);
    kprintf("[STAND] heap: large pieces of %zu and %zu bytes took %zu and %zu page(s) "
            "and gave them back (%zu KB in large pieces before, %zu after)\n",
            small_bytes, long_bytes, small_pages, long_pages,
            s0.large_bytes / 1024, s3.large_bytes / 1024);
    TEST_ASSERT_EQ(s3.large, s0.large, "both pieces are gone");
    TEST_ASSERT_EQ(s3.large_bytes, s0.large_bytes, "and every page of them went back");
out:
    kfree(longer);
    kfree(small);
    KHeapScopeLeave(outer);
    return verdict;
}

/* Cabins with no authority key made to hold one buddy block at a time until
 * they are refused; the refusal must be the share rule's, said once; the
 * reserve must be untouched by them; and while they stand refused, one of the
 * kernel's own paths is served — from the reserve. That class, because a
 * cabin without an authority key is refused every op but the unrestricted
 * ones (manifest_auth.h): pushing it to its share starves no program running
 * beside the test, and leaves the refusals the test counts its own. Each piece
 * is exactly one block of the largest order with its head, so no piece leaves
 * a tail of a block that no later piece could use, and the share rule — not
 * the buddy's supply of large blocks — is what stops them. */
TestResult TestStandHeapClassRefusedReserveServes(void)
{
    const size_t head  = 64u + sizeof(uint64_t);    /* KHeapLargeHead + the end canary */
    const size_t chunk = pmm_max_alloc_pages() * VMM_PAGE_SIZE - head;
    const size_t most  = pmm_get_total_ram_bytes() / chunk + 1;

    TestResult verdict  = TEST_PASS;
    KHeapScope outer    = KHeapScopeEnter(KHEAP_CHARGE_OTHER, 0);
    KHeapShape r0, r1, r2;
    void      *chain    = NULL;
    void      *kept     = NULL;
    void      *again    = NULL;
    void      *served   = NULL;
    size_t     chunks   = 0;
    uint64_t   refused0 = KHeapChargeRefusals(KHEAP_CHARGE_OTHER);
    size_t     held0    = KHeapChargeHeld(KHEAP_CHARGE_OTHER);
    size_t     held     = 0;

    KHeapShapeNow(&r0);
    while (chunks < most) {
        void **block = kmalloc(chunk);
        if (!block)
            break;
        *block = chain;
        chain  = block;
        chunks++;
    }
    held = KHeapChargeHeld(KHEAP_CHARGE_OTHER);
    KHeapShapeNow(&r1);
    TEST_ASSERT(chunks < most,
                "cabins with no authority key were refused before they held the machine");
    TEST_ASSERT_EQ(KHeapChargeRefusals(KHEAP_CHARGE_OTHER), refused0 + 1,
                   "refused by the share rule, once");
    TEST_ASSERT(r1.reserve_bytes == r0.reserve_bytes && r1.reserve_live == r0.reserve_live,
                "and nothing they were given came from the reserve");

    KHeapReserveOpen();
    kept = kmalloc(TAGFS_BLOCK_SIZE);
    KHeapReserveClose();
    KHeapShapeNow(&r2);
    TEST_ASSERT(kept != NULL, "while they stand refused, a kernel path is served");
    TEST_ASSERT(r2.reserve_live >= r1.reserve_live + TAGFS_BLOCK_SIZE, "and served from the reserve");

    again = kmalloc(chunk);
    TEST_ASSERT(again == NULL, "the class is still refused");
    TEST_ASSERT_EQ(KHeapChargeRefusals(KHEAP_CHARGE_OTHER), refused0 + 2,
                   "and the refusal is counted");

    kprintf("[STAND] heap: cabins with no authority key held %zu KB in %zu piece(s) of "
            "%zu KB when they were refused, with %zu KB free; the reserve served a "
            "kernel path %zu bytes and holds %zu KB\n",
            (held - held0) / 1024, chunks, chunk / 1024,
            PmmFreePagesNow() * VMM_PAGE_SIZE / 1024, (size_t)TAGFS_BLOCK_SIZE,
            r2.reserve_bytes / 1024);

    StandHeapGiveChain(&chain);
    TEST_ASSERT(KHeapChargeHeld(KHEAP_CHARGE_OTHER) < held0 + chunk,
                "giving the pieces back gives the class its room back");
    served = kmalloc(TAGFS_BLOCK_SIZE);
    TEST_ASSERT(served != NULL, "and they are served again");
out:
    kfree(served);
    kfree(again);
    kfree(kept);
    StandHeapGiveChain(&chain);
    KHeapScopeLeave(outer);
    return verdict;
}

#endif /* CONFIG_VOLUME_TESTS */
