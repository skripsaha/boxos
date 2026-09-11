
#include "memtag.h"
#include "pmm.h"
#include "vmm.h"
#include "klib.h"
#include "mce.h"
#include "iommu.h"
#include "cpuid.h"

static MemTagResult s_res;

#define MT_CHECK(cond, label) \
    do { if (cond) { pass++; } \
         else { fail++; kprintf("[MEMTAG TEST]   %[R]FAIL%[D]: " label "\n"); } \
    } while (0)

static __attribute__((noinline)) size_t CountAnd1(const char *a) {
    const char *tags[2] = { a, NULL };
    static MemTagResult r;
    MemTagQueryAndInto_(tags, &r);
    return r.count;
}

static __attribute__((noinline)) size_t CountAnd2(const char *a, const char *b) {
    const char *tags[3] = { a, b, NULL };
    static MemTagResult r;
    MemTagQueryAndInto_(tags, &r);
    return r.count;
}

static bool ResultContainsRegionForPhys(MemTagResult *r, uintptr_t phys) {
    uint32_t target = MemRegionFromPhys(phys);
    if (target == MEMTAG_INVALID_REGION_ID) return false;
    for (size_t i = 0; i < r->count; i++) {
        if (r->region_ids[i] == target) return true;
    }
    return false;
}

