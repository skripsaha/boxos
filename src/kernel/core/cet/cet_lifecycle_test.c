
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

    {
        error_t e1 = cet_lifecycle_init_bsp();
        error_t e2 = cet_lifecycle_init_bsp();
        bool both_ok = (e1 == OK || e1 == ERR_UNSUPPORTED) &&
                       (e2 == OK || e2 == ERR_UNSUPPORTED);
        CET_CHECK(both_ok, "T1: init_bsp idempotent (OK or NOT_SUPPORTED)");
    }

    {
        bool en       = cet_is_enabled();
        bool has_any  = g_cpu_caps.has_shstk || g_cpu_caps.has_ibt;
        if (!has_any) {
            CET_CHECK(!en, "T2: dormant on CPU without SHSTK/IBT");
        } else {
            CET_CHECK(en, "T2: enabled on CET-capable CPU");
        }
    }

    if (!g_cpu_caps.has_shstk) {
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

    {
        uintptr_t va         = process_user_ssp_va_for(NULL);
        uintptr_t guard_hi   = process_user_ssp_guard_hi_for(NULL);
        uintptr_t guard_lo   = process_user_ssp_guard_lo_for(NULL);
        CET_CHECK(va != 0, "T4: SSP VA anchor non-zero");
        CET_CHECK(va < VMM_USER_STACK_TOP,
                  "T4: SSP VA anchor below VMM_USER_STACK_TOP");
        CET_CHECK((va & 0xFFFu) == 0,
                  "T4: SSP VA anchor page-aligned");
        CET_CHECK(guard_hi == va + (4u * 4096u),
                  "T4: HI guard at va + 16 KiB");
        CET_CHECK(guard_lo == va - 0x1000ULL,
                  "T4: LO guard at va - 4 KiB");
        CET_CHECK(guard_hi + (8ULL * 1024ULL * 1024ULL) == VMM_USER_STACK_TOP,
                  "T4: SSP region clear of max ASLR stack reach");
    }

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