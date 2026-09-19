/*
 * VMM Helper + Probe tests
 *
 * Covers vmm_pte_* encoder/decoder helpers, vmm_*_probe consistency
 * checks, and Phase 2D-2K bit-window helpers. These were previously
 * mixed into memtag_test.c; moved here in the clean-slate audit because
 * they're VMM-level helpers with no MemTag-specific state. MemTag's
 * own test file now covers only the TagFS-shaped namespace, regions,
 * and capability enforcement.
 */

#include "vmm.h"
#include "klib.h"
#include "cpuid.h"
#include "memtag.h"   /* MEMTAG_PTE_REGION_MASK — verify orthogonality */

#define VT_CHECK(cond, label) \
    do { if (cond) { pass++; } \
         else { fail++; kprintf("[VMM TEST]   %[R]FAIL%[D]: " label "\n"); } \
    } while (0)

void VmmHelperTest(void) {
    kprintf("[VMM TEST] Starting VMM helper + probe test...\n");
    size_t pass = 0, fail = 0;

    /* ── A: PTE-bits-52-58 metadata probe (Phase 2D) ───────────────── */
    kprintf("[VMM TEST] A: PTE bits 52-58 metadata probe\n");
    VT_CHECK(vmm_verify_pte_metadata_bits_52_58(),
             "bits 52-58 SAFE on current CPU");

    /* ── B: PAT MSR consistency probe (Phase 2E) ────────────────────── */
    kprintf("[VMM TEST] B: PAT MSR consistency probe\n");
    VT_CHECK(vmm_verify_pat_msr(),
             "vmm_verify_pat_msr returns true on BSP");
    VT_CHECK(vmm_get_pat_msr_value() != 0,
             "vmm_get_pat_msr_value() non-zero (programmed by vmm_pat_init)");

    /* ── C: PTE → cache-type decoder (Phase 2E) ─────────────────────── */
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

    /* ── D: PKU PTE bits 62:59 encoder + decoder ──────────────────── */
    kprintf("[VMM TEST] D: PKU bit-62:59 encoder + decoder\n");
    for (uint8_t k = 0; k <= VMM_PTE_PKEY_MAX; k++) {
        uint64_t enc = vmm_pte_encode_pkey(k);
        VT_CHECK(vmm_pte_pkey(enc | VMM_FLAG_PRESENT) == k,
                 "vmm_pte_encode_pkey ↔ vmm_pte_pkey round-trip");
    }
    if (g_cpu_caps.has_pku) {
        (void)vmm_read_pkru();  /* gate test — no #GP after vmm_pku_init */
        VT_CHECK(true, "RDPKRU executed without #GP");
    } else {
        VT_CHECK(vmm_read_pkru() == 0,
                 "vmm_read_pkru returns 0 on non-PKU CPU");
    }

    /* ── E: LAM pointer-tag helpers (Phase 2I) ─────────────────────── */
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

    /* ── F: TME-MK KeyID encoder (Phase 2J) ────────────────────────── */
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
        /* Bound guard (audit-1): num+max > 64 returns phys unchanged */
        uint64_t bad = vmm_phys_with_keyid(phys_base, 1, 15, 52);
        VT_CHECK(bad == phys_base,
                 "vmm_phys_with_keyid bound guard (15+52 > 64) → no-op");
    }

    /* ── G: shadow-stack leaf encoding (Intel SDM Vol 3A §4.6) ─────── */
    kprintf("[VMM TEST] G: shadow-stack leaf encoding\n");
    {
        /* "A linear address is a shadow-stack address if ... the R/W flag
         * (bit 1) is 0 and the dirty flag (bit 6) is 1 in the paging-
         * structure entry that maps the page" — U/S then picks the ring.
         * There is no shadow-stack flag bit; bit 60 is protection-key
         * bit 1 and never meant anything else in ordinary paging. */
        uint64_t supv = VMM_FLAGS_SHADOW_STACK_SUPV;
        uint64_t user = VMM_FLAGS_SHADOW_STACK_USER;
        VT_CHECK((supv & VMM_FLAG_WRITABLE) == 0, "supervisor SS is R/W=0");
        VT_CHECK((supv & VMM_FLAG_DIRTY) != 0,    "supervisor SS is D=1");
        VT_CHECK((supv & VMM_FLAG_PRESENT) != 0,  "supervisor SS is present");
        VT_CHECK((supv & VMM_FLAG_USER) == 0,     "supervisor SS is U/S=0");
        VT_CHECK((user & VMM_FLAG_WRITABLE) == 0, "user SS is R/W=0");
        VT_CHECK((user & VMM_FLAG_DIRTY) != 0,    "user SS is D=1");
        VT_CHECK((user & VMM_FLAG_USER) != 0,     "user SS is U/S=1");
        /* Both bits must survive the flag mask vmm_make_pte applies, or
         * a fresh mapping would come out as ordinary read-only memory. */
        VT_CHECK((supv & VMM_PTE_FLAGS_MASK) == supv,
                 "supervisor SS flags survive vmm_make_pte's mask");
        VT_CHECK((user & VMM_PTE_FLAGS_MASK) == user,
                 "user SS flags survive vmm_make_pte's mask");
        /* The encoding lives entirely below bit 12, so MemTag's region
         * window (52-58) and the PKEY field (62:59) cannot disturb it. */
        VT_CHECK((MEMTAG_PTE_REGION_MASK & (VMM_FLAG_WRITABLE | VMM_FLAG_DIRTY)) == 0,
                 "shadow-stack bits outside Phase 2D region bits");
        VT_CHECK((VMM_PTE_PKEY_MASK & (VMM_FLAG_WRITABLE | VMM_FLAG_DIRTY)) == 0,
                 "shadow-stack bits outside the PKEY field");
    }

    if (fail == 0)
        kprintf("[VMM TEST] %[S]PASSED%[D]: all %zu checks OK\n", pass);
    else
        kprintf("[VMM TEST] %[R]FAILED%[D]: %zu pass, %zu fail\n", pass, fail);
}
