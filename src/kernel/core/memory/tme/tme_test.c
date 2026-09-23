/*
 * TME — Total Memory Encryption + Multi-Key tests.
 *
 * Exercises the KeyID pool lifecycle, PCONFIG state, and dormant-path
 * fallback. On QEMU TCG (no TME-MK) verifies that all allocation
 * paths return ERR_UNSUPPORTED and don't crash. On real silicon with
 * TME-MK locked active, exercises the full alloc/free round-trip and
 * confirms the bitmap accounting + re-key flow.
 *
 * Only one of those two groups can run on any given machine, and the
 * other is COUNTED, not dropped: pass + fail + unrun is the same number
 * on every machine this kernel boots on. That is what keeps the summary
 * from saying the same green thing on a CPU that encrypts memory and on
 * one that has never heard of it — which is exactly what it used to say.
 *
 * Touch publish: every test failure is printed; the function returns
 * pass/fail counts via stat block.
 */

#include "tme.h"
#include "vmm.h"
#include "klib.h"
#include "cpuid.h"

#define TME_CHECK(cond, label) \
    do { if (cond) { pass++; } \
         else { fail++; kprintf("[TME TEST]   %[R]FAIL%[D]: " label "\n"); } \
    } while (0)

/* The mirror of the one below: a check only a machine WITHOUT a usable key
 * pool can answer. On silicon whose pool is live there is no refusal to
 * observe, so the check is counted as unrun rather than quietly dropped —
 * which is what used to happen, and what let a summary of eight say the
 * same thing on both kinds of machine. */
#define TME_CHECK_DORMANT(cond, label) \
    do { if (live) { skip++; } \
         else if (cond) { pass++; } \
         else { fail++; kprintf("[TME TEST]   %[R]FAIL%[D]: " label "\n"); } \
    } while (0)

/* A check that only a live key pool can answer.
 *
 * Without one the condition is NOT evaluated and the check is counted as
 * unrun: a question this machine was never asked must not end up inside
 * the number of questions it answered. The count comes from the checks
 * themselves, so it cannot drift away from what is written below — which
 * is the whole reason it is a macro and not a number in the summary. */
#define TME_CHECK_POOL(cond, label) \
    do { if (!live) { skip++; } \
         else if (cond) { pass++; } \
         else { fail++; kprintf("[TME TEST]   %[R]FAIL%[D]: " label "\n"); } \
    } while (0)

static void test_tme_dormant_path(int *pass_p, int *fail_p, int *skip_p) {
    int pass = 0, fail = 0, skip = 0;
    const bool live = g_tme.mk_active;

    /* Init must succeed regardless of hardware support. */
    error_t rc = tme_init_bsp();
    TME_CHECK(rc == OK, "T1: tme_init_bsp idempotent + safe on any HW");

    /* Calling alloc when MK inactive must return ERR_UNSUPPORTED, not crash.
     * The call itself is made only on a dormant machine: on a live one it
     * would take a KeyID out of the pool to ask a question the pool cannot
     * answer. */
    uint16_t k = 0xFFFF;
    if (!live) rc = tme_keyid_alloc(&k);
    TME_CHECK_DORMANT(rc == ERR_UNSUPPORTED, "T2: alloc on dormant MK -> UNSUPPORTED");
    TME_CHECK_DORMANT(k == 0xFFFF, "T2.1: out_keyid untouched on failure");

    if (!live) rc = tme_keyid_free(1);
    TME_CHECK_DORMANT(rc == ERR_UNSUPPORTED, "T3: free on dormant MK -> UNSUPPORTED");

    /* phys_with_keyid is an identity when MK is off. */
    uint64_t phys = 0x12345000ULL;
    uint64_t out  = tme_phys_with_keyid(phys, 5);
    TME_CHECK_DORMANT(out == phys, "T4: phys_with_keyid identity when MK off");
    out = tme_phys_strip_keyid(phys);
    TME_CHECK_DORMANT(out == phys, "T4.1: phys_strip_keyid identity when MK off");

    /* Bogus args reject. */
    rc = tme_keyid_alloc(NULL);
    TME_CHECK(rc == ERR_INVALID_ARGUMENT || rc == ERR_UNSUPPORTED,
              "T5: alloc(NULL) -> INVALID_ARGUMENT or UNSUPPORTED");

    rc = tme_keyid_free(0);
    TME_CHECK(rc == ERR_INVALID_ARGUMENT || rc == ERR_UNSUPPORTED,
              "T5.1: free(KeyID=0) rejected (reserved platform default)");

    *pass_p += pass;
    *fail_p += fail;
    *skip_p += skip;
}

/* The pool's own path. It used to return at the door when there was no
 * pool, and the checks below then existed for nobody: the summary counted
 * the dormant path's eight and said "0 failed", which reads on a machine
 * without memory encryption exactly as it reads on a machine with it. The
 * door is gone; `live` decides per check whether it is answered or counted
 * as unrun. Every call here is refused by tme.c itself while MK is
 * inactive (mk_active is the second line of each one), so nothing below
 * touches silicon that is not there. */
static void test_tme_active_path(int *pass_p, int *fail_p, int *skip_p) {
    int pass = 0, fail = 0, skip = 0;
    const bool live = g_tme.mk_active;

    /* Pool sanity. */
    TME_CHECK_POOL(g_tme.pool_size > 0, "A1: pool_size > 0 when MK active");
    TME_CHECK_POOL(g_tme.pool_programmed > 0,
              "A2: at least one KeyID programmed via PCONFIG");
    TME_CHECK_POOL(g_tme.num_keyid_bits > 0,
              "A3: num_keyid_bits > 0");
    TME_CHECK_POOL(g_tme.reduced_maxphyaddr <= vmm_maxphyaddr,
              "A4: reduced_maxphyaddr <= effective MAXPHYADDR");

    /* phys_with_keyid embeds non-zero KeyID into upper bits. */
    uint64_t phys = 0x00001000ULL;
    uint64_t with = tme_phys_with_keyid(phys, 1);
    TME_CHECK_POOL(with != phys,
              "A5: phys_with_keyid != raw phys for KeyID != 0");
    TME_CHECK_POOL(tme_phys_strip_keyid(with) == phys,
              "A6: strip_keyid recovers raw phys");

    /* Round-trip: alloc, free. */
    uint16_t k1 = 0, k2 = 0;
    error_t rc = tme_keyid_alloc(&k1);
    TME_CHECK_POOL(rc == OK, "A7: tme_keyid_alloc 1st");
    TME_CHECK_POOL(k1 >= 1 && k1 <= g_tme.max_keyid, "A8: KeyID within pool range");

    rc = tme_keyid_alloc(&k2);
    TME_CHECK_POOL(rc == OK, "A9: tme_keyid_alloc 2nd");
    TME_CHECK_POOL(k2 != k1, "A10: distinct KeyIDs from concurrent alloc");

    /* Free and re-alloc — slot should be reusable (after re-key). */
    rc = tme_keyid_free(k1);
    TME_CHECK_POOL(rc == OK, "A11: tme_keyid_free succeeds (PCONFIG SET_KEY_RANDOM)");

    uint16_t k3 = 0;
    rc = tme_keyid_alloc(&k3);
    TME_CHECK_POOL(rc == OK, "A12: re-alloc after free succeeds");
    /* k3 may or may not equal k1 — the bitmap is linear-scan, so the
     * lowest free slot wins. Either way, the slot is freshly re-keyed. */

    /* Cleanup */
    (void)tme_keyid_free(k2);
    (void)tme_keyid_free(k3);

    /* Double-free rejected. */
    rc = tme_keyid_free(k2);
    TME_CHECK_POOL(rc == ERR_INVALID_ARGUMENT,
              "A13: double-free returns INVALID_ARGUMENT");

    *pass_p += pass;
    *fail_p += fail;
    *skip_p += skip;
}

void TmeRunTests(int *out_pass, int *out_fail) {
    int pass = 0, fail = 0, skip = 0;

    /* Say which of the four machines this is BEFORE any check runs. The
     * old line listed four fields and left the reader to work out that
     * TME=0 MK=0 means every encryption check below is about refusals. */
    if (!g_cpu_caps.has_tme) {
        kprintf("[TME TEST] this processor does not implement Total Memory "
                "Encryption at all, so DRAM here is written in the clear and "
                "no key can be programmed: what follows can only measure "
                "whether this kernel refuses every key request instead of "
                "pretending to serve one\n");
    } else if (!g_tme.tme_active) {
        kprintf("[TME TEST] this processor implements Total Memory Encryption "
                "and the firmware left it switched off, so DRAM here is "
                "written in the clear: what follows can only measure this "
                "kernel's refusal path\n");
    } else if (!g_tme.mk_active) {
        kprintf("[TME TEST] this machine encrypts all of DRAM under the one "
                "platform key its firmware locked in, but Multi-Key is not "
                "usable here (%u KeyID bit(s) in a physical address), so no "
                "region can be given a key of its own: what follows can only "
                "measure this kernel's refusal path\n",
                (unsigned)g_tme.num_keyid_bits);
    } else {
        kprintf("[TME TEST] this machine encrypts DRAM and holds a pool of "
                "%u KeyID(s), %u of them already programmed with a key the "
                "processor generated, spending %u physical-address bit(s) on "
                "the field: the pool below is exercised on real silicon\n",
                (unsigned)g_tme.max_keyid, (unsigned)g_tme.pool_programmed,
                (unsigned)g_tme.num_keyid_bits);
    }

    test_tme_dormant_path(&pass, &fail, &skip);
    test_tme_active_path(&pass, &fail, &skip);

    if (fail > 0) {
        kprintf("[TME TEST] %[R]FAILED%[D]: %d answered, %d failed, and %d "
                "that this machine cannot be asked were not run\n",
                pass, fail, skip);
    } else if (!g_tme.mk_active) {
        kprintf("[TME TEST] %[Y]SKIPPED%[D]: %d check(s) answered, every one "
                "of them about a refusal, and the %d that need a key pool "
                "were not run — nothing on this machine was ever encrypted "
                "under a key of its own, and this line is not evidence that "
                "the pool works\n", pass, skip);
    } else {
        kprintf("[TME TEST] %[S]PASSED%[D]: %d check(s) answered on this "
                "machine's own key pool — allocated from, freed to and "
                "re-keyed through PCONFIG — and %d that only a machine "
                "without a pool can answer were not run\n", pass, skip);
    }

    if (out_pass) *out_pass = pass;
    if (out_fail) *out_fail = fail;
}