void MemTagStressTest(void) {
    kprintf("[MEMTAG TEST] Starting MemTag stress test (TagFS-shaped)...\n");
    size_t pass = 0, fail = 0;

    if (!MemTagIsInitialized()) {
        kprintf("[MEMTAG TEST] %[R]FAIL%[D]: MemTag not initialized\n");
        return;
    }

    kprintf("[MEMTAG TEST] Phase 1: tag intern\n");

    uint16_t t1a = MemTagInternStr("test:phase1");
    MT_CHECK(t1a != MEMTAG_INVALID_TAG_ID, "intern test:phase1");

    uint16_t t1b = MemTagInternStr("test:phase1");
    MT_CHECK(t1b == t1a, "intern idempotent (same id on second call)");

    uint16_t t1c = MemTagResolveStr("test:phase1");
    MT_CHECK(t1c == t1a, "resolve returns interned id");

    uint16_t t1miss = MemTagResolveStr("test:never-interned");
    MT_CHECK(t1miss == MEMTAG_INVALID_TAG_ID, "resolve unknown → INVALID");

    uint16_t t1key = MemTagIntern("test", NULL);
    MT_CHECK(t1key != MEMTAG_INVALID_TAG_ID, "intern key-only");
    MT_CHECK(MemTagKey(t1key) && strcmp(MemTagKey(t1key), "test") == 0,
             "MemTagKey returns 'test'");
    MT_CHECK(MemTagValue(t1key) == NULL, "MemTagValue is NULL for key-only");

    kprintf("[MEMTAG TEST] Phase 2: pmm_alloc creates region\n");

    void *p2 = MemTagPmmAlloc(1, "test:p2");
    MT_CHECK(p2 != NULL, "pmm_alloc(1, test:p2) non-NULL");
    uint32_t r2 = MemRegionFromPhys((uintptr_t)p2);
    MT_CHECK(r2 != MEMTAG_INVALID_REGION_ID, "MemRegionFromPhys(p2) found");
    MT_CHECK(MemRegionHasTagStr(r2, "test:p2"), "region has tag test:p2");
    MT_CHECK(CountAnd1("test:p2") >= 1, "AND(test:p2) count >= 1");

    kprintf("[MEMTAG TEST] Phase 3: multi-tag\n");

    void *p3 = pmm_alloc(1);
    MT_CHECK(p3 != NULL, "p3 alloc");
    uint32_t r3 = MemRegionCreate((uintptr_t)p3, 0, NULL, 1,
                                   MEMTAG_REGION_FLAG_PHYSICAL);
    MT_CHECK(r3 != MEMTAG_INVALID_REGION_ID, "MemRegionCreate r3");

    const char *r3_tags[] = { "test:a", "test:b", "test:shared", NULL };
    error_t apply_e = MemTagApplyMany(r3, r3_tags);
    MT_CHECK(apply_e == OK, "MemTagApplyMany ok");

    MT_CHECK(MemRegionHasTagStr(r3, "test:a"), "r3 has test:a");
    MT_CHECK(MemRegionHasTagStr(r3, "test:b"), "r3 has test:b");
    MT_CHECK(MemRegionHasTagStr(r3, "test:shared"), "r3 has test:shared");
    MT_CHECK(!MemRegionHasTagStr(r3, "test:not-applied"), "r3 lacks unapplied");

    MT_CHECK(CountAnd2("test:a", "test:b") >= 1,
             "AND(test:a, test:b) count >= 1");
    MT_CHECK(CountAnd2("test:a", "test:never-interned") == 0,
             "AND(test:a, unknown) == 0 (unknown tag kills AND)");

    kprintf("[MEMTAG TEST] Phase 4: OR query\n");

    void *p4a = MemTagPmmAlloc(1, "test:or_x");
    void *p4b = MemTagPmmAlloc(1, "test:or_y");
    MT_CHECK(p4a && p4b, "p4a + p4b alloc");

    if (p4a && p4b) {
        MemTagOrInto(&s_res, "test:or_x", "test:or_y");
        MT_CHECK(s_res.count >= 2, "OR(or_x, or_y) count >= 2");
        MT_CHECK(ResultContainsRegionForPhys(&s_res, (uintptr_t)p4a),
                 "OR result contains p4a's region");
        MT_CHECK(ResultContainsRegionForPhys(&s_res, (uintptr_t)p4b),
                 "OR result contains p4b's region");
    }

    kprintf("[MEMTAG TEST] Phase 5: mixed query\n");

    void *p5a = pmm_alloc(1);
    void *p5b = pmm_alloc(1);
    void *p5c = pmm_alloc(1);

    uint32_t r5a = MemRegionCreate((uintptr_t)p5a, 0, NULL, 1,
                                    MEMTAG_REGION_FLAG_PHYSICAL);
    uint32_t r5b = MemRegionCreate((uintptr_t)p5b, 0, NULL, 1,
                                    MEMTAG_REGION_FLAG_PHYSICAL);
    uint32_t r5c = MemRegionCreate((uintptr_t)p5c, 0, NULL, 1,
                                    MEMTAG_REGION_FLAG_PHYSICAL);

    MemTagApply(r5a, "test:shared2");
    MemTagApply(r5a, "test:a2");
    MemTagApply(r5b, "test:shared2");
    MemTagApply(r5b, "test:b2");
    MemTagApply(r5c, "test:shared2");
    MemTagApply(r5c, "test:a2");
    MemTagApply(r5c, "test:excluded5");

    const char *req[]  = { "test:shared2", NULL };
    const char *any_[] = { "test:a2", "test:b2", NULL };
    const char *exc[]  = { "test:excluded5", NULL };
    MemTagQueryMixedInto_(req, any_, exc, &s_res);

    MT_CHECK(s_res.count >= 2, "mixed query count >= 2");
    MT_CHECK(ResultContainsRegionForPhys(&s_res, (uintptr_t)p5a),
             "mixed: p5a present (shared2 ∧ a2 ∧ !excluded)");
    MT_CHECK(ResultContainsRegionForPhys(&s_res, (uintptr_t)p5b),
             "mixed: p5b present (shared2 ∧ b2 ∧ !excluded)");
    MT_CHECK(!ResultContainsRegionForPhys(&s_res, (uintptr_t)p5c),
             "mixed: p5c absent (excluded5 filtered)");

    kprintf("[MEMTAG TEST] Phase 6: clear + re-add\n");

    size_t before = CountAnd1("test:a");
    error_t ce = MemTagClear(r3, "test:a");
    MT_CHECK(ce == OK, "clear test:a from r3 ok");
    size_t after = CountAnd1("test:a");
    MT_CHECK(after == before - 1, "AND(test:a) count drops by 1 after clear");
    MT_CHECK(!MemRegionHasTagStr(r3, "test:a"),
             "r3 no longer has test:a");
    MT_CHECK(MemRegionHasTagStr(r3, "test:b"),
             "r3 still has test:b (only test:a cleared)");

    MemTagApply(r3, "test:a");
    MT_CHECK(CountAnd1("test:a") == before, "re-apply restores count");

    kprintf("[MEMTAG TEST] Phase 7: boot-seeded zones queryable\n");

    MemTagAndInto(&s_res, "zone:dma32");
    MT_CHECK(s_res.count >= 1, "AND(zone:dma32) finds boot-seeded zone");

    MemTagAndInto(&s_res, "zone:user");
    if (s_res.count == 0) {
        kprintf("[MEMTAG TEST]   note: zone:user empty (small RAM config)\n");
    }

    MemTagAndInto(&s_res, "purpose:mmio");
    MT_CHECK(s_res.count >= 1, "AND(purpose:mmio) finds at least one MMIO region");

    MemTagAndInto(&s_res, "purpose:kernel");
    MT_CHECK(s_res.count >= 1, "AND(purpose:kernel) finds kernel image");

    kprintf("[MEMTAG TEST] Phase 8: query cache\n");

    MemTagStats stats_before;
    MemTagGetStats(&stats_before);

    for (int i = 0; i < 5; i++) (void)MemTagAnd("zone:dma32");

    MemTagStats stats_after;
    MemTagGetStats(&stats_after);

    uint64_t new_hits   = stats_after.cache_hits   - stats_before.cache_hits;
    uint64_t new_misses = stats_after.cache_misses - stats_before.cache_misses;
    MT_CHECK(new_hits >= 4, "cache: 4+ hits on repeated query");
    MT_CHECK(new_misses <= 1, "cache: at most 1 miss on first query");

    kprintf("[MEMTAG TEST] Phase 9: cache invalidation\n");

    MemTagAnd("test:cache_inv_x");
    void *pinv = pmm_alloc(1);
    uint32_t rinv = MemRegionCreate((uintptr_t)pinv, 0, NULL, 1,
                                     MEMTAG_REGION_FLAG_PHYSICAL);
    MemTagApply(rinv, "test:cache_inv_x");

    MemTagAndInto(&s_res, "test:cache_inv_x");
    MT_CHECK(s_res.count >= 1,
             "post-mutation query sees new region (cache invalidated)");
    MT_CHECK(ResultContainsRegionForPhys(&s_res, (uintptr_t)pinv),
             "new region rinv present in re-query");

    kprintf("[MEMTAG TEST] Phase 10: bitmap word boundary\n");

    char buf[32];
    for (int i = 0; i < 70; i++) {
        ksnprintf(buf, sizeof(buf), "test:pad:%d", i);
        MemTagInternStr(buf);
    }
    void *pbnd = pmm_alloc(1);
    uint32_t rbnd = MemRegionCreate((uintptr_t)pbnd, 0, NULL, 1,
                                     MEMTAG_REGION_FLAG_PHYSICAL);
    MemTagApply(rbnd, "test:bnd_lo");
    MemTagApply(rbnd, "test:bnd_hi");

    MemTagAndInto(&s_res, "test:bnd_lo", "test:bnd_hi");
    MT_CHECK(s_res.count >= 1, "AND(bnd_lo, bnd_hi) >= 1");
    MT_CHECK(ResultContainsRegionForPhys(&s_res, (uintptr_t)pbnd),
             "pbnd in AND(bnd_lo, bnd_hi) result");

    kprintf("[MEMTAG TEST] Phase 11: PMM round-trip\n");

    void *prt = MemTagPmmAlloc(1, "test:roundtrip");
    MT_CHECK(prt != NULL, "rt alloc");
    MT_CHECK(CountAnd1("test:roundtrip") >= 1, "rt query before free");
    uintptr_t prt_phys = (uintptr_t)prt;
    pmm_free(prt, 1);
    MT_CHECK(MemRegionFromPhys(prt_phys) == MEMTAG_INVALID_REGION_ID,
             "after pmm_free: phys → INVALID region");

    kprintf("[MEMTAG TEST] Phase 12.5: NUMA derived tag (graceful if SRAT absent)\n");

    void *pnuma = MemTagPmmAlloc(1, "test:numa-probe");
    if (pnuma) {
        uint32_t rnuma = MemRegionFromPhys((uintptr_t)pnuma);
        uint32_t domain = pmm_phys_domain((uintptr_t)pnuma);
        if (domain != 0xFFFFFFFFu) {
            char expected[32];
            ksnprintf(expected, sizeof(expected), "numa:domain:%u", domain);
            MT_CHECK(MemRegionHasTagStr(rnuma, expected),
                     "NUMA tag auto-derived on alloc");
        } else {
            kprintf("[MEMTAG TEST]   SRAT not available (UMA) — NUMA tag skipped (OK)\n");
        }
        pmm_free(pnuma, 1);
    }

    kprintf("[MEMTAG TEST] Phase 12.6: MMIO derived cache:uc + purpose:mmio\n");

    volatile void *mmio_va = vmm_map_mmio(0xFEE00000UL, 4096, VMM_FLAGS_KERNEL_RW);
    if (mmio_va) {
        uint32_t rmmio = MemRegionFromPhys(0xFEE00000UL);
        if (rmmio != MEMTAG_INVALID_REGION_ID) {
            MT_CHECK(MemRegionHasTagStr(rmmio, "cache:uc"),
                     "MMIO region tagged cache:uc");
            MT_CHECK(MemRegionHasTagStr(rmmio, "purpose:mmio"),
                     "MMIO region tagged purpose:mmio");
        } else {
            MemTagResult mmio_q = MemTagAnd("cache:uc", "purpose:mmio");
            MT_CHECK(mmio_q.count >= 1,
                     "MMIO regions findable via tag bitmap (id_by_page miss is OK)");
        }
        vmm_unmap_mmio(mmio_va, 4096);
    }

    kprintf("[MEMTAG TEST] Phase 13: capability enforcement (guard/grant/revoke/check)\n");

    uint32_t test_pid = 0;
    uint16_t cap_tid  = MemTagInternStr("test:phase13:capability");
    MT_CHECK(cap_tid != MEMTAG_INVALID_TAG_ID, "intern test capability tag");

    void *pcap = MemTagPmmAlloc(1, "test:phase13:guarded-region");
    MT_CHECK(pcap != NULL, "alloc guarded-region");
    uint32_t rcap = MemRegionFromPhys((uintptr_t)pcap);
    MT_CHECK(rcap != MEMTAG_INVALID_REGION_ID, "find guarded-region");
    MT_CHECK(MemRegionAccessAllowed(test_pid, rcap),
             "default: no guards → access allowed");

    MemTagApply(rcap, "test:phase13:capability");
    MT_CHECK(MemRegionAccessAllowed(test_pid, rcap),
             "tag applied, not guarded → access allowed");

    error_t gerr = MemTagSetGuard("test:phase13:capability", true);
    MT_CHECK(gerr == OK, "MemTagSetGuard(ON) ok");
    MT_CHECK(MemTagIsGuard(cap_tid), "tag is guarded after SetGuard");
    MT_CHECK(!MemRegionAccessAllowed(test_pid, rcap),
             "guard ON, cap not held → access DENIED");
    MT_CHECK(MemRegionFirstMissingGuard(test_pid, rcap) == cap_tid,
             "missing-guard reports correct tag_id");

    error_t grerr = MemCabinGrant(test_pid, cap_tid);
    MT_CHECK(grerr == OK || grerr == ERR_OBJECT_NOT_FOUND,
             "MemCabinGrant returns ok or no-proc");
    if (grerr == OK) {
        MT_CHECK(MemCabinHolds(test_pid, cap_tid),
                 "MemCabinHolds returns true after grant");
        MT_CHECK(MemRegionAccessAllowed(test_pid, rcap),
                 "grant ON → access ALLOWED");

        MemCabinRevoke(test_pid, cap_tid);
        MT_CHECK(!MemCabinHolds(test_pid, cap_tid),
                 "MemCabinHolds returns false after revoke");
        MT_CHECK(!MemRegionAccessAllowed(test_pid, rcap),
                 "revoke → access DENIED");
    } else {
        kprintf("[MEMTAG TEST]   note: pid %u has no cabin — grant/revoke skipped (OK)\n",
                test_pid);
    }

    MemTagSetGuard("test:phase13:capability", false);
    MT_CHECK(!MemTagIsGuard(cap_tid), "guard OFF cleared correctly");

    if (pcap) pmm_free(pcap, 1);

    kprintf("[MEMTAG TEST] Phase 14: MemTagEnforce/EnforcePhys gates\n");

    MT_CHECK(MemTagEnforcePhys(0, 0xDEADBEEF000UL),
             "untracked phys → MemTagEnforcePhys returns true");

    void *p14a = MemTagPmmAlloc(1, "test:phase14:metadata-tag");
    uint32_t r14a = MemRegionFromPhys((uintptr_t)p14a);
    MT_CHECK(MemTagEnforce(0, 0, r14a),
             "no-guard region → MemTagEnforce returns true");
    MT_CHECK(MemTagEnforcePhys(0, (uintptr_t)p14a),
             "no-guard region via phys → MemTagEnforcePhys returns true");

    MemTagSetGuard("test:phase14:metadata-tag", true);
    MT_CHECK(!MemTagEnforce(0, 0, r14a),
             "guard set + pid invalid → MemTagEnforce returns false");
    MT_CHECK(!MemTagEnforcePhys(0, (uintptr_t)p14a),
             "guard set + pid invalid → MemTagEnforcePhys returns false");

    MT_CHECK(MemTagEnforce(0, 0, MEMTAG_INVALID_REGION_ID),
             "invalid region_id → returns true (no enforcement)");

    MemTagSetGuard("test:phase14:metadata-tag", false);
    if (p14a) pmm_free(p14a, 1);

    kprintf("[MEMTAG TEST] Phase 14C: attach/detach bookkeeping\n");
    {
        void *pat = MemTagPmmAlloc(1, "test:phase14c:attach-region");
        MT_CHECK(pat != NULL, "alloc attach-region");
        uint32_t rat = MemRegionFromPhys((uintptr_t)pat);
        MT_CHECK(rat != MEMTAG_INVALID_REGION_ID, "find attach-region");

        vmm_context_t *ctx_a = vmm_create_context();
        vmm_context_t *ctx_b = vmm_create_context();
        MT_CHECK(ctx_a != NULL && ctx_b != NULL, "two test contexts allocated");

        void *fake_ctx_a = (void *)ctx_a;
        void *fake_ctx_b = (void *)ctx_b;
        uintptr_t va1 = 0x40000000UL;
        uintptr_t va2 = 0x40400000UL;

        error_t e1 = MemRegionAttachCabin(rat, fake_ctx_a, va1, 1,
                                           MEMTAG_ATTACH_CLASS_4K,
                                           VMM_FLAGS_USER_RW | VMM_FLAG_NO_EXECUTE);
        MT_CHECK(e1 == OK, "attach #1 ok");
        error_t e2 = MemRegionAttachCabin(rat, fake_ctx_b, va2, 1,
                                           MEMTAG_ATTACH_CLASS_4K,
                                           VMM_FLAGS_USER_RW | VMM_FLAG_NO_EXECUTE);
        MT_CHECK(e2 == OK, "attach #2 ok");

        MemRegionAttach snap[8];
        size_t ns = MemRegionRegistrySnapshotAttachs(MemTagGetRegionRegistry(),
                                                      rat, snap, 8);
        MT_CHECK(ns == 2, "snapshot returns 2 attachments");

        bool seen_a = false, seen_b = false;
        for (size_t i = 0; i < ns; i++) {
            if (snap[i].ctx == fake_ctx_a && snap[i].va_base == va1) seen_a = true;
            if (snap[i].ctx == fake_ctx_b && snap[i].va_base == va2) seen_b = true;
            MT_CHECK(snap[i].state == MEMTAG_ATTACH_ACTIVE,
                     "fresh attach state == ACTIVE");
        }
        MT_CHECK(seen_a && seen_b, "snapshot contains both attaches");

        error_t e3 = MemRegionAttachCabin(rat, fake_ctx_a, va1, 1,
                                           MEMTAG_ATTACH_CLASS_4K,
                                           VMM_FLAGS_USER_RW | VMM_FLAG_NO_EXECUTE);
        MT_CHECK(e3 == OK, "idempotent re-attach ok");
        ns = MemRegionRegistrySnapshotAttachs(MemTagGetRegionRegistry(),
                                               rat, snap, 8);
        MT_CHECK(ns == 2, "snapshot still returns 2 after re-attach");

        MemRegionRegistrySetAttachState(MemTagGetRegionRegistry(), rat,
                                         fake_ctx_a, va1,
                                         MEMTAG_ATTACH_REVOKED);
        ns = MemRegionRegistrySnapshotAttachs(MemTagGetRegionRegistry(),
                                               rat, snap, 8);
        bool revoked_seen = false;
        for (size_t i = 0; i < ns; i++) {
            if (snap[i].ctx == fake_ctx_a && snap[i].state == MEMTAG_ATTACH_REVOKED)
                revoked_seen = true;
        }
        MT_CHECK(revoked_seen, "state mutation reflected in snapshot");

        error_t e4 = MemRegionDetachCabin(rat, fake_ctx_a, va1);
        MT_CHECK(e4 == OK, "single detach ok");
        ns = MemRegionRegistrySnapshotAttachs(MemTagGetRegionRegistry(),
                                               rat, snap, 8);
        MT_CHECK(ns == 1, "snapshot returns 1 after one detach");

        size_t detached = MemTagDetachAllForCabin(fake_ctx_b);
        MT_CHECK(detached >= 1, "bulk detach by ctx removes ≥1");
        ns = MemRegionRegistrySnapshotAttachs(MemTagGetRegionRegistry(),
                                               rat, snap, 8);
        MT_CHECK(ns == 0, "snapshot empty after bulk detach");

        error_t e5 = MemRegionDetachCabin(rat, fake_ctx_a, va1);
        MT_CHECK(e5 == OK, "idempotent detach (already gone) returns OK");

        if (pat) pmm_free(pat, 1);
        if (ctx_a) vmm_destroy_context(ctx_a);
        if (ctx_b) vmm_destroy_context(ctx_b);
    }

    kprintf("[MEMTAG TEST] Phase 14D: enforce post-ops no-crash on bogus pid\n");
    {
        uint16_t spook = MemTagInternStr("test:phase14d:guard");
        MT_CHECK(spook != MEMTAG_INVALID_TAG_ID, "intern phase14d guard tag");

        size_t swept_on  = MemTagSweepGuard(spook, true);
        size_t swept_off = MemTagSweepGuard(spook, false);
        MT_CHECK(swept_on == 0 && swept_off == 0,
                 "SweepGuard returns 0 when no regions match");

        size_t pr = MemCabinEnforceRevokePost(0xFFFFFFFFu, spook);
        size_t pg = MemCabinEnforceGrantPost(0xFFFFFFFFu, spook);
        MT_CHECK(pr == 0 && pg == 0,
                 "EnforceRevokePost / GrantPost no-op on bogus pid");
    }

    kprintf("[MEMTAG TEST] Phase 14E: MemRegionFromPte fast-path\n");
    {
        void *p14e = MemTagPmmAlloc(1, "test:phase14e:fast-path");
        MT_CHECK(p14e != NULL, "alloc 14e region");
        uint32_t r14e = MemRegionFromPhys((uintptr_t)p14e);
        MT_CHECK(r14e != MEMTAG_INVALID_REGION_ID, "find 14e region");

        if (r14e != MEMTAG_INVALID_REGION_ID) {
            uintptr_t phys_14e = (uintptr_t)p14e;
            uint64_t encoded = ((uint64_t)(r14e & MEMTAG_PTE_REGION_MAX))
                               << MEMTAG_PTE_REGION_SHIFT;

            uint64_t good_pte = phys_14e | encoded |
                                VMM_FLAG_PRESENT | VMM_FLAG_USER;
            uint32_t found = MemRegionFromPte(good_pte, phys_14e);
            MT_CHECK(found == r14e,
                     "MemRegionFromPte hits cache when encoded id covers phys");

            uint32_t wrong_id = (r14e + 1u) & MEMTAG_PTE_REGION_MAX;
            if (wrong_id != (r14e & MEMTAG_PTE_REGION_MAX)) {
                uint64_t wrong_encoded = ((uint64_t)wrong_id)
                                          << MEMTAG_PTE_REGION_SHIFT;
                uint64_t mismatch_pte = phys_14e | wrong_encoded |
                                         VMM_FLAG_PRESENT;
                uint32_t fallback = MemRegionFromPte(mismatch_pte, phys_14e);
                MT_CHECK(fallback == r14e,
                         "MemRegionFromPte falls back to phys index on cache miss");
            }
        }

        if (p14e) pmm_free(p14e, 1);
    }

    kprintf("[MEMTAG TEST] Phase 14F: MemRegionFromPte rejects bogus phys\n");
    {
        void *p14f = MemTagPmmAlloc(1, "test:phase14f:collide-base");
        MT_CHECK(p14f != NULL, "alloc 14f region");
        uint32_t r14f = MemRegionFromPhys((uintptr_t)p14f);
        MT_CHECK(r14f != MEMTAG_INVALID_REGION_ID, "find 14f region");

        if (r14f != MEMTAG_INVALID_REGION_ID) {
            uint64_t encoded = ((uint64_t)(r14f & MEMTAG_PTE_REGION_MAX))
                               << MEMTAG_PTE_REGION_SHIFT;
            uintptr_t bogus_phys = 0x1000000000000UL;
            uint64_t bogus_pte = bogus_phys | encoded | VMM_FLAG_PRESENT;
            uint32_t found = MemRegionFromPte(bogus_pte, bogus_phys);
            MT_CHECK(found == MEMTAG_INVALID_REGION_ID,
                     "MemRegionFromPte rejects mismatched phys (cache + fallback both miss)");
        }

        if (p14f) pmm_free(p14f, 1);
    }


    kprintf("[MEMTAG TEST] Phase 14H: derived cache tags on boot regions\n");
    {
        MemTagAndInto(&s_res, "zone:dma32", "cache:wb");
        MT_CHECK(s_res.count >= 1,
                 "zone:dma32 ∧ cache:wb finds boot zone region");

        MemTagAndInto(&s_res, "purpose:mmio", "cache:uc");
        MT_CHECK(s_res.count >= 1,
                 "purpose:mmio ∧ cache:uc finds MMIO region");

        MemTagAndInto(&s_res, "purpose:kernel", "cache:wb");
        MT_CHECK(s_res.count >= 1,
                 "purpose:kernel ∧ cache:wb finds kernel image region");
    }


    {
        MemTagAndInto(&s_res, "mce:poisoned");
        MT_CHECK(MemTagResolveStr("mce:poisoned") != MEMTAG_INVALID_TAG_ID,
                 "mce:poisoned reserved tag interned at boot");
    }

    {
        MT_CHECK(MemTagResolveStr("iommu:ready") != MEMTAG_INVALID_TAG_ID,
                 "iommu:ready reserved tag interned at boot");
        MT_CHECK(MemTagResolveStr("iommu:dma:mapped") != MEMTAG_INVALID_TAG_ID,
                 "iommu:dma:mapped reserved tag interned");
        MT_CHECK(MemTagResolveStr("iommu:domain:attached") != MEMTAG_INVALID_TAG_ID,
                 "iommu:domain:attached reserved tag interned");
    }

    kprintf("[MEMTAG TEST] Phase NS: hardware-derived reserved namespace\n");
    {
        MT_CHECK(MemTagResolveStr("cache:wb") != MEMTAG_INVALID_TAG_ID,
                 "cache:wb interned");
        MT_CHECK(MemTagResolveStr("cache:uc") != MEMTAG_INVALID_TAG_ID,
                 "cache:uc interned");
        MT_CHECK(MemTagResolveStr("cache:wc") != MEMTAG_INVALID_TAG_ID,
                 "cache:wc interned");
        MT_CHECK(MemTagResolveStr("cache:wt") != MEMTAG_INVALID_TAG_ID,
                 "cache:wt interned");

        MT_CHECK(MemTagResolveStr("pku:0")  != MEMTAG_INVALID_TAG_ID,
                 "pku:0 interned");
        MT_CHECK(MemTagResolveStr("pku:15") != MEMTAG_INVALID_TAG_ID,
                 "pku:15 interned");
        MT_CHECK(MemTagResolveStr("pku:fault:denied") != MEMTAG_INVALID_TAG_ID,
                 "pku:fault:denied interned");

        MT_CHECK(MemTagResolveStr("lam:ready") != MEMTAG_INVALID_TAG_ID,
                 "lam:ready interned");
        MT_CHECK(MemTagResolveStr("lam:fault:tag") != MEMTAG_INVALID_TAG_ID,
                 "lam:fault:tag interned");

        MT_CHECK(MemTagResolveStr("tme:ready") != MEMTAG_INVALID_TAG_ID,
                 "tme:ready interned");
        MT_CHECK(MemTagResolveStr("tme:mk_active") != MEMTAG_INVALID_TAG_ID,
                 "tme:mk_active interned");
        MT_CHECK(MemTagResolveStr("tme:keyid:0") != MEMTAG_INVALID_TAG_ID,
                 "tme:keyid:0 interned");
        MT_CHECK(MemTagResolveStr("tme:keyid:15") != MEMTAG_INVALID_TAG_ID,
                 "tme:keyid:15 interned");

        MT_CHECK(MemTagResolveStr("cet:ready") != MEMTAG_INVALID_TAG_ID,
                 "cet:ready interned");
        MT_CHECK(MemTagResolveStr("cet:shstk:supervisor") != MEMTAG_INVALID_TAG_ID,
                 "cet:shstk:supervisor interned");
        MT_CHECK(MemTagResolveStr("cet:fault:cp") != MEMTAG_INVALID_TAG_ID,
                 "cet:fault:cp interned");
    }

    kprintf("[MEMTAG TEST] Phase 12: stress 64 × 8 tags\n");

    #define STRESS_N    64
    #define STRESS_TAGS 8
    static const char *s_tag_strs[STRESS_TAGS] = {
        "stress:s0", "stress:s1", "stress:s2", "stress:s3",
        "stress:s4", "stress:s5", "stress:s6", "stress:s7"
    };
    static uintptr_t s_stress[STRESS_N];
    size_t n_alloc = 0;

    for (size_t i = 0; i < STRESS_N; i++) {
        void *sp = MemTagPmmAlloc(1, s_tag_strs[i % STRESS_TAGS]);
        if (!sp) break;
        s_stress[n_alloc++] = (uintptr_t)sp;
    }
    kprintf("[MEMTAG TEST]   stress: allocated %zu/%u pages\n", n_alloc, STRESS_N);

    if (n_alloc > 0) {
        size_t expected = n_alloc / STRESS_TAGS;
        for (size_t t = 0; t < STRESS_TAGS; t++) {
            size_t cnt = CountAnd1(s_tag_strs[t]);
            bool ok = (cnt >= expected - 1) && (cnt <= expected + 1);
            MT_CHECK(ok, "stress per-tag count within ±1");
        }
        const char *all8[STRESS_TAGS + 1];
        for (size_t t = 0; t < STRESS_TAGS; t++) all8[t] = s_tag_strs[t];
        all8[STRESS_TAGS] = NULL;
        MemTagQueryOrInto_(all8, &s_res);
        MT_CHECK(s_res.count >= n_alloc,
                 "OR of all 8 stress tags covers all allocations");
    }

    for (size_t i = 0; i < n_alloc; i++) {
        pmm_free((void *)s_stress[i], 1);
    }

    if (p2)    pmm_free(p2, 1);
    if (p3)    { MemRegionDestroy(r3);  pmm_free(p3, 1); }
    if (p4a)   pmm_free(p4a, 1);
    if (p4b)   pmm_free(p4b, 1);
    if (p5a)   { MemRegionDestroy(r5a); pmm_free(p5a, 1); }
    if (p5b)   { MemRegionDestroy(r5b); pmm_free(p5b, 1); }
    if (p5c)   { MemRegionDestroy(r5c); pmm_free(p5c, 1); }
    if (pinv)  { MemRegionDestroy(rinv); pmm_free(pinv, 1); }
    if (pbnd)  { MemRegionDestroy(rbnd); pmm_free(pbnd, 1); }

    MemTagStats final_stats;
    MemTagGetStats(&final_stats);
    kprintf("[MEMTAG TEST] Stats: tags=%u active_regs=%u/%u  reg_gen=%lu  bmp_gen=%lu  hits=%lu  misses=%lu\n",
            final_stats.tag_count, final_stats.region_active,
            final_stats.region_slot_count,
            (unsigned long)final_stats.registry_generation,
            (unsigned long)final_stats.bitmap_generation,
            (unsigned long)final_stats.cache_hits,
            (unsigned long)final_stats.cache_misses);

    if (fail == 0)
        kprintf("[MEMTAG TEST] %[S]PASSED%[D]: all %zu checks OK\n", pass);
    else
        kprintf("[MEMTAG TEST] %[R]FAILED%[D]: %zu pass, %zu fail\n", pass, fail);
}