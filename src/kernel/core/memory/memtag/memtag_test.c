#include "memtag.h"
#include "pmm.h"
#include "klib.h"

// Single static result buffer — avoids placing 4104-byte MemTagResult on the
// 16 KB kernel stack (512 region pointers × 8 bytes = 4096 bytes per result).
static MemTagResult s_res;

#define MT_CHECK(cond, label) \
    do { if (cond) { pass++; } \
         else { fail++; kprintf("[MEMTAG TEST]   %[R]FAIL%[D]: " label "\n"); } \
    } while (0)

// Count-only helper: query and return just .count without storing the result.
static size_t count_and(const char *a, const char *b) {
    const char *tags[3] = { a, b, NULL };
    return MemTagQueryAnd(tags).count;
}
static size_t count_and1(const char *a) {
    const char *tags[2] = { a, NULL };
    return MemTagQueryAnd(tags).count;
}

void MemTagStressTest(void)
{
    kprintf("[MEMTAG TEST] Starting MemTag stress test...\n");

    size_t pass = 0, fail = 0;

    // ── Phase 1: basic alloc + tag ────────────────────────────────────────────
    kprintf("[MEMTAG TEST] Phase 1: basic alloc+tag\n");

    void *p1 = _pmm_alloc_memtag(1, "mt:basic");
    MT_CHECK(p1 != NULL, "p1 alloc non-NULL");

    if (p1) {
        MT_CHECK(MemTagFindRegion((uintptr_t)p1) != NULL,
                 "FindRegion after tagged alloc");
        MT_CHECK(MemTagLookup("mt:basic") != MEMTAG_ID_NONE,
                 "Lookup mt:basic != NONE");
        MT_CHECK(count_and1("mt:basic") >= 1,
                 "MemTagAnd(mt:basic).count >= 1");
    }

    // ── Phase 2: multi-tag on one region ──────────────────────────────────────
    kprintf("[MEMTAG TEST] Phase 2: multi-tag on one region\n");

    void *p2 = pmm_alloc(1);
    MT_CHECK(p2 != NULL, "p2 bare alloc non-NULL");

    if (p2) {
        error_t e2 = MemTagAdd((uintptr_t)p2, 1, "mt:a", "mt:b", "mt:shared", NULL);
        MT_CHECK(e2 == OK, "MemTagAdd three tags to p2");
        MT_CHECK(count_and("mt:a", "mt:b") >= 1,
                 "MemTagAnd(mt:a, mt:b).count >= 1");
        MT_CHECK(count_and("mt:a", "mt:notexist") == 0,
                 "MemTagAnd(mt:a, mt:notexist) == 0 (unknown tag)");
    }

    // ── Phase 3: OR query ─────────────────────────────────────────────────────
    kprintf("[MEMTAG TEST] Phase 3: OR query\n");

    void *p3a = _pmm_alloc_memtag(1, "mt:or_x");
    void *p3b = _pmm_alloc_memtag(1, "mt:or_y");
    MT_CHECK(p3a != NULL, "p3a alloc");
    MT_CHECK(p3b != NULL, "p3b alloc");

    if (p3a && p3b) {
        s_res = MemTagOr("mt:or_x", "mt:or_y");
        MT_CHECK(s_res.count >= 2, "MemTagOr(or_x, or_y).count >= 2");

        bool found_p3a = false, found_p3b = false;
        for (size_t i = 0; i < s_res.count; i++) {
            if (s_res.regions[i]->base_phys == (uintptr_t)p3a) found_p3a = true;
            if (s_res.regions[i]->base_phys == (uintptr_t)p3b) found_p3b = true;
        }
        MT_CHECK(found_p3a, "p3a found in OR result");
        MT_CHECK(found_p3b, "p3b found in OR result");
    }

    // ── Phase 4: Mixed AND + OR + excluded ────────────────────────────────────
    kprintf("[MEMTAG TEST] Phase 4: mixed AND+OR+excluded query\n");

    void *pa = pmm_alloc(1);
    void *pb = pmm_alloc(1);
    void *pc = pmm_alloc(1);
    MT_CHECK(pa != NULL, "pa alloc");
    MT_CHECK(pb != NULL, "pb alloc");
    MT_CHECK(pc != NULL, "pc alloc");

    if (pa && pb && pc) {
        MemTagAdd((uintptr_t)pa, 1, "mt:shared2", "mt:a2", NULL);
        MemTagAdd((uintptr_t)pb, 1, "mt:shared2", "mt:b2", NULL);
        MemTagAdd((uintptr_t)pc, 1, "mt:shared2", "mt:a2", "mt:excl", NULL);

        const char *const req[]  = { "mt:shared2", NULL };
        const char *const any[]  = { "mt:a2", "mt:b2", NULL };
        const char *const excl[] = { "mt:excl", NULL };
        s_res = MemTagQueryMixed(req, any, excl);

        MT_CHECK(s_res.count >= 2,
                 "Mixed query count >= 2 (pa + pb pass, pc excluded)");

        bool pc_leaked = false;
        for (size_t i = 0; i < s_res.count; i++) {
            if (s_res.regions[i]->base_phys == (uintptr_t)pc) {
                pc_leaked = true;
                break;
            }
        }
        MT_CHECK(!pc_leaked, "pc NOT in mixed result (mt:excl correctly filtered)");
    }

    // ── Phase 5: RemoveFromRegion ─────────────────────────────────────────────
    kprintf("[MEMTAG TEST] Phase 5: RemoveFromRegion\n");

    if (p2) {
        size_t before = count_and1("mt:a");
        MemTagRemoveFromRegion((uintptr_t)p2, "mt:a");
        size_t after  = count_and1("mt:a");
        MT_CHECK(after == before - 1,
                 "RemoveFromRegion: And(mt:a) count drops by 1");

        s_res = MemTagAnd("mt:b");
        bool p2_has_b = false;
        for (size_t i = 0; i < s_res.count; i++) {
            if (s_res.regions[i]->base_phys == (uintptr_t)p2) {
                p2_has_b = true;
                break;
            }
        }
        MT_CHECK(p2_has_b, "p2 still found via mt:b after mt:a removal");
    }

    // ── Phase 6: RemoveRegion ─────────────────────────────────────────────────
    kprintf("[MEMTAG TEST] Phase 6: RemoveRegion\n");

    if (p2) {
        size_t sh_before = count_and1("mt:shared");
        MemTagRemoveRegion((uintptr_t)p2);
        size_t sh_after  = count_and1("mt:shared");
        MT_CHECK(sh_after == sh_before - 1,
                 "RemoveRegion decrements And(mt:shared) by 1");
        MT_CHECK(MemTagFindRegion((uintptr_t)p2) == NULL,
                 "FindRegion returns NULL after RemoveRegion");
        pmm_free(p2, 1);
        p2 = NULL;
    }

    // ── Phase 7: bitmap word boundary (tag IDs crossing bit 63 → 64) ─────────
    kprintf("[MEMTAG TEST] Phase 7: bitmap word boundary\n");

    void *pbnd = pmm_alloc(1);
    MT_CHECK(pbnd != NULL, "pbnd alloc for boundary test");

    if (pbnd) {
        // Pad the tag registry until we have IDs assigned near the 64-bit word
        // boundary (bit 63 = last bit of word 0, bit 64 = first bit of word 1).
        char pad_name[MEMTAG_NAME_MAX];
        int pad_idx = 0;
        memtag_id_t cur_id = 0;
        while (cur_id < 62 && cur_id != MEMTAG_ID_NONE) {
            ksnprintf(pad_name, sizeof(pad_name), "mt:pad%d", pad_idx++);
            cur_id = MemTagRegister(pad_name);
        }

        memtag_id_t id_lo = MemTagRegister("mt:bnd_lo");   // should be ~63
        memtag_id_t id_hi = MemTagRegister("mt:bnd_hi");   // should be ~64

        kprintf("[MEMTAG TEST]   bnd_lo id=%u  bnd_hi id=%u\n",
                (unsigned)id_lo, (unsigned)id_hi);

        MT_CHECK(id_lo != MEMTAG_ID_NONE, "bnd_lo registered");
        MT_CHECK(id_hi != MEMTAG_ID_NONE, "bnd_hi registered");

        MemTagAdd((uintptr_t)pbnd, 1, "mt:bnd_lo", "mt:bnd_hi", NULL);

        s_res = MemTagAnd("mt:bnd_lo", "mt:bnd_hi");
        MT_CHECK(s_res.count >= 1, "MemTagAnd(bnd_lo, bnd_hi).count >= 1");

        bool pbnd_found = false;
        for (size_t i = 0; i < s_res.count; i++) {
            if (s_res.regions[i]->base_phys == (uintptr_t)pbnd) {
                pbnd_found = true;
                break;
            }
        }
        MT_CHECK(pbnd_found, "pbnd address found in boundary AND result");

        MemTagRemoveRegion((uintptr_t)pbnd);
        pmm_free(pbnd, 1);
        pbnd = NULL;
    }

    // ── Phase 8: stress — 64 allocs cycling 8 tags ───────────────────────────
    kprintf("[MEMTAG TEST] Phase 8: stress 64 allocs x 8 tags\n");

    #define STRESS_N    64
    #define STRESS_TAGS 8

    static const char *s_tags[STRESS_TAGS] = {
        "mt:s0", "mt:s1", "mt:s2", "mt:s3",
        "mt:s4", "mt:s5", "mt:s6", "mt:s7"
    };
    static uintptr_t s_stress[STRESS_N];

    size_t n_alloc = 0;
    for (size_t i = 0; i < STRESS_N; i++) {
        void *sp = _pmm_alloc_memtag(1, s_tags[i % STRESS_TAGS]);
        if (!sp) break;
        s_stress[n_alloc++] = (uintptr_t)sp;
    }
    kprintf("[MEMTAG TEST]   stress allocated %zu/%u pages\n", n_alloc, STRESS_N);

    if (n_alloc > 0) {
        size_t expected = n_alloc / STRESS_TAGS;
        for (size_t t = 0; t < STRESS_TAGS; t++) {
            size_t cnt = count_and1(s_tags[t]);
            bool ok = (cnt >= expected - 1) && (cnt <= expected + 1);
            MT_CHECK(ok, "stress per-tag count within ±1 of expected");
        }

        const char *all8[STRESS_TAGS + 1];
        for (size_t t = 0; t < STRESS_TAGS; t++) all8[t] = s_tags[t];
        all8[STRESS_TAGS] = NULL;
        s_res = MemTagQueryOr(all8);
        MT_CHECK(s_res.count >= n_alloc,
                 "OR of all 8 stress tags covers all allocations");
    }

    for (size_t i = 0; i < n_alloc; i++) {
        MemTagRemoveRegion(s_stress[i]);
        pmm_free((void *)s_stress[i], 1);
    }

    // ── Cleanup remaining allocs from earlier phases ──────────────────────────
    if (p1)  { MemTagRemoveRegion((uintptr_t)p1);  pmm_free(p1,  1); }
    if (p3a) { MemTagRemoveRegion((uintptr_t)p3a); pmm_free(p3a, 1); }
    if (p3b) { MemTagRemoveRegion((uintptr_t)p3b); pmm_free(p3b, 1); }
    if (pa)  { MemTagRemoveRegion((uintptr_t)pa);  pmm_free(pa,  1); }
    if (pb)  { MemTagRemoveRegion((uintptr_t)pb);  pmm_free(pb,  1); }
    if (pc)  { MemTagRemoveRegion((uintptr_t)pc);  pmm_free(pc,  1); }

    // ── Summary ───────────────────────────────────────────────────────────────
    if (fail == 0)
        kprintf("[MEMTAG TEST] %[S]PASSED%[D]: all %zu checks OK\n", pass);
    else
        kprintf("[MEMTAG TEST] %[R]FAILED%[D]: %zu pass, %zu fail\n", pass, fail);
}
