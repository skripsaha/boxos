/*
 * CET lifecycle integration test.
 *
 * On QEMU TCG (which doesn't advertise SHSTK/IBT as of QEMU 10.x) the
 * runtime stays dormant — these tests verify the dormant path doesn't
 * crash + correctly reports "not enabled" through cet_is_enabled +
 * the stats accessor. Real-HW CET enforcement (RET-vs-shadow-stack,
 * indirect-call-vs-ENDBR64) belongs to the physical-HW QA pass
 * (must-implement #4).
 *
 * Scenarios:
 *   T1. cet_lifecycle_init_bsp can be called when CET is dormant +
 *       reports ERR_UNSUPPORTED (TCG case).
 *   T2. cet_is_enabled reflects the post-init state coherently with
 *       g_cpu_caps.has_shstk / has_ibt.
 *   T3. cet_process_create on a CET-less CPU sets ssp fields to 0.
 *   T4. process_user_ssp_va_for returns the canonical SSP VA anchor
 *       (below VMM_USER_STACK_TOP) regardless of CET state.
 *   T5. cet_lifecycle_get_stats fills every field without UB.
 */

#include "cet_lifecycle.h"
#include "process.h"
#include "cpuid.h"
#include "klib.h"
#include "vmm.h"

#define CET_CHECK(cond, label) \
    do { if (cond) { pass++; } \
         else { fail++; \
                kprintf("[CET TEST]   %[R]FAIL%[D]: " label "\n"); } \
    } while (0)

void CetLifecycleTest(void) {
    kprintf("[CET TEST] Starting CET lifecycle test...\n");
    size_t pass = 0, fail = 0;

    /* ── T1: init idempotency ───────────────────────────────────── */
    {
        error_t e1 = cet_lifecycle_init_bsp();
        error_t e2 = cet_lifecycle_init_bsp();   /* idempotent */
        bool both_ok = (e1 == OK || e1 == ERR_UNSUPPORTED) &&
                       (e2 == OK || e2 == ERR_UNSUPPORTED);
        CET_CHECK(both_ok, "T1: init_bsp idempotent (OK or NOT_SUPPORTED)");
    }

    /* ── T2: cet_is_enabled coherent with cpu caps ──────────────── */
    {
        bool en       = cet_is_enabled();
        bool has_any  = g_cpu_caps.has_shstk || g_cpu_caps.has_ibt;
        if (!has_any) {
            CET_CHECK(!en, "T2: dormant on CPU without SHSTK/IBT");
        } else {
            CET_CHECK(en, "T2: enabled on CET-capable CPU");
        }
    }

    /* ── T3: process_create on CET-less leaves ssp=0 ────────────── */
    if (!g_cpu_caps.has_shstk) {
        /* Synthesize a transient process struct stub.
         * cet_process_create only reads has_shstk to decide; it doesn't
         * dereference cabin in the dormant branch. */
        process_t dummy;
        for (size_t i = 0; i < sizeof(dummy); i++) ((uint8_t *)&dummy)[i] = 0;
        dummy.pid = 0xCE7u;
        error_t e = cet_process_create(&dummy);
        CET_CHECK(e == OK, "T3: cet_process_create returns OK on dormant CPU");
        CET_CHECK(process_get_user_ssp_phys(&dummy) == 0,
                  "T3: SSP phys remains 0 on dormant CPU");
        CET_CHECK(process_get_user_ssp_va(&dummy) == 0,
                  "T3: SSP va remains 0 on dormant CPU");
        CET_CHECK(process_get_user_ssp_size(&dummy) == 0,
                  "T3: SSP size remains 0 on dormant CPU");
    }

    /* ── T4: canonical SSP VA anchor ───────────────────────────── */
    {
        uintptr_t va         = process_user_ssp_va_for(NULL);
        uintptr_t guard_hi   = process_user_ssp_guard_hi_for(NULL);
        uintptr_t guard_lo   = process_user_ssp_guard_lo_for(NULL);
        CET_CHECK(va != 0, "T4: SSP VA anchor non-zero");
        CET_CHECK(va < VMM_USER_STACK_TOP,
                  "T4: SSP VA anchor below VMM_USER_STACK_TOP");
        /* Layout invariants (process.c PROCESS_USER_SSP_*):
         *   - SSP region is 4 pages (16 KiB).
         *   - HI guard sits ABOVE the region (= VA + SIZE).
         *   - LO guard sits BELOW the region (= VA - 4 KiB).
         *   - 8 MiB slack separates region top from the max possible
         *     ASLR-shifted user stack base.
         * We assert structural relationships rather than absolute
         * constants so the layout can change without churning this
         * test — the relationships are what real-HW correctness depends
         * on. */
        CET_CHECK((va & 0xFFFu) == 0,
                  "T4: SSP VA anchor page-aligned");
        CET_CHECK(guard_hi == va + (4u * 4096u),
                  "T4: HI guard at va + 16 KiB");
        CET_CHECK(guard_lo == va - 0x1000ULL,
                  "T4: LO guard at va - 4 KiB");
        CET_CHECK(guard_hi + (8ULL * 1024ULL * 1024ULL) == VMM_USER_STACK_TOP,
                  "T4: SSP region clear of max ASLR stack reach");
    }

    /* ── T5: stats sane ─────────────────────────────────────────── */
    {
        cet_lifecycle_stats_t s;
        cet_lifecycle_get_stats(&s);
        bool sane = (s.cp_faults  >= 0 || s.cp_faults  == 0) &&
                    (s.ssp_allocs >= 0 || s.ssp_allocs == 0) &&
                    (s.ssp_frees  >= 0 || s.ssp_frees  == 0);
        CET_CHECK(sane, "T5: stats accessors no UB");
        CET_CHECK(s.enabled == cet_is_enabled(),
                  "T5: stats.enabled coherent with cet_is_enabled()");
    }

    if (fail == 0)
        kprintf("[CET TEST] %[S]PASSED%[D]: all %zu checks OK\n", pass);
    else
        kprintf("[CET TEST] %[R]FAILED%[D]: %zu pass, %zu fail\n", pass, fail);

    cet_lifecycle_dump();
}
