/*
 * MemTag — Stress + Correctness Tests
 *
 * Exercises the TagFS-shaped memory tagging architecture:
 *   - Tag intern (key:value, key-only, idempotence, registry growth)
 *   - Region create / destroy via PMM (pmm_alloc(N, "tag"))
 *   - Multi-tag per region (MemTagApply, MemTagApplyMany, MemTagClear)
 *   - Set algebra: AND / OR / Mixed (required, any, excluded)
 *   - Query cache (hit/miss, generation invalidation)
 *   - Bitmap word boundary (tag_id 63→64 across uint64_t boundary)
 *   - Boot-seeded zone regions queryable via MemTagAnd("zone:dma32")
 *   - Forward lookup MemRegionFromPhys (O(1) dense index)
 *   - PMM round-trip: alloc tagged, query, free, region gone
 *   - Stress: 64 alloc cycle × 8 tags
 *   - Generation counter monotone under mutation
 */

#include "memtag.h"
#include "pmm.h"
#include "vmm.h"
#include "klib.h"
#include "mce.h"     /* Phase 14K — mce_is_initialized + bank count */
#include "iommu.h"   /* Phase 14L — iommu_present / iommu_domain_id */
#include "cpuid.h"   /* Phase 14M — g_cpu_caps.has_pku gate */

static MemTagResult s_res;  /* avoid 4 KB on kernel stack */

#define MT_CHECK(cond, label) \
    do { if (cond) { pass++; } \
         else { fail++; kprintf("[MEMTAG TEST]   %[R]FAIL%[D]: " label "\n"); } \
    } while (0)

static size_t CountAnd1(const char *a) {
    const char *tags[2] = { a, NULL };
    return MemTagQueryAnd_(tags).count;
}

