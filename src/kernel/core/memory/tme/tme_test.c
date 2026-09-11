
#include "tme.h"
#include "vmm.h"
#include "klib.h"
#include "cpuid.h"

#define TME_CHECK(cond, label) \
    do { if (cond) { pass++; } \
         else { fail++; kprintf("[TME TEST]   %[R]FAIL%[D]: " label "\n"); } \
    } while (0)

static void test_tme_dormant_path(int *pass_p, int *fail_p) {
    int pass = 0, fail = 0;

    error_t rc = tme_init_bsp();
    TME_CHECK(rc == OK, "T1: tme_init_bsp idempotent + safe on any HW");

    if (!g_tme.mk_active) {
        uint16_t k = 0xFFFF;
        rc = tme_keyid_alloc(&k);
        TME_CHECK(rc == ERR_UNSUPPORTED, "T2: alloc on dormant MK -> UNSUPPORTED");
        TME_CHECK(k == 0xFFFF, "T2.1: out_keyid untouched on failure");

        rc = tme_keyid_free(1);
        TME_CHECK(rc == ERR_UNSUPPORTED, "T3: free on dormant MK -> UNSUPPORTED");
    }

    if (!g_tme.mk_active) {
        uint64_t phys = 0x12345000ULL;
        uint64_t out  = tme_phys_with_keyid(phys, 5);
        TME_CHECK(out == phys, "T4: phys_with_keyid identity when MK off");
        out = tme_phys_strip_keyid(phys);
        TME_CHECK(out == phys, "T4.1: phys_strip_keyid identity when MK off");
    }

    rc = tme_keyid_alloc(NULL);
    TME_CHECK(rc == ERR_INVALID_ARGUMENT || rc == ERR_UNSUPPORTED,
              "T5: alloc(NULL) -> INVALID_ARGUMENT or UNSUPPORTED");

    rc = tme_keyid_free(0);
    TME_CHECK(rc == ERR_INVALID_ARGUMENT || rc == ERR_UNSUPPORTED,
              "T5.1: free(KeyID=0) rejected (reserved platform default)");

    *pass_p += pass;
    *fail_p += fail;
}

static void test_tme_active_path(int *pass_p, int *fail_p) {
    if (!g_tme.mk_active) return;
    int pass = 0, fail = 0;

    TME_CHECK(g_tme.pool_size > 0, "A1: pool_size > 0 when MK active");
    TME_CHECK(g_tme.pool_programmed > 0,
              "A2: at least one KeyID programmed via PCONFIG");
    TME_CHECK(g_tme.num_keyid_bits > 0,
              "A3: num_keyid_bits > 0");
    TME_CHECK(g_tme.reduced_maxphyaddr <= vmm_maxphyaddr,
              "A4: reduced_maxphyaddr <= effective MAXPHYADDR");

    uint64_t phys = 0x00001000ULL;
    uint64_t with = tme_phys_with_keyid(phys, 1);
    TME_CHECK(with != phys,
              "A5: phys_with_keyid != raw phys for KeyID != 0");
    TME_CHECK(tme_phys_strip_keyid(with) == phys,
              "A6: strip_keyid recovers raw phys");

    uint16_t k1 = 0, k2 = 0;
    error_t rc = tme_keyid_alloc(&k1);
    TME_CHECK(rc == OK, "A7: tme_keyid_alloc 1st");
    TME_CHECK(k1 >= 1 && k1 <= g_tme.max_keyid, "A8: KeyID within pool range");

    rc = tme_keyid_alloc(&k2);
    TME_CHECK(rc == OK, "A9: tme_keyid_alloc 2nd");
    TME_CHECK(k2 != k1, "A10: distinct KeyIDs from concurrent alloc");

    rc = tme_keyid_free(k1);
    TME_CHECK(rc == OK, "A11: tme_keyid_free succeeds (PCONFIG SET_KEY_RANDOM)");

    uint16_t k3 = 0;
    rc = tme_keyid_alloc(&k3);
    TME_CHECK(rc == OK, "A12: re-alloc after free succeeds");

    (void)tme_keyid_free(k2);
    (void)tme_keyid_free(k3);

    rc = tme_keyid_free(k2);
    TME_CHECK(rc == ERR_INVALID_ARGUMENT,
              "A13: double-free returns INVALID_ARGUMENT");

    *pass_p += pass;
    *fail_p += fail;
}

void TmeRunTests(int *out_pass, int *out_fail) {
    int pass = 0, fail = 0;

    kprintf("[TME TEST] Starting (TME=%d MK=%d num_keyid_bits=%u max_keyid=%u)\n",
            (int)g_tme.tme_active,
            (int)g_tme.mk_active,
            (unsigned)g_tme.num_keyid_bits,
            (unsigned)g_tme.max_keyid);

    test_tme_dormant_path(&pass, &fail);
    test_tme_active_path(&pass, &fail);

    kprintf("[TME TEST] %d passed, %d failed\n", pass, fail);

    if (out_pass) *out_pass = pass;
    if (out_fail) *out_fail = fail;
}