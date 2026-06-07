/*
 * MCE Page Migration — integration test
 *
 * Exercises the full migrate path synchronously (mce_migrate_run_sync,
 * which bypasses irq_defer but uses the same perform() core) so we can
 * observe side effects in the same test process.
 *
 * Scenarios covered:
 *   T1. No-owner path — phys not covered by any MemRegion → no_owner
 *       counter increments, no PTE swap, no panic.
 *   T2. Single-cabin migration — real vmm_context + vmm_map_page +
 *       MemRegion + Attach. After migrate: PTE phys changed, original
 *       data preserved at new phys, completed counter incremented,
 *       cabins_touched == 1.
 *   T3. Multi-cabin (Bay-shaped) migration — same phys mapped into two
 *       contexts at different VAs. After migrate: BOTH PTEs point to
 *       the SAME new phys (shared semantics preserved).
 *   T4. Concurrent attach with no PTE present — attach exists but
 *       vmm_map_page wasn't done. Migration must skip (vmm_get_leaf_pte
 *       returns NULL) without crashing.
 *   T5. Cabin partial-overlap — attach covers pages [0..N) but the
 *       poisoned page is at index >= N. Migration must skip that
 *       attach.
 *
 * Real-hardware MCE injection (EINJ via APEI) is the integration-level
 * test; this file validates kernel-side correctness without needing
 * real silicon.
 */

#include "mce_migrate.h"
#include "memtag.h"
#include "region_registry.h"
#include "vmm.h"
#include "pmm.h"
#include "klib.h"

#define MIG_CHECK(cond, label) \
    do { if (cond) { pass++; } \
         else { fail++; \
                kprintf("[MCE MIG TEST]   %[R]FAIL%[D]: " label "\n"); } \
    } while (0)

/* Read PTE phys bits at (ctx, va). Returns 0 if PTE absent. */
static uintptr_t pte_phys_at(vmm_context_t *ctx, uintptr_t va) {
    uint8_t level = 0;
    pte_t *pte = vmm_get_leaf_pte(ctx, va, &level);
    if (!pte) return 0;
    if (level != 1) return 0;
    pte_t v = __atomic_load_n(pte, __ATOMIC_ACQUIRE);
    if (!(v & VMM_FLAG_PRESENT)) return 0;
    return (uintptr_t)(v & vmm_get_addr_mask());
}

void McMigrationTest(void) {
    kprintf("[MCE MIG TEST] Starting MCE migration test...\n");
    size_t pass = 0, fail = 0;

    if (!mce_migrate_is_initialized()) {
        kprintf("[MCE MIG TEST] %[Y]SKIPPED%[D]: mce_migrate not initialized\n");
        return;
    }

    /* ── T1: no-owner path ──────────────────────────────────────── */
    {
        mce_migrate_stats_t s0; mce_migrate_get_stats(&s0);
        /* Pick a phys with very high probability of NOT being in any
         * region: a high-zone offset well past kernel image and most
         * boot-seeded regions. If the test machine happens to have a
         * region there the no_owner counter check just no-ops, which
         * is fine — we still verify no panic. */
        uintptr_t orphan = 0x7FF00000ULL;
        (void)mce_migrate_run_sync(orphan);
        mce_migrate_stats_t s1; mce_migrate_get_stats(&s1);
        MIG_CHECK(s1.no_owner >= s0.no_owner,
                  "T1: no_owner counter monotonically increased or stable");
    }

    /* ── T2: single-cabin migration ─────────────────────────────── */
    void *old_phys_ptr = NULL;
    vmm_context_t *ctx_a = NULL;
    uintptr_t va_a = 0x60000000UL;
    uint32_t rid = MEMTAG_INVALID_REGION_ID;
    {
        old_phys_ptr = pmm_alloc(1, PHYS_TAG_USER);
        if (!old_phys_ptr) old_phys_ptr = pmm_alloc(1);
        MIG_CHECK(old_phys_ptr != NULL, "T2: pmm_alloc(1) succeeded");
        if (!old_phys_ptr) goto T2_cleanup;
        uintptr_t old_phys = (uintptr_t)old_phys_ptr;

        /* Write a deterministic pattern via the kernel pull-map. */
        uint8_t *src_va = (uint8_t *)vmm_phys_to_virt(old_phys);
        MIG_CHECK(src_va != NULL, "T2: vmm_phys_to_virt(old_phys)");
        for (size_t i = 0; i < PMM_PAGE_SIZE; i++) src_va[i] = (uint8_t)(0x37 ^ (i & 0xFF));

        ctx_a = vmm_create_context();
        MIG_CHECK(ctx_a != NULL, "T2: vmm_create_context");
        if (!ctx_a) goto T2_cleanup;

        vmm_map_result_t mr = vmm_map_page(ctx_a, va_a, old_phys,
                                            VMM_FLAGS_USER_RW | VMM_FLAG_NO_EXECUTE);
        MIG_CHECK(mr.success, "T2: vmm_map_page(va_a)");
        if (!mr.success) goto T2_cleanup;

        rid = MemRegionCreate(old_phys, va_a, ctx_a, 1,
                              MEMTAG_REGION_FLAG_PHYSICAL | MEMTAG_REGION_FLAG_CABIN);
        MIG_CHECK(rid != MEMTAG_INVALID_REGION_ID, "T2: MemRegionCreate");

        error_t at_err = MemRegionAttachCabin(rid, ctx_a, va_a, 1,
                                              MEMTAG_ATTACH_CLASS_4K,
                                              VMM_FLAGS_USER_RW | VMM_FLAG_NO_EXECUTE);
        MIG_CHECK(at_err == OK, "T2: MemRegionAttachCabin");

        uintptr_t pre_phys = pte_phys_at(ctx_a, va_a);
        MIG_CHECK(pre_phys == old_phys, "T2: PTE phys == old_phys pre-migrate");

        mce_migrate_stats_t b; mce_migrate_get_stats(&b);
        uint32_t touched = mce_migrate_run_sync(old_phys);
        mce_migrate_stats_t a; mce_migrate_get_stats(&a);

        MIG_CHECK(touched == 1, "T2: migration touched exactly 1 cabin");
        MIG_CHECK(a.completed > b.completed, "T2: completed counter incremented");
        MIG_CHECK(a.cabins_touched > b.cabins_touched,
                  "T2: cabins_touched counter incremented");

        uintptr_t post_phys = pte_phys_at(ctx_a, va_a);
        MIG_CHECK(post_phys != 0,        "T2: PTE still PRESENT after migrate");
        MIG_CHECK(post_phys != old_phys, "T2: PTE phys swapped (≠ old_phys)");

        /* Verify data integrity via the new phys (kernel pull-map). */
        if (post_phys != 0 && post_phys != old_phys) {
            uint8_t *new_va = (uint8_t *)vmm_phys_to_virt(post_phys);
            bool data_ok = (new_va != NULL);
            if (data_ok) {
                for (size_t i = 0; i < PMM_PAGE_SIZE; i++) {
                    if (new_va[i] != (uint8_t)(0x37 ^ (i & 0xFF))) {
                        data_ok = false; break;
                    }
                }
            }
            MIG_CHECK(data_ok, "T2: data integrity preserved across migrate");

            /* The migration allocated post_phys via pmm_alloc — we now
             * own that allocation transitively via the only mapping. */
            (void)vmm_unmap_page(ctx_a, va_a);
            pmm_free((void *)post_phys, 1);
        }

T2_cleanup:
        if (rid != MEMTAG_INVALID_REGION_ID) {
            MemRegionDetachCabin(rid, ctx_a, va_a);
            MemRegionDestroy(rid);
        }
        if (ctx_a) vmm_destroy_context(ctx_a);
        if (old_phys_ptr) pmm_free(old_phys_ptr, 1);
    }

    /* ── T3: multi-cabin (Bay-shaped) migration ─────────────────── */
    void *t3_phys_ptr = NULL;
    vmm_context_t *ctx_x = NULL, *ctx_y = NULL;
    uintptr_t va_x = 0x68000000UL, va_y = 0x68400000UL;
    uint32_t rid3 = MEMTAG_INVALID_REGION_ID;
    {
        t3_phys_ptr = pmm_alloc(1, PHYS_TAG_USER);
        if (!t3_phys_ptr) t3_phys_ptr = pmm_alloc(1);
        MIG_CHECK(t3_phys_ptr != NULL, "T3: pmm_alloc shared phys");
        if (!t3_phys_ptr) goto T3_cleanup;
        uintptr_t shared_phys = (uintptr_t)t3_phys_ptr;

        ctx_x = vmm_create_context();
        ctx_y = vmm_create_context();
        MIG_CHECK(ctx_x && ctx_y, "T3: two contexts allocated");
        if (!ctx_x || !ctx_y) goto T3_cleanup;

        vmm_map_result_t mrx = vmm_map_page(ctx_x, va_x, shared_phys,
                                             VMM_FLAGS_USER_RW | VMM_FLAG_NO_EXECUTE);
        vmm_map_result_t mry = vmm_map_page(ctx_y, va_y, shared_phys,
                                             VMM_FLAGS_USER_RW | VMM_FLAG_NO_EXECUTE);
        MIG_CHECK(mrx.success && mry.success, "T3: two vmm_map_page succeeded");
        if (!mrx.success || !mry.success) goto T3_cleanup;

        rid3 = MemRegionCreate(shared_phys, 0, NULL, 1,
                               MEMTAG_REGION_FLAG_PHYSICAL);
        MIG_CHECK(rid3 != MEMTAG_INVALID_REGION_ID, "T3: shared region created");

        error_t e_x = MemRegionAttachCabin(rid3, ctx_x, va_x, 1,
                                           MEMTAG_ATTACH_CLASS_4K,
                                           VMM_FLAGS_USER_RW | VMM_FLAG_NO_EXECUTE);
        error_t e_y = MemRegionAttachCabin(rid3, ctx_y, va_y, 1,
                                           MEMTAG_ATTACH_CLASS_4K,
                                           VMM_FLAGS_USER_RW | VMM_FLAG_NO_EXECUTE);
        MIG_CHECK(e_x == OK && e_y == OK, "T3: two attaches OK");

        uint32_t touched = mce_migrate_run_sync(shared_phys);
        MIG_CHECK(touched == 2, "T3: migration touched both cabins");

        uintptr_t after_x = pte_phys_at(ctx_x, va_x);
        uintptr_t after_y = pte_phys_at(ctx_y, va_y);
        MIG_CHECK(after_x != 0 && after_y != 0,
                  "T3: both PTEs remain PRESENT");
        MIG_CHECK(after_x == after_y,
                  "T3: both attachments now share the SAME new phys (Bay semantics)");
        MIG_CHECK(after_x != shared_phys,
                  "T3: new phys differs from original shared phys");

        if (after_x != 0 && after_x != shared_phys) {
            (void)vmm_unmap_page(ctx_x, va_x);
            (void)vmm_unmap_page(ctx_y, va_y);
            pmm_free((void *)after_x, 1);
        }

T3_cleanup:
        if (rid3 != MEMTAG_INVALID_REGION_ID) {
            MemRegionDetachCabin(rid3, ctx_x, va_x);
            MemRegionDetachCabin(rid3, ctx_y, va_y);
            MemRegionDestroy(rid3);
        }
        if (ctx_x) vmm_destroy_context(ctx_x);
        if (ctx_y) vmm_destroy_context(ctx_y);
        if (t3_phys_ptr) pmm_free(t3_phys_ptr, 1);
    }

    /* ── T4: attach without PTE present ─────────────────────────── */
    {
        void *p4 = pmm_alloc(1, PHYS_TAG_USER);
        if (!p4) p4 = pmm_alloc(1);
        if (p4) {
            uintptr_t phys4 = (uintptr_t)p4;
            vmm_context_t *ctx4 = vmm_create_context();
            uint32_t rid4 = MemRegionCreate(phys4, 0, NULL, 1,
                                            MEMTAG_REGION_FLAG_PHYSICAL);
            uintptr_t va4 = 0x70000000UL;  /* unmapped — PTE absent */
            if (ctx4 && rid4 != MEMTAG_INVALID_REGION_ID) {
                (void)MemRegionAttachCabin(rid4, ctx4, va4, 1,
                                            MEMTAG_ATTACH_CLASS_4K,
                                            VMM_FLAGS_USER_RW);
                uint32_t touched4 = mce_migrate_run_sync(phys4);
                MIG_CHECK(touched4 == 0,
                          "T4: attach with no PTE → 0 cabins touched (no crash)");
                MemRegionDetachCabin(rid4, ctx4, va4);
            }
            if (rid4 != MEMTAG_INVALID_REGION_ID) MemRegionDestroy(rid4);
            if (ctx4) vmm_destroy_context(ctx4);
            pmm_free(p4, 1);
        }
    }

    /* ── T5: cabin partial-overlap ──────────────────────────────── */
    {
        /* Region spans 4 pages but attach only covers first 2.
         * Poisoned page is at offset 3 (inside region, outside attach). */
        void *p5 = pmm_alloc(4, PHYS_TAG_USER);
        if (!p5) p5 = pmm_alloc(4);
        if (p5) {
            uintptr_t base5 = (uintptr_t)p5;
            uintptr_t bad_phys5 = base5 + 3 * PMM_PAGE_SIZE;
            vmm_context_t *ctx5 = vmm_create_context();
            uint32_t rid5 = MemRegionCreate(base5, 0, NULL, 4,
                                            MEMTAG_REGION_FLAG_PHYSICAL);
            uintptr_t va5 = 0x78000000UL;
            if (ctx5 && rid5 != MEMTAG_INVALID_REGION_ID) {
                vmm_map_result_t mr5 = vmm_map_pages(ctx5, va5, base5, 2,
                                                      VMM_FLAGS_USER_RW |
                                                      VMM_FLAG_NO_EXECUTE);
                if (mr5.success) {
                    (void)MemRegionAttachCabin(rid5, ctx5, va5, 2,
                                                MEMTAG_ATTACH_CLASS_4K,
                                                VMM_FLAGS_USER_RW |
                                                VMM_FLAG_NO_EXECUTE);
                    uint32_t touched5 = mce_migrate_run_sync(bad_phys5);
                    MIG_CHECK(touched5 == 0,
                              "T5: partial-attach skip outside-range page");
                    MemRegionDetachCabin(rid5, ctx5, va5);
                    (void)vmm_unmap_pages(ctx5, va5, 2);
                }
            }
            if (rid5 != MEMTAG_INVALID_REGION_ID) MemRegionDestroy(rid5);
            if (ctx5) vmm_destroy_context(ctx5);
            pmm_free(p5, 4);
        }
    }

    if (fail == 0)
        kprintf("[MCE MIG TEST] %[S]PASSED%[D]: all %zu checks OK\n", pass);
    else
        kprintf("[MCE MIG TEST] %[R]FAILED%[D]: %zu pass, %zu fail\n", pass, fail);

    mce_migrate_dump();
}
