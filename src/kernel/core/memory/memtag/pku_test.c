
#include "memtag.h"
#include "region_registry.h"
#include "vmm.h"
#include "pmm.h"
#include "klib.h"

#define PKU_CHECK(cond, label) \
    do { if (cond) { pass++; } \
         else { fail++; \
                kprintf("[PKU TEST]   %[R]FAIL%[D]: " label "\n"); } \
    } while (0)

static uint8_t pte_pkey_at(vmm_context_t *ctx, uintptr_t va) {
    uint8_t level = 0;
    pte_t *pte = vmm_get_leaf_pte(ctx, va, &level);
    if (!pte) return 0xFF;
    if (level != 1) return 0xFE;
    pte_t v = __atomic_load_n(pte, __ATOMIC_ACQUIRE);
    return vmm_pte_pkey(v);
}

void PkuStampTest(void) {
    kprintf("[PKU TEST] Starting PKU stamp test (Phase 2H+)...\n");
    size_t pass = 0, fail = 0;

    if (!MemTagIsInitialized()) {
        kprintf("[PKU TEST] %[Y]SKIPPED%[D]: MemTag not initialized\n");
        return;
    }

    {
        void *p1 = pmm_alloc(1, PHYS_TAG_USER);
        if (!p1) p1 = pmm_alloc(1);
        if (!p1) { kprintf("[PKU TEST] alloc failed; skipping T1\n"); }
        else {
            uintptr_t phys1 = (uintptr_t)p1;
            vmm_context_t *ctx1 = vmm_create_context();
            uintptr_t va1 = 0x40000000UL;
            uint32_t rid1 = MEMTAG_INVALID_REGION_ID;
            if (ctx1) {
                vmm_map_result_t mr = vmm_map_page(ctx1, va1, phys1,
                                                    VMM_FLAGS_USER_RW |
                                                    VMM_FLAG_NO_EXECUTE);
                PKU_CHECK(mr.success, "T1: vmm_map_page");
                rid1 = MemRegionCreate(phys1, va1, ctx1, 1,
                                        MEMTAG_REGION_FLAG_PHYSICAL |
                                        MEMTAG_REGION_FLAG_CABIN);
                PKU_CHECK(rid1 != MEMTAG_INVALID_REGION_ID, "T1: MemRegionCreate");

                error_t e = MemTagApply(rid1, "pku:5");
                PKU_CHECK(e == OK, "T1: MemTagApply(pku:5)");

                e = MemRegionAttachCabin(rid1, ctx1, va1, 1,
                                          MEMTAG_ATTACH_CLASS_4K,
                                          VMM_FLAGS_USER_RW |
                                          VMM_FLAG_NO_EXECUTE);
                PKU_CHECK(e == OK, "T1: AttachCabin");

                uint8_t pkey = pte_pkey_at(ctx1, va1);
                PKU_CHECK(pkey == 5, "T1: PTE.PKEY == 5 after attach");

                MemRegionDetachCabin(rid1, ctx1, va1);
                MemRegionDestroy(rid1);
                vmm_unmap_page(ctx1, va1);
                vmm_destroy_context(ctx1);
            }
            pmm_free(p1, 1);
        }
    }

    {
        void *p2 = pmm_alloc(1, PHYS_TAG_USER);
        if (!p2) p2 = pmm_alloc(1);
        if (p2) {
            uintptr_t phys2 = (uintptr_t)p2;
            vmm_context_t *ctx2 = vmm_create_context();
            uintptr_t va2 = 0x42000000UL;
            uint32_t rid2 = MEMTAG_INVALID_REGION_ID;
            if (ctx2) {
                vmm_map_result_t mr = vmm_map_page(ctx2, va2, phys2,
                                                    VMM_FLAGS_USER_RW |
                                                    VMM_FLAG_NO_EXECUTE);
                rid2 = MemRegionCreate(phys2, va2, ctx2, 1,
                                        MEMTAG_REGION_FLAG_PHYSICAL |
                                        MEMTAG_REGION_FLAG_CABIN);
                if (mr.success && rid2 != MEMTAG_INVALID_REGION_ID) {
                    (void)MemRegionAttachCabin(rid2, ctx2, va2, 1,
                                                MEMTAG_ATTACH_CLASS_4K,
                                                VMM_FLAGS_USER_RW |
                                                VMM_FLAG_NO_EXECUTE);
                    uint8_t pkey = pte_pkey_at(ctx2, va2);
                    PKU_CHECK(pkey == 0,
                              "T2: PTE.PKEY == 0 when region has no pku tag");
                    MemRegionDetachCabin(rid2, ctx2, va2);
                }
                if (rid2 != MEMTAG_INVALID_REGION_ID) MemRegionDestroy(rid2);
                vmm_unmap_page(ctx2, va2);
                vmm_destroy_context(ctx2);
            }
            pmm_free(p2, 1);
        }
    }

    {
        void *p3 = pmm_alloc(1, PHYS_TAG_USER);
        if (!p3) p3 = pmm_alloc(1);
        if (p3) {
            uintptr_t phys3 = (uintptr_t)p3;
            vmm_context_t *ctx3 = vmm_create_context();
            uintptr_t va3 = 0x44000000UL;
            uint32_t rid3 = MEMTAG_INVALID_REGION_ID;
            if (ctx3) {
                vmm_map_result_t mr = vmm_map_page(ctx3, va3, phys3,
                                                    VMM_FLAGS_USER_RW |
                                                    VMM_FLAG_NO_EXECUTE);
                rid3 = MemRegionCreate(phys3, va3, ctx3, 1,
                                        MEMTAG_REGION_FLAG_PHYSICAL |
                                        MEMTAG_REGION_FLAG_CABIN);
                if (mr.success && rid3 != MEMTAG_INVALID_REGION_ID) {
                    (void)MemRegionAttachCabin(rid3, ctx3, va3, 1,
                                                MEMTAG_ATTACH_CLASS_4K,
                                                VMM_FLAGS_USER_RW |
                                                VMM_FLAG_NO_EXECUTE);
                    PKU_CHECK(pte_pkey_at(ctx3, va3) == 0,
                              "T3 pre: PTE.PKEY == 0");

                    error_t e = MemTagApplyPkey(rid3, 7);
                    PKU_CHECK(e == OK, "T3: MemTagApplyPkey(7)");
                    PKU_CHECK(pte_pkey_at(ctx3, va3) == 7,
                              "T3: PTE.PKEY == 7 after sweep");
                    PKU_CHECK(MemRegionEffectivePkey(rid3) == 7,
                              "T3: EffectivePkey == 7");

                    e = MemTagApplyPkey(rid3, 0);
                    PKU_CHECK(e == OK, "T4: MemTagApplyPkey(0) clears");
                    PKU_CHECK(pte_pkey_at(ctx3, va3) == 0,
                              "T4: PTE.PKEY back to 0");
                    PKU_CHECK(MemRegionEffectivePkey(rid3) == 0,
                              "T4: EffectivePkey == 0");

                    e = MemTagApplyPkey(rid3, 3);
                    PKU_CHECK(e == OK && pte_pkey_at(ctx3, va3) == 3,
                              "T5 setup: pku:3 applied");
                    e = MemTagApplyPkey(rid3, 11);
                    PKU_CHECK(e == OK, "T5: MemTagApplyPkey(11) replace");
                    PKU_CHECK(pte_pkey_at(ctx3, va3) == 11,
                              "T5: PTE.PKEY == 11");
                    PKU_CHECK(MemRegionEffectivePkey(rid3) == 11,
                              "T5: only pku:11 remains (replace, not dup)");

                    MemRegionDetachCabin(rid3, ctx3, va3);
                }
                if (rid3 != MEMTAG_INVALID_REGION_ID) MemRegionDestroy(rid3);
                vmm_unmap_page(ctx3, va3);
                vmm_destroy_context(ctx3);
            }
            pmm_free(p3, 1);
        }
    }

    {
        void *p6 = pmm_alloc(1, PHYS_TAG_USER);
        if (!p6) p6 = pmm_alloc(1);
        if (p6) {
            uint32_t rid6 = MemRegionCreate((uintptr_t)p6, 0, NULL, 1,
                                             MEMTAG_REGION_FLAG_PHYSICAL);
            if (rid6 != MEMTAG_INVALID_REGION_ID) {
                error_t e = MemTagApplyPkey(rid6, 16);
                PKU_CHECK(e == ERR_INVALID_ARGUMENT,
                          "T6: pkey=16 rejected");
                e = MemTagApplyPkey(rid6, 255);
                PKU_CHECK(e == ERR_INVALID_ARGUMENT,
                          "T6: pkey=255 rejected");
                MemRegionDestroy(rid6);
            }
            pmm_free(p6, 1);
        }
    }

    if (fail == 0)
        kprintf("[PKU TEST] %[S]PASSED%[D]: all %zu checks OK\n", pass);
    else
        kprintf("[PKU TEST] %[R]FAILED%[D]: %zu pass, %zu fail\n", pass, fail);
}