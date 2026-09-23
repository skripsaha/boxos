/*
 * PKU PTE-stamping integration test (Phase 2H+)
 *
 * Exercises tag-driven PKU stamping end-to-end on real vmm_context +
 * MemRegion attaches. Cannot run a true userspace WRPKRU (we're in
 * kernel context) but verifies the kernel-side state machine the
 * PKRU enforcement depends on:
 *
 *   T1. Attach with pre-applied pku:5 → PTE bits 62:59 stamped to 5
 *   T2. Attach without pku tag → PTE bits 62:59 stay at 0
 *   T3. MemTagApplyPkey(rid, 7) on a live attach → sweep updates PTE
 *   T4. MemTagApplyPkey(rid, 0) clears existing pku tag, PTE → 0
 *   T5. MemTagApplyPkey replace (pku:3 → pku:11) — old tag dropped
 *   T6. MemTagApplyPkey rejects a pkey outside 0..15
 *   S1. CR4.PKE is up on a CPU that reports protection keys — the only
 *       question here the processor answers, and the one that decides
 *       whether T1..T6 describe enforcement or only bookkeeping. On a CPU
 *       without protection keys it is counted as unrun and the summary
 *       says so, because six green scenarios about bits nothing reads are
 *       not a working feature.
 *
 * Builds on Phase 2D test patterns (memtag_test.c Phase 14C).
 */

#include "memtag.h"
#include "region_registry.h"
#include "vmm.h"
#include "pmm.h"
#include "klib.h"
#include "cpuid.h"

#define PKU_CHECK(cond, label) \
    do { if (cond) { pass++; } \
         else { fail++; \
                kprintf("[PKU TEST]   %[R]FAIL%[D]: " label "\n"); } \
    } while (0)

/* The one check here the PROCESSOR answers rather than the kernel's own
 * bookkeeping — and the one a CPU without protection keys cannot be asked.
 * On such a CPU it is counted as unrun; it does not quietly vanish into a
 * total that then reads as green proof of a feature the silicon does not
 * have. */
#define PKU_CHECK_SILICON(cond, label) \
    do { if (!silicon) { unrun++; } \
         else if (cond) { pass++; } \
         else { fail++; \
                kprintf("[PKU TEST]   %[R]FAIL%[D]: " label "\n"); } \
    } while (0)

/* CR4.PKE — Intel SDM Vol 3A §2.5 "Control Registers", which lays CR4 out in
 * Figure 2-7 and names this one "Enable protection keys for user-mode pages
 * (bit 22 of CR4)". (Table 2-2 stood here and is a different table
 * altogether: x87 FPU behaviour for combinations of EM, MP and TS.) It is
 * the whole difference between a stamp that is enforced and a stamp that is
 * decoration: with PKE clear, §4.6.2 says "the protection key of a user-mode
 * address does not control data accesses to the address", and the four bits
 * every check below writes into a page table are ignored by the paging
 * hardware (Vol 3A §4.5, bits 62:59 of a leaf entry: "if CR4.PKE = 1 or
 * CR4.PKS = 1, this may control the page's access rights ...; otherwise, it
 * is ignored"). vmm_pku_init sets the bit on every CPU that reports PKU;
 * reading it back is how this test knows rather than assumes. */
#define PKU_CR4_PKE_BIT   (1ULL << 22)

static uint64_t pku_read_cr4(void) {
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    return cr4;
}

static uint8_t pte_pkey_at(vmm_context_t *ctx, uintptr_t va) {
    uint8_t level = 0;
    pte_t *pte = vmm_get_leaf_pte(ctx, va, &level);
    if (!pte) return 0xFF;     /* sentinel — no PTE */
    if (level != 1) return 0xFE;
    pte_t v = __atomic_load_n(pte, __ATOMIC_ACQUIRE);
    return vmm_pte_pkey(v);
}

void PkuStampTest(void) {
    kprintf("[PKU TEST] stamping protection keys into page tables this test "
            "builds itself: what follows measures whether this kernel writes "
            "the bits its tags call for, which is a different question from "
            "whether the processor under it enforces them\n");
    size_t pass = 0, fail = 0, unrun = 0;
    const bool silicon = g_cpu_caps.has_pku;

    if (!MemTagIsInitialized()) {
        kprintf("[PKU TEST] %[Y]SKIPPED%[D]: MemTag not initialized\n");
        return;
    }

    /* ── T1: pre-applied pku:5 → auto-stamped at attach ─────────── */
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

                /* Apply pku:5 BEFORE attach — auto-stamp in AttachCabin. */
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

    /* ── T2: no pku tag → PTE.PKEY == 0 ─────────────────────────── */
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

    /* ── T3: MemTagApplyPkey on live attach → sweep updates PTE ─── */
    /* ── T4: pkey == 0 clears existing tag → PTE.PKEY = 0 ─────────── */
    /* ── T5: replace pku:3 → pku:11 ─────────────────────────────── */
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

                    /* Apply pku:7 — should sweep + stamp. */
                    error_t e = MemTagApplyPkey(rid3, 7);
                    PKU_CHECK(e == OK, "T3: MemTagApplyPkey(7)");
                    PKU_CHECK(pte_pkey_at(ctx3, va3) == 7,
                              "T3: PTE.PKEY == 7 after sweep");
                    PKU_CHECK(MemRegionEffectivePkey(rid3) == 7,
                              "T3: EffectivePkey == 7");

                    /* T4: clear by pkey=0. */
                    e = MemTagApplyPkey(rid3, 0);
                    PKU_CHECK(e == OK, "T4: MemTagApplyPkey(0) clears");
                    PKU_CHECK(pte_pkey_at(ctx3, va3) == 0,
                              "T4: PTE.PKEY back to 0");
                    PKU_CHECK(MemRegionEffectivePkey(rid3) == 0,
                              "T4: EffectivePkey == 0");

                    /* T5: apply pku:3, then replace with pku:11. */
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

    /* ── T6: pkey out-of-range rejected ─────────────────────────── */
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

    /* ── S1: the silicon that decides whether any of the above matters ── */
    PKU_CHECK_SILICON((pku_read_cr4() & PKU_CR4_PKE_BIT) != 0,
                      "S1: CR4.PKE set on a CPU that reports protection keys");

    if (fail > 0) {
        kprintf("[PKU TEST] %[R]FAILED%[D]: %zu answered, %zu failed, %zu not "
                "runnable on this processor\n", pass, fail, unrun);
    } else if (silicon) {
        kprintf("[PKU TEST] %[S]PASSED%[D]: all %zu checks OK, and CR4.PKE is "
                "up on this processor — the four bits each of them stamped "
                "are the ones the MMU reads on every access to those pages\n",
                pass);
    } else {
        kprintf("[PKU TEST] %[Y]SKIPPED%[D]: this processor has no protection "
                "keys, so the %zu check(s) that answered prove only that the "
                "kernel writes bits 62:59 where its tags say; the paging "
                "hardware here ignores those bits, %zu check(s) that only "
                "silicon can answer were not run, and nothing on this machine "
                "was denied access by a key\n", pass, unrun);
    }
}