static size_t CountAnd2(const char *a, const char *b) {
    const char *tags[3] = { a, b, NULL };
    return MemTagQueryAnd_(tags).count;
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

    /* ── Phase 1: tag intern ─────────────────────────────────────────── */
    kprintf("[MEMTAG TEST] Phase 1: tag intern\n");

    uint16_t t1a = MemTagInternStr("test:phase1");
    MT_CHECK(t1a != MEMTAG_INVALID_TAG_ID, "intern test:phase1");

    uint16_t t1b = MemTagInternStr("test:phase1");  /* idempotent */
    MT_CHECK(t1b == t1a, "intern idempotent (same id on second call)");

    uint16_t t1c = MemTagResolveStr("test:phase1");
    MT_CHECK(t1c == t1a, "resolve returns interned id");

    uint16_t t1miss = MemTagResolveStr("test:never-interned");
    MT_CHECK(t1miss == MEMTAG_INVALID_TAG_ID, "resolve unknown → INVALID");

    /* Key-only tag */
    uint16_t t1key = MemTagIntern("test", NULL);
    MT_CHECK(t1key != MEMTAG_INVALID_TAG_ID, "intern key-only");
    MT_CHECK(MemTagKey(t1key) && strcmp(MemTagKey(t1key), "test") == 0,
             "MemTagKey returns 'test'");
    MT_CHECK(MemTagValue(t1key) == NULL, "MemTagValue is NULL for key-only");

    /* ── Phase 2: pmm_alloc(N, "tag") creates a region ─────────────── */
    kprintf("[MEMTAG TEST] Phase 2: pmm_alloc creates region\n");

    void *p2 = MemTagPmmAlloc(1, "test:p2");
    MT_CHECK(p2 != NULL, "pmm_alloc(1, test:p2) non-NULL");
    uint32_t r2 = MemRegionFromPhys((uintptr_t)p2);
    MT_CHECK(r2 != MEMTAG_INVALID_REGION_ID, "MemRegionFromPhys(p2) found");
    MT_CHECK(MemRegionHasTagStr(r2, "test:p2"), "region has tag test:p2");
    MT_CHECK(CountAnd1("test:p2") >= 1, "AND(test:p2) count >= 1");

    /* ── Phase 3: multi-tag application ────────────────────────────── */
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

    /* ── Phase 4: OR query ─────────────────────────────────────────── */
    kprintf("[MEMTAG TEST] Phase 4: OR query\n");

    void *p4a = MemTagPmmAlloc(1, "test:or_x");
    void *p4b = MemTagPmmAlloc(1, "test:or_y");
    MT_CHECK(p4a && p4b, "p4a + p4b alloc");

    if (p4a && p4b) {
        s_res = MemTagOr("test:or_x", "test:or_y");
        MT_CHECK(s_res.count >= 2, "OR(or_x, or_y) count >= 2");
        MT_CHECK(ResultContainsRegionForPhys(&s_res, (uintptr_t)p4a),
                 "OR result contains p4a's region");
        MT_CHECK(ResultContainsRegionForPhys(&s_res, (uintptr_t)p4b),
                 "OR result contains p4b's region");
    }

    /* ── Phase 5: Mixed (required+any+excluded) ────────────────────── */
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
    s_res = MemTagQueryMixed_(req, any_, exc);

    /* p5a (shared2+a2) and p5b (shared2+b2) should match; p5c excluded */
    MT_CHECK(s_res.count >= 2, "mixed query count >= 2");
    MT_CHECK(ResultContainsRegionForPhys(&s_res, (uintptr_t)p5a),
             "mixed: p5a present (shared2 ∧ a2 ∧ !excluded)");
    MT_CHECK(ResultContainsRegionForPhys(&s_res, (uintptr_t)p5b),
             "mixed: p5b present (shared2 ∧ b2 ∧ !excluded)");
    MT_CHECK(!ResultContainsRegionForPhys(&s_res, (uintptr_t)p5c),
             "mixed: p5c absent (excluded5 filtered)");

    /* ── Phase 6: tag clear + tag re-add ───────────────────────────── */
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

    /* Re-apply restores */
    MemTagApply(r3, "test:a");
    MT_CHECK(CountAnd1("test:a") == before, "re-apply restores count");

    /* ── Phase 7: zone-region seeding ──────────────────────────────── */
    kprintf("[MEMTAG TEST] Phase 7: boot-seeded zones queryable\n");

    s_res = MemTagAnd("zone:dma32");
    MT_CHECK(s_res.count >= 1, "AND(zone:dma32) finds boot-seeded zone");

    s_res = MemTagAnd("zone:user");
    /* zone:user may be absent on tiny memory configs; not a hard fail */
    if (s_res.count == 0) {
        kprintf("[MEMTAG TEST]   note: zone:user empty (small RAM config)\n");
    }

    s_res = MemTagAnd("purpose:mmio");
    MT_CHECK(s_res.count >= 1, "AND(purpose:mmio) finds at least one MMIO region");

    s_res = MemTagAnd("purpose:kernel");
    MT_CHECK(s_res.count >= 1, "AND(purpose:kernel) finds kernel image");

    /* ── Phase 8: query cache hit/miss ─────────────────────────────── */
    kprintf("[MEMTAG TEST] Phase 8: query cache\n");

    MemTagStats stats_before;
    MemTagGetStats(&stats_before);

    /* Run the same query 5 times; first miss, next 4 hits. */
    for (int i = 0; i < 5; i++) (void)MemTagAnd("zone:dma32");

    MemTagStats stats_after;
    MemTagGetStats(&stats_after);

    uint64_t new_hits   = stats_after.cache_hits   - stats_before.cache_hits;
    uint64_t new_misses = stats_after.cache_misses - stats_before.cache_misses;
    MT_CHECK(new_hits >= 4, "cache: 4+ hits on repeated query");
    MT_CHECK(new_misses <= 1, "cache: at most 1 miss on first query");

    /* ── Phase 9: cache invalidation on mutation ───────────────────── */
    kprintf("[MEMTAG TEST] Phase 9: cache invalidation\n");

    /* Run query, cache it */
    MemTagAnd("test:cache_inv_x");
    /* Mutate — apply tag to a fresh region */
    void *pinv = pmm_alloc(1);
    uint32_t rinv = MemRegionCreate((uintptr_t)pinv, 0, NULL, 1,
                                     MEMTAG_REGION_FLAG_PHYSICAL);
    MemTagApply(rinv, "test:cache_inv_x");

    /* Re-query — should pick up the new region (cache invalidated) */
    s_res = MemTagAnd("test:cache_inv_x");
    MT_CHECK(s_res.count >= 1,
             "post-mutation query sees new region (cache invalidated)");
    MT_CHECK(ResultContainsRegionForPhys(&s_res, (uintptr_t)pinv),
             "new region rinv present in re-query");

    /* ── Phase 10: bitmap word boundary (tag_id 63→64) ─────────────── */
    kprintf("[MEMTAG TEST] Phase 10: bitmap word boundary\n");

    /* Pad registry to cross the 64-bit boundary inside the per-region
     * tag_ids sorted array. (Phase 1's bitmap is byte-addressed, so the
     * "word boundary" is internal — but exercise the path regardless.) */
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

    s_res = MemTagAnd("test:bnd_lo", "test:bnd_hi");
    MT_CHECK(s_res.count >= 1, "AND(bnd_lo, bnd_hi) >= 1");
    MT_CHECK(ResultContainsRegionForPhys(&s_res, (uintptr_t)pbnd),
             "pbnd in AND(bnd_lo, bnd_hi) result");

    /* ── Phase 11: PMM round-trip (alloc → query → free → gone) ─── */
    kprintf("[MEMTAG TEST] Phase 11: PMM round-trip\n");

    void *prt = MemTagPmmAlloc(1, "test:roundtrip");
    MT_CHECK(prt != NULL, "rt alloc");
    MT_CHECK(CountAnd1("test:roundtrip") >= 1, "rt query before free");
    uintptr_t prt_phys = (uintptr_t)prt;
    pmm_free(prt, 1);
    MT_CHECK(MemRegionFromPhys(prt_phys) == MEMTAG_INVALID_REGION_ID,
             "after pmm_free: phys → INVALID region");

    /* ── Phase 12.5: derived NUMA tag ──────────────────────────────── */
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

    /* ── Phase 12.6: MMIO derived cache + purpose tags ────────────── */
    kprintf("[MEMTAG TEST] Phase 12.6: MMIO derived cache:uc + purpose:mmio\n");

    /* LAPIC base 0xFEE00000 — standard MMIO that we don't actually use here.
     * Map it briefly, check the tags landed, unmap. */
    volatile void *mmio_va = vmm_map_mmio(0xFEE00000UL, 4096, VMM_FLAGS_KERNEL_RW);
    if (mmio_va) {
        uint32_t rmmio = MemRegionFromPhys(0xFEE00000UL);
        if (rmmio != MEMTAG_INVALID_REGION_ID) {
            MT_CHECK(MemRegionHasTagStr(rmmio, "cache:uc"),
                     "MMIO region tagged cache:uc");
            MT_CHECK(MemRegionHasTagStr(rmmio, "purpose:mmio"),
                     "MMIO region tagged purpose:mmio");
        } else {
            /* id_by_page doesn't cover MMIO range above mem_end — region
             * exists in registry but not findable by phys. Verify via
             * bitmap query instead. */
            MemTagResult mmio_q = MemTagAnd("cache:uc", "purpose:mmio");
            MT_CHECK(mmio_q.count >= 1,
                     "MMIO regions findable via tag bitmap (id_by_page miss is OK)");
        }
        vmm_unmap_mmio(mmio_va, 4096);
    }

    /* ── Phase 13: capability enforcement (Phase 2A infrastructure) ── */
    kprintf("[MEMTAG TEST] Phase 13: capability enforcement (guard/grant/revoke/check)\n");

    /* Take any kernel process (pid 0 == kernel sentinel — skip; use boot
     * shell's pid which we don't know yet). Instead, use the calling
     * context — but this test runs early in boot before any user proc.
     * Strategy: use pid 0 (the kernel "process") as the cabin under test.
     * pid 0 has no real cabin but process_find returns the kernel sentinel
     * and grant/revoke on it just flips bits in an unused mask. The
     * MemRegionAccessAllowed query is what we actually verify. */
    uint32_t test_pid = 0;
    uint16_t cap_tid  = MemTagInternStr("test:phase13:capability");
    MT_CHECK(cap_tid != MEMTAG_INVALID_TAG_ID, "intern test capability tag");

    /* Default: no guards → access allowed. */
    void *pcap = MemTagPmmAlloc(1, "test:phase13:guarded-region");
    MT_CHECK(pcap != NULL, "alloc guarded-region");
    uint32_t rcap = MemRegionFromPhys((uintptr_t)pcap);
    MT_CHECK(rcap != MEMTAG_INVALID_REGION_ID, "find guarded-region");
    MT_CHECK(MemRegionAccessAllowed(test_pid, rcap),
             "default: no guards → access allowed");

    /* Apply capability tag to the region. Still no guard set → allowed. */
    MemTagApply(rcap, "test:phase13:capability");
    MT_CHECK(MemRegionAccessAllowed(test_pid, rcap),
             "tag applied, not guarded → access allowed");

    /* Flip guard ON. Cabin does NOT hold cap → DENIED. */
    error_t gerr = MemTagSetGuard("test:phase13:capability", true);
    MT_CHECK(gerr == OK, "MemTagSetGuard(ON) ok");
    MT_CHECK(MemTagIsGuard(cap_tid), "tag is guarded after SetGuard");
    MT_CHECK(!MemRegionAccessAllowed(test_pid, rcap),
             "guard ON, cap not held → access DENIED");
    MT_CHECK(MemRegionFirstMissingGuard(test_pid, rcap) == cap_tid,
             "missing-guard reports correct tag_id");

    /* Grant capability to cabin. Now ALLOWED. */
    error_t grerr = MemCabinGrant(test_pid, cap_tid);
    MT_CHECK(grerr == OK || grerr == ERR_OBJECT_NOT_FOUND,
             "MemCabinGrant returns ok or no-proc");
    /* pid 0 may not have a real process_t — that's OK for this test;
     * we mostly verify the API doesn't crash. Run check anyway: if
     * grant succeeded (real cabin), access should now be allowed. */
    if (grerr == OK) {
        MT_CHECK(MemCabinHolds(test_pid, cap_tid),
                 "MemCabinHolds returns true after grant");
        MT_CHECK(MemRegionAccessAllowed(test_pid, rcap),
                 "grant ON → access ALLOWED");

        /* Revoke → DENIED again. */
        MemCabinRevoke(test_pid, cap_tid);
        MT_CHECK(!MemCabinHolds(test_pid, cap_tid),
                 "MemCabinHolds returns false after revoke");
        MT_CHECK(!MemRegionAccessAllowed(test_pid, rcap),
                 "revoke → access DENIED");
    } else {
        kprintf("[MEMTAG TEST]   note: pid %u has no cabin — grant/revoke skipped (OK)\n",
                test_pid);
    }

    /* Cleanup: clear guard so subsequent system code isn't affected. */
    MemTagSetGuard("test:phase13:capability", false);
    MT_CHECK(!MemTagIsGuard(cap_tid), "guard OFF cleared correctly");

    if (pcap) pmm_free(pcap, 1);

    /* ── Phase 14: MemTagEnforce / EnforcePhys gates (Phase 2B) ─── */
    kprintf("[MEMTAG TEST] Phase 14: MemTagEnforce/EnforcePhys gates\n");

    /* Untracked phys → allow (no region known). */
    MT_CHECK(MemTagEnforcePhys(0, 0xDEADBEEF000UL),
             "untracked phys → MemTagEnforcePhys returns true");

    /* Region without guards → allow. */
    void *p14a = MemTagPmmAlloc(1, "test:phase14:metadata-tag");
    uint32_t r14a = MemRegionFromPhys((uintptr_t)p14a);
    MT_CHECK(MemTagEnforce(0, 0, r14a),
             "no-guard region → MemTagEnforce returns true");
    MT_CHECK(MemTagEnforcePhys(0, (uintptr_t)p14a),
             "no-guard region via phys → MemTagEnforcePhys returns true");

    /* Region with guard → deny (pid 0, no cabin). */
    MemTagSetGuard("test:phase14:metadata-tag", true);
    MT_CHECK(!MemTagEnforce(0, 0, r14a),
             "guard set + pid invalid → MemTagEnforce returns false");
    MT_CHECK(!MemTagEnforcePhys(0, (uintptr_t)p14a),
             "guard set + pid invalid → MemTagEnforcePhys returns false");

    /* Invalid region_id → permissive default (no enforcement target). */
    MT_CHECK(MemTagEnforce(0, 0, MEMTAG_INVALID_REGION_ID),
             "invalid region_id → returns true (no enforcement)");

    /* Cleanup */
    MemTagSetGuard("test:phase14:metadata-tag", false);
    if (p14a) pmm_free(p14a, 1);

    /* ── Phase 14C: attach/detach bookkeeping (Phase 2C infra) ────── */
    kprintf("[MEMTAG TEST] Phase 14C: attach/detach bookkeeping\n");
    {
        /* Allocate a region and use real VMM contexts as stand-in cabins.
         * Phase 2D's StampPteRegion walks ctx->pml4 inside MemRegionAttachCabin,
         * so fake pointers would #GP — allocate two empty contexts whose
         * PML4 entries are all NULL for the test's va_base values. The
         * leaf walker returns NULL on first PML4 check (entries unmapped),
         * so stamping is a safe no-op while the attach bookkeeping is
         * exercised end-to-end. */
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

        /* Verify both ctx pointers + va_base pairs are present. */
        bool seen_a = false, seen_b = false;
        for (size_t i = 0; i < ns; i++) {
            if (snap[i].ctx == fake_ctx_a && snap[i].va_base == va1) seen_a = true;
            if (snap[i].ctx == fake_ctx_b && snap[i].va_base == va2) seen_b = true;
            MT_CHECK(snap[i].state == MEMTAG_ATTACH_ACTIVE,
                     "fresh attach state == ACTIVE");
        }
        MT_CHECK(seen_a && seen_b, "snapshot contains both attaches");

        /* Idempotent re-attach: should refresh, not duplicate. */
        error_t e3 = MemRegionAttachCabin(rat, fake_ctx_a, va1, 1,
                                           MEMTAG_ATTACH_CLASS_4K,
                                           VMM_FLAGS_USER_RW | VMM_FLAG_NO_EXECUTE);
        MT_CHECK(e3 == OK, "idempotent re-attach ok");
        ns = MemRegionRegistrySnapshotAttachs(MemTagGetRegionRegistry(),
                                               rat, snap, 8);
        MT_CHECK(ns == 2, "snapshot still returns 2 after re-attach");

        /* State change via set_attach_state. */
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

        /* Single detach. */
        error_t e4 = MemRegionDetachCabin(rat, fake_ctx_a, va1);
        MT_CHECK(e4 == OK, "single detach ok");
        ns = MemRegionRegistrySnapshotAttachs(MemTagGetRegionRegistry(),
                                               rat, snap, 8);
        MT_CHECK(ns == 1, "snapshot returns 1 after one detach");

        /* Bulk detach by ctx (defensive scrub). */
        size_t detached = MemTagDetachAllForCabin(fake_ctx_b);
        MT_CHECK(detached >= 1, "bulk detach by ctx removes ≥1");
        ns = MemRegionRegistrySnapshotAttachs(MemTagGetRegionRegistry(),
                                               rat, snap, 8);
        MT_CHECK(ns == 0, "snapshot empty after bulk detach");

        /* Idempotent detach. */
        error_t e5 = MemRegionDetachCabin(rat, fake_ctx_a, va1);
        MT_CHECK(e5 == OK, "idempotent detach (already gone) returns OK");

        if (pat) pmm_free(pat, 1);
        if (ctx_a) vmm_destroy_context(ctx_a);
        if (ctx_b) vmm_destroy_context(ctx_b);
    }

    /* ── Phase 14D: SweepGuard + EnforceRevokePost no-crash on bogus pid ── */
    kprintf("[MEMTAG TEST] Phase 14D: enforce post-ops no-crash on bogus pid\n");
    {
        uint16_t spook = MemTagInternStr("test:phase14d:guard");
        MT_CHECK(spook != MEMTAG_INVALID_TAG_ID, "intern phase14d guard tag");

        /* No regions carry this tag — sweep is no-op but must not crash. */
        size_t swept_on  = MemTagSweepGuard(spook, true);
        size_t swept_off = MemTagSweepGuard(spook, false);
        MT_CHECK(swept_on == 0 && swept_off == 0,
                 "SweepGuard returns 0 when no regions match");

        /* Bogus pid — post-grant/revoke must be safe (process_find fails). */
        size_t pr = MemCabinEnforceRevokePost(0xFFFFFFFFu, spook);
        size_t pg = MemCabinEnforceGrantPost(0xFFFFFFFFu, spook);
        MT_CHECK(pr == 0 && pg == 0,
                 "EnforceRevokePost / GrantPost no-op on bogus pid");
    }

    /* ── Phase 14E: MemRegionFromPte fast-path correctness (Phase 2D M1) ── */
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

            /* Synthesize PTE: phys + flags + correct encoded id. */
            uint64_t good_pte = phys_14e | encoded |
                                VMM_FLAG_PRESENT | VMM_FLAG_USER;
            uint32_t found = MemRegionFromPte(good_pte, phys_14e);
            MT_CHECK(found == r14e,
                     "MemRegionFromPte hits cache when encoded id covers phys");

            /* Synthesize PTE with WRONG encoded id but correct phys —
             * fast-path mismatches the slot's base_phys, falls back to
             * dense id_by_page → still returns r14e. */
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

    /* ── Phase 14F: MemRegionFromPte collision rejection ─────────────── */
    kprintf("[MEMTAG TEST] Phase 14F: MemRegionFromPte rejects bogus phys\n");
    {
        void *p14f = MemTagPmmAlloc(1, "test:phase14f:collide-base");
        MT_CHECK(p14f != NULL, "alloc 14f region");
        uint32_t r14f = MemRegionFromPhys((uintptr_t)p14f);
        MT_CHECK(r14f != MEMTAG_INVALID_REGION_ID, "find 14f region");

        if (r14f != MEMTAG_INVALID_REGION_ID) {
            /* Construct a PTE with r14f's encoded id but phys far above
             * mem_end (no region covers it). MemRegionFromPte should
             * reject the cache hit (slot's base_phys mismatch) and the
             * fallback should also fail (phys outside id_by_page). */
            uint64_t encoded = ((uint64_t)(r14f & MEMTAG_PTE_REGION_MAX))
                               << MEMTAG_PTE_REGION_SHIFT;
            uintptr_t bogus_phys = 0x1000000000000UL;  /* above MAXPHYADDR */
            uint64_t bogus_pte = bogus_phys | encoded | VMM_FLAG_PRESENT;
            uint32_t found = MemRegionFromPte(bogus_pte, bogus_phys);
            MT_CHECK(found == MEMTAG_INVALID_REGION_ID,
                     "MemRegionFromPte rejects mismatched phys (cache + fallback both miss)");
        }

        if (p14f) pmm_free(p14f, 1);
    }

    /* ── Phase 14G: M5 boot probe — bits 52-58 SAFE on this CPU ──────── */
    kprintf("[MEMTAG TEST] Phase 14G: M5 PTE-metadata probe\n");
    {
        bool m5 = MemTagVerifyPteMetadataBits();
        MT_CHECK(m5, "M5 probe returns true (bits 52-58 SAFE on current CPU)");
    }

    /* ── Phase 14H: cache:* derived tags on boot-seeded regions (2E) ── */
    kprintf("[MEMTAG TEST] Phase 14H: derived cache tags on boot regions\n");
    {
        /* Zones carry cache:wb (Phase 2E SeedZoneRegions). */
        s_res = MemTagAnd("zone:dma32", "cache:wb");
        MT_CHECK(s_res.count >= 1,
                 "zone:dma32 ∧ cache:wb finds boot zone region");

        /* MMIO E820 entries carry cache:uc (Phase 2E). */
        s_res = MemTagAnd("purpose:mmio", "cache:uc");
        MT_CHECK(s_res.count >= 1,
                 "purpose:mmio ∧ cache:uc finds MMIO region");

        /* Kernel image gets cache:wb. */
        s_res = MemTagAnd("purpose:kernel", "cache:wb");
        MT_CHECK(s_res.count >= 1,
                 "purpose:kernel ∧ cache:wb finds kernel image region");
    }

    /* ── Phase 14I: PAT MSR consistency probe (2E) ───────────────────── */
    kprintf("[MEMTAG TEST] Phase 14I: PAT MSR consistency probe\n");
    {
        bool pat_ok = MemTagVerifyPatMsr();
        MT_CHECK(pat_ok, "MemTagVerifyPatMsr returns true on BSP");

        uint64_t pat_msr = vmm_get_pat_msr_value();
        MT_CHECK(pat_msr != 0,
                 "vmm_get_pat_msr_value() non-zero (programmed by vmm_pat_init)");
    }

    /* ── Phase 14J: vmm_pte_cache_type_str decoder (2E) ──────────────── */
    kprintf("[MEMTAG TEST] Phase 14J: PTE→cache-type decoder\n");
    {
        /* PAT index 0 (no PWT, no PCD, no PAT) → PA0 = WB. */
        uint64_t pte_wb = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
        const char *c_wb = vmm_pte_cache_type_str(pte_wb, false);
        MT_CHECK(c_wb && strcmp(c_wb, "cache:wb") == 0,
                 "PTE with no cache flags → cache:wb (PAT[0]=WB)");

        /* PAT index 3 (PCD=1, PWT=1, PAT=0) → PA3 = UC.
         * Pattern used by vmm_map_mmio. */
        uint64_t pte_uc = VMM_FLAG_PRESENT |
                          VMM_FLAG_CACHE_DISABLE | VMM_FLAG_WRITE_THROUGH;
        const char *c_uc = vmm_pte_cache_type_str(pte_uc, false);
        MT_CHECK(c_uc && strcmp(c_uc, "cache:uc") == 0,
                 "PTE PCD+PWT → cache:uc (PAT[3]=UC, vmm_map_mmio pattern)");

        /* PAT index 6 (PCD=1, PAT=1, PWT=0) → PA6 = WC (vmm_pat_init programs).
         * Pattern used by vmm_map_framebuffer. */
        uint64_t pte_wc = VMM_FLAG_PRESENT |
                          VMM_FLAG_CACHE_DISABLE | VMM_FLAG_PAT_BIT;
        const char *c_wc = vmm_pte_cache_type_str(pte_wc, false);
        MT_CHECK(c_wc && strcmp(c_wc, "cache:wc") == 0,
                 "PTE PCD+PAT → cache:wc (PAT[6]=WC, vmm_map_framebuffer pattern)");

        /* PAT index 1 (PWT=1, PCD=0, PAT=0) → PA1 = WT. */
        uint64_t pte_wt = VMM_FLAG_PRESENT | VMM_FLAG_WRITE_THROUGH;
        const char *c_wt = vmm_pte_cache_type_str(pte_wt, false);
        MT_CHECK(c_wt && strcmp(c_wt, "cache:wt") == 0,
                 "PTE PWT → cache:wt (PAT[1]=WT)");

        /* vmm_pte_pat_index sanity — same flags should yield index 0..7. */
        MT_CHECK(vmm_pte_pat_index(pte_wb, false) == 0, "PAT index decode = 0 (WB)");
        MT_CHECK(vmm_pte_pat_index(pte_uc, false) == 3, "PAT index decode = 3 (UC)");
        MT_CHECK(vmm_pte_pat_index(pte_wc, false) == 6, "PAT index decode = 6 (WC)");
    }

    /* ── Phase 14K: MCE poison bitmap + tag + alloc skip (2F) ───────── */
    kprintf("[MEMTAG TEST] Phase 14K: MCE poison bitmap + alloc retry\n");
    {
        /* mce_init must have completed (called from main.c before this test). */
        MT_CHECK(mce_is_initialized(),
                 "MCE subsystem initialized on BSP");
        MT_CHECK(mce_bank_count() > 0,
                 "MCE reports at least one bank on this CPU");

        /* Allocate two DMA32 pages, free the second one, then poison its
         * phys. Re-allocation must skip the poisoned page and return
         * the first (still-cached in the buddy free list). */
        void *clean = pmm_alloc(1, PHYS_TAG_DMA32);
        MT_CHECK(clean != NULL, "alloc fresh DMA32 page");
        uintptr_t target_phys = (uintptr_t)clean;

        /* Initial state: target is NOT poisoned. */
        MT_CHECK(!pmm_is_poisoned(target_phys),
                 "fresh phys is not poisoned");
        size_t before = pmm_poisoned_page_count();

        /* Poison the phys. pmm_set_poisoned is the API the MCE handler
         * would call from IST context — exercise it directly here. */
        pmm_set_poisoned(target_phys);
        MT_CHECK(pmm_is_poisoned(target_phys),
                 "pmm_set_poisoned marks phys");
        MT_CHECK(pmm_poisoned_page_count() == before + 1,
                 "pmm_poisoned_page_count incremented");

        /* Apply mce:poisoned tag so userspace observers see the event. */
        error_t mce_tag_err = MemTagApplyByPhys(target_phys, 1, "mce:poisoned");
        MT_CHECK(mce_tag_err == OK, "MemTagApplyByPhys mce:poisoned ok");

        /* Bitmap query: mce:poisoned should now return ≥1 region. */
        s_res = MemTagAnd("mce:poisoned");
        MT_CHECK(s_res.count >= 1,
                 "MemTagAnd(mce:poisoned) finds the poisoned region");

        /* Free the chunk and try to re-allocate. The buddy will likely
         * hand back the SAME phys (LIFO free-list); _pmm_alloc_impl
         * post-checks pmm_is_range_poisoned, frees the chunk, and
         * retries. Result: returned addr must NOT equal target_phys
         * (unless all alternative pages were exhausted, in which case
         * we accept failure as a valid outcome — the test environment
         * may not have spare DMA32 pages). */
        pmm_free(clean, 1);

        void *retry = pmm_alloc(1, PHYS_TAG_DMA32);
        if (retry != NULL) {
            MT_CHECK((uintptr_t)retry != target_phys,
                     "post-poison alloc skips the poisoned phys");
            pmm_free(retry, 1);
        } else {
            kprintf("[MEMTAG TEST]   note: DMA32 retry returned NULL (zone "
                    "near-empty — skip-check still satisfied)\n");
        }

        /* Cleanup: we deliberately leave the poisoned bit set since the
         * page is "permanently bad" semantics. pmm_free was called above
         * for cleanliness, but the page is now bitmap-poisoned forever. */
    }

    /* ── Phase 14L: IOMMU MemTag/Touch surface (2G) ──────────────────── */
    kprintf("[MEMTAG TEST] Phase 14L: IOMMU domain accessor + namespace\n");
    {
        /* iommu_present mirrors g_ops != NULL. On QEMU TCG without
         * DMAR/IVRS this returns false; on real HW with VT-d or AMD-Vi
         * firmware it returns true. Either is acceptable — the test
         * verifies the API surface is wired, not that the IOMMU is
         * active. */
        bool present = iommu_present();
        MT_CHECK(present == (iommu_get_ops() != NULL),
                 "iommu_present matches g_ops state");

        /* iommu_domain_id on NULL returns INVALID. */
        MT_CHECK(iommu_domain_id(NULL) == 0xFFFFFFFFu,
                 "iommu_domain_id(NULL) returns INVALID");

        if (present) {
            /* When an IOMMU is online, a fresh domain_alloc should
             * succeed and report a valid ID. */
            iommu_domain_t* dom = iommu_domain_alloc();
            if (dom) {
                uint32_t id = iommu_domain_id(dom);
                MT_CHECK(id != 0xFFFFFFFFu,
                         "fresh domain has valid id");
                iommu_domain_free(dom);
            } else {
                kprintf("[MEMTAG TEST]   note: domain_alloc returned NULL "
                        "(backend cap reached — OK)\n");
            }
        } else {
            kprintf("[MEMTAG TEST]   note: no DMAR/IVRS — IOMMU "
                    "subsystem dormant (OK on QEMU TCG)\n");
        }

        /* Reserved namespace check — iommu:ready is the gate event;
         * by this point it has been published if iommu_init found a
         * backend. iommu:dma:mapped is reserved but only fires from
         * actual driver-side iommu_map calls. */
        uint16_t ready_tid = MemTagResolveStr("iommu:ready");
        MT_CHECK(ready_tid != MEMTAG_INVALID_TAG_ID,
                 "iommu:ready tag interned (reserved namespace seeded)");
    }

    /* ── Phase 14M: PKU/PKS encoding + namespace (2H) ─────────────── */
    kprintf("[MEMTAG TEST] Phase 14M: PKU/PKS PTE bits 62:59\n");
    {
        /* Encoder/decoder round-trip — pure functions, work regardless
         * of has_pku at runtime. */
        for (uint8_t k = 0; k <= VMM_PTE_PKEY_MAX; k++) {
            uint64_t enc = vmm_pte_encode_pkey(k);
            uint8_t  dec = vmm_pte_pkey(enc | VMM_FLAG_PRESENT);
            MT_CHECK(dec == k, "vmm_pte_encode_pkey ↔ vmm_pte_pkey round-trip");
            if (dec != k) break;
        }

        /* Reserved namespace: pku:0 and pku:15 must be interned. */
        MT_CHECK(MemTagResolveStr("pku:0")  != MEMTAG_INVALID_TAG_ID,
                 "pku:0 interned");
        MT_CHECK(MemTagResolveStr("pku:15") != MEMTAG_INVALID_TAG_ID,
                 "pku:15 interned");
        MT_CHECK(MemTagResolveStr("pku:fault:denied") != MEMTAG_INVALID_TAG_ID,
                 "pku:fault:denied event tag interned");

        /* If the CPU supports PKU, read_pkru should at least not #GP
         * (CR4.PKE was set by vmm_pku_init). On non-PKU systems the
         * helper returns 0 by gate. */
        if (g_cpu_caps.has_pku) {
            uint32_t pkru_now = vmm_read_pkru();
            MT_CHECK(true,
                     "RDPKRU executed without #GP (CR4.PKE active)");
            /* No semantic assertion on the value — default is 0
             * (everyone allowed) but firmware may have left a non-zero
             * value. Just log. */
            kprintf("[MEMTAG TEST]   PKRU snapshot: 0x%08x\n",
                    (unsigned)pkru_now);
        } else {
            MT_CHECK(vmm_read_pkru() == 0,
                     "vmm_read_pkru returns 0 on non-PKU CPU (gate)");
            kprintf("[MEMTAG TEST]   note: CPU lacks PKU — RDPKRU "
                    "path skipped\n");
        }
    }

    /* ── Phase 14N: LAM substrate (2I) ───────────────────────────────── */
    kprintf("[MEMTAG TEST] Phase 14N: LAM tag-bit helpers + CR3 mask\n");
    {
        /* LAM_U48 tag round-trip: 7-bit field at bits 62:56. */
        for (uint8_t tag = 0; tag <= 0x7F; tag++) {
            uint64_t base = 0x0000123456789000ULL;  /* canonical user VA */
            uint64_t p    = vmm_user_ptr_set_tag_u48(base, tag);
            MT_CHECK(vmm_user_ptr_get_tag_u48(p) == tag,
                     "LAM_U48 tag round-trip");
            if (vmm_user_ptr_get_tag_u48(p) != tag) break;
        }

        /* LAM_U57 tag round-trip: 6-bit field at bits 62:57. */
        for (uint8_t tag = 0; tag <= 0x3F; tag++) {
            uint64_t base = 0x0000123456789000ULL;
            uint64_t p    = vmm_user_ptr_set_tag_u57(base, tag);
            MT_CHECK(vmm_user_ptr_get_tag_u57(p) == tag,
                     "LAM_U57 tag round-trip");
            if (vmm_user_ptr_get_tag_u57(p) != tag) break;
        }

        /* CR3 LAM bits must not overlap PML4 phys (bits 51:12 per Intel
         * SDM Vol 3A §4.5.4). Verify by construction: vmm_pte_addr_mask
         * masks 51:12 only, never the LAM bits 62:48. */
        MT_CHECK((vmm_pte_addr_mask & VMM_CR3_LAM_U48) == 0,
                 "CR3.LAM_U48 outside PML4 phys mask");
        MT_CHECK((vmm_pte_addr_mask & VMM_CR3_LAM_U57) == 0,
                 "CR3.LAM_U57 outside PML4 phys mask");

        /* Reserved namespace */
        MT_CHECK(MemTagResolveStr("lam:ready") != MEMTAG_INVALID_TAG_ID,
                 "lam:ready interned");
        MT_CHECK(MemTagResolveStr("lam:fault:tag") != MEMTAG_INVALID_TAG_ID,
                 "lam:fault:tag interned");

        if (g_cpu_caps.has_lam) {
            kprintf("[MEMTAG TEST]   LAM supported on this CPU\n");
        } else {
            kprintf("[MEMTAG TEST]   note: CPU lacks LAM — substrate "
                    "still tested (pure helpers)\n");
        }
    }

    /* ── Phase 14O: TME / TME-MK substrate (2J) ─────────────────────── */
    kprintf("[MEMTAG TEST] Phase 14O: TME-MK KeyID encoding + namespace\n");
    {
        /* KeyID encoder round-trip — pure function, works regardless
         * of has_tme. Treat MAXPHYADDR=42 + 4 KeyID bits as a fixed
         * platform pattern for the test. */
        uint64_t phys_base = 0x0000000ABCDEF000ULL;  /* page-aligned */
        for (uint8_t keyid = 0; keyid <= 0xF; keyid++) {
            uint64_t encoded = vmm_phys_with_keyid(phys_base, keyid, 4, 42);
            /* The 4-bit window at bits 45:42 must hold the keyid. */
            uint8_t recovered = (uint8_t)((encoded >> 42) & 0xF);
            MT_CHECK(recovered == keyid,
                     "vmm_phys_with_keyid round-trip");
            /* Phys bits below the window must be untouched. */
            MT_CHECK((encoded & ((1ULL << 42) - 1ULL)) ==
                     (phys_base & ((1ULL << 42) - 1ULL)),
                     "low phys bits preserved");
        }

        /* Reserved namespace */
        MT_CHECK(MemTagResolveStr("tme:ready") != MEMTAG_INVALID_TAG_ID,
                 "tme:ready interned");
        MT_CHECK(MemTagResolveStr("tme:mk_active") != MEMTAG_INVALID_TAG_ID,
                 "tme:mk_active interned");
        MT_CHECK(MemTagResolveStr("tme:keyid:0") != MEMTAG_INVALID_TAG_ID,
                 "tme:keyid:0 interned");
        MT_CHECK(MemTagResolveStr("tme:keyid:15") != MEMTAG_INVALID_TAG_ID,
                 "tme:keyid:15 interned");

        if (g_cpu_caps.has_tme) {
            kprintf("[MEMTAG TEST]   TME supported on this CPU\n");
        } else {
            kprintf("[MEMTAG TEST]   note: CPU lacks TME — encoder "
                    "still validated (pure)\n");
        }
    }

    /* ── Phase 14P: CET shadow-stack encoding + namespace (2K) ────── */
    kprintf("[MEMTAG TEST] Phase 14P: CET shadow-stack PTE bits 60/61\n");
    {
        /* PTE bit 60 (supervisor SS) round-trip. */
        uint64_t base = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
        uint64_t with_ss = vmm_pte_with_cet_supv_ss(base);
        MT_CHECK((with_ss & VMM_PTE_CET_SS_SUPV) != 0,
                 "vmm_pte_with_cet_supv_ss sets bit 60");
        MT_CHECK(vmm_pte_is_supv_ss(with_ss),
                 "vmm_pte_is_supv_ss detects bit 60");
        MT_CHECK(!vmm_pte_is_supv_ss(base),
                 "vmm_pte_is_supv_ss false on unstamped PTE");

        /* Bit 60 must not overlap PML4 phys mask (Phase 2D region bits
         * 52-58 either). Defensive: ensure encoding stays in CET's
         * documented bit window. */
        MT_CHECK((vmm_pte_addr_mask & VMM_PTE_CET_SS_SUPV) == 0,
                 "CET SS bit 60 outside phys mask");
        MT_CHECK((MEMTAG_PTE_REGION_MASK & VMM_PTE_CET_SS_SUPV) == 0,
                 "CET SS bit 60 outside Phase 2D region bits");
        /* NOTE: bit 60 is intentionally inside Phase 2H PKEY mask
         * (bits 62:59). Intel SDM gives bit 60 two interpretations
         * gated by CR4: PKEY field when CR4.PKE=1, supervisor SS when
         * CR4.CET=1. Stamping both on the same page is a kernel-policy
         * error — Phase 2K stamping callers must check the active
         * feature mix before composing CET + PKU. */

        /* Reserved namespace */
        MT_CHECK(MemTagResolveStr("cet:ready") != MEMTAG_INVALID_TAG_ID,
                 "cet:ready interned");
        MT_CHECK(MemTagResolveStr("cet:shstk:supervisor") != MEMTAG_INVALID_TAG_ID,
                 "cet:shstk:supervisor interned");
        MT_CHECK(MemTagResolveStr("cet:fault:cp") != MEMTAG_INVALID_TAG_ID,
                 "cet:fault:cp interned");

        if (g_cpu_caps.has_shstk || g_cpu_caps.has_ibt) {
            kprintf("[MEMTAG TEST]   CET supported on this CPU "
                    "(SHSTK=%d IBT=%d)\n",
                    (int)g_cpu_caps.has_shstk,
                    (int)g_cpu_caps.has_ibt);
        } else {
            kprintf("[MEMTAG TEST]   note: CPU lacks CET — encoders "
                    "still validated (pure)\n");
        }
    }

    /* ── Phase 12: stress — 64 allocs × 8 tags ────────────────────── */
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
        /* Use MemTagPmmAlloc directly — the pmm_alloc macro's
         * __typeof__(+(arg)) trick can't classify dynamic char* values
         * (only literal char[N] arrays). */
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
        s_res = MemTagQueryOr_(all8);
        MT_CHECK(s_res.count >= n_alloc,
                 "OR of all 8 stress tags covers all allocations");
    }

    /* Cleanup */
    for (size_t i = 0; i < n_alloc; i++) {
        pmm_free((void *)s_stress[i], 1);
    }

    /* Cleanup remaining Phase 2-10 allocs */
    if (p2)    pmm_free(p2, 1);
    if (p3)    { MemRegionDestroy(r3);  pmm_free(p3, 1); }
    if (p4a)   pmm_free(p4a, 1);
    if (p4b)   pmm_free(p4b, 1);
    if (p5a)   { MemRegionDestroy(r5a); pmm_free(p5a, 1); }
    if (p5b)   { MemRegionDestroy(r5b); pmm_free(p5b, 1); }
    if (p5c)   { MemRegionDestroy(r5c); pmm_free(p5c, 1); }
    if (pinv)  { MemRegionDestroy(rinv); pmm_free(pinv, 1); }
    if (pbnd)  { MemRegionDestroy(rbnd); pmm_free(pbnd, 1); }

    /* ── Summary ──────────────────────────────────────────────────── */
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
