
#include "vmm.h"
#include "klib.h"
#include "cpuid.h"
#include "memtag.h"

#define VT_CHECK(cond, label) \
    do { if (cond) { pass++; } \
         else { fail++; kprintf("[VMM TEST]   %[R]FAIL%[D]: " label "\n"); } \
    } while (0)

void VmmHelperTest(void) {
    kprintf("[VMM TEST] Starting VMM helper + probe test...\n");
    size_t pass = 0, fail = 0;

    kprintf("[VMM TEST] A: PTE bits 52-58 metadata probe\n");
    VT_CHECK(vmm_verify_pte_metadata_bits_52_58(),
             "bits 52-58 SAFE on current CPU");

    kprintf("[VMM TEST] B: PAT MSR consistency probe\n");
    VT_CHECK(vmm_verify_pat_msr(),
             "vmm_verify_pat_msr returns true on BSP");
    VT_CHECK(vmm_get_pat_msr_value() != 0,
             "vmm_get_pat_msr_value() non-zero (programmed by vmm_pat_init)");

    kprintf("[VMM TEST] C: PTE→cache-type decoder\n");
    {
        uint64_t pte_wb = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
        const char *c_wb = vmm_pte_cache_type_str(pte_wb, false);
        VT_CHECK(c_wb && strcmp(c_wb, "cache:wb") == 0,
                 "PTE no cache flags → cache:wb (PAT[0]=WB)");

        uint64_t pte_uc = VMM_FLAG_PRESENT |
                          VMM_FLAG_CACHE_DISABLE | VMM_FLAG_WRITE_THROUGH;
        const char *c_uc = vmm_pte_cache_type_str(pte_uc, false);
        VT_CHECK(c_uc && strcmp(c_uc, "cache:uc") == 0,
                 "PTE PCD+PWT → cache:uc (PAT[3]=UC)");

        uint64_t pte_wc = VMM_FLAG_PRESENT |
                          VMM_FLAG_CACHE_DISABLE | VMM_FLAG_PAT_BIT;
        const char *c_wc = vmm_pte_cache_type_str(pte_wc, false);
        VT_CHECK(c_wc && strcmp(c_wc, "cache:wc") == 0,
                 "PTE PCD+PAT → cache:wc (PAT[6]=WC)");

        uint64_t pte_wt = VMM_FLAG_PRESENT | VMM_FLAG_WRITE_THROUGH;
        const char *c_wt = vmm_pte_cache_type_str(pte_wt, false);
        VT_CHECK(c_wt && strcmp(c_wt, "cache:wt") == 0,
                 "PTE PWT → cache:wt (PAT[1]=WT)");

        VT_CHECK(vmm_pte_pat_index(pte_wb, false) == 0, "PAT index = 0");
        VT_CHECK(vmm_pte_pat_index(pte_uc, false) == 3, "PAT index = 3");
        VT_CHECK(vmm_pte_pat_index(pte_wc, false) == 6, "PAT index = 6");
    }

    kprintf("[VMM TEST] D: PKU bit-62:59 encoder + effective decoder\n");
    for (uint8_t k = 0; k <= VMM_PTE_PKEY_MAX; k++) {
        uint64_t enc = vmm_pte_encode_pkey(k);
        VT_CHECK(vmm_pte_pkey(enc | VMM_FLAG_PRESENT) == k,
                 "vmm_pte_encode_pkey ↔ vmm_pte_pkey round-trip");
    }
    {
        uint64_t pkey1     = vmm_pte_encode_pkey(1);
        uint64_t cet_only  = VMM_PTE_CET_SS_SUPV;
        uint64_t both      = pkey1 | VMM_PTE_CET_SS_SUPV;
        VT_CHECK(!vmm_pte_pkey_cet_conflict(0), "no conflict on zero flags");
        VT_CHECK(!vmm_pte_pkey_cet_conflict(pkey1),
                 "no conflict on pkey=1 alone (bit 60 unused)");
        VT_CHECK(!vmm_pte_pkey_cet_conflict(cet_only),
                 "no conflict on CET-only");
        VT_CHECK(vmm_pte_pkey_cet_conflict(both),
                 "conflict on pkey=1 + CET bit 60");
    }
    if (g_cpu_caps.has_pku) {
        (void)vmm_read_pkru();
        VT_CHECK(true, "RDPKRU executed without #GP");
    } else {
        VT_CHECK(vmm_read_pkru() == 0,
                 "vmm_read_pkru returns 0 on non-PKU CPU");
    }

    kprintf("[VMM TEST] E: LAM tag-bit helpers\n");
    for (uint8_t tag = 0; tag <= 0x7F; tag++) {
        uint64_t base = 0x0000123456789000ULL;
        uint64_t p    = vmm_user_ptr_set_tag_u48(base, tag);
        VT_CHECK(vmm_user_ptr_get_tag_u48(p) == tag,
                 "LAM_U48 tag round-trip");
    }
    for (uint8_t tag = 0; tag <= 0x3F; tag++) {
        uint64_t base = 0x0000123456789000ULL;
        uint64_t p    = vmm_user_ptr_set_tag_u57(base, tag);
        VT_CHECK(vmm_user_ptr_get_tag_u57(p) == tag,
                 "LAM_U57 tag round-trip");
    }
    VT_CHECK((vmm_pte_addr_mask & VMM_CR3_LAM_U48) == 0,
             "CR3.LAM_U48 outside PML4 phys mask");
    VT_CHECK((vmm_pte_addr_mask & VMM_CR3_LAM_U57) == 0,
             "CR3.LAM_U57 outside PML4 phys mask");

    kprintf("[VMM TEST] F: TME-MK KeyID encoder\n");
    {
        uint64_t phys_base = 0x0000000ABCDEF000ULL;
        for (uint8_t keyid = 0; keyid <= 0xF; keyid++) {
            uint64_t encoded = vmm_phys_with_keyid(phys_base, keyid, 4, 42);
            uint8_t recovered = (uint8_t)((encoded >> 42) & 0xF);
            VT_CHECK(recovered == keyid,
                     "vmm_phys_with_keyid round-trip");
            VT_CHECK((encoded & ((1ULL << 42) - 1ULL)) ==
                     (phys_base & ((1ULL << 42) - 1ULL)),
                     "low phys bits preserved");
        }
        uint64_t bad = vmm_phys_with_keyid(phys_base, 1, 15, 52);
        VT_CHECK(bad == phys_base,
                 "vmm_phys_with_keyid bound guard (15+52 > 64) → no-op");
    }

    kprintf("[VMM TEST] G: CET shadow-stack bit-60 encoder\n");
    {
        uint64_t base = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
        uint64_t with_ss = vmm_pte_with_cet_supv_ss(base);
        VT_CHECK((with_ss & VMM_PTE_CET_SS_SUPV) != 0,
                 "vmm_pte_with_cet_supv_ss sets bit 60");
        VT_CHECK(vmm_pte_is_supv_ss(with_ss),
                 "vmm_pte_is_supv_ss detects bit 60");
        VT_CHECK(!vmm_pte_is_supv_ss(base),
                 "vmm_pte_is_supv_ss false on unstamped PTE");
        VT_CHECK((vmm_pte_addr_mask & VMM_PTE_CET_SS_SUPV) == 0,
                 "CET SS bit 60 outside phys mask");
        VT_CHECK((MEMTAG_PTE_REGION_MASK & VMM_PTE_CET_SS_SUPV) == 0,
                 "CET SS bit 60 outside Phase 2D region bits");
    }

    if (fail == 0)
        kprintf("[VMM TEST] %[S]PASSED%[D]: all %zu checks OK\n", pass);
    else
        kprintf("[VMM TEST] %[R]FAILED%[D]: %zu pass, %zu fail\n", pass, fail);
}