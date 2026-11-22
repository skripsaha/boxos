/*
 * decks — full-deck stress test.
 *
 * Walks every deck (Operations / Storage / Hardware / System) and exercises
 * one or two representative ops via the public boxlib API + ManifestSubmit
 * for the lower-level checks. Prints PASS/FAIL per test and a final tally.
 *
 * Goal: end-to-end smoke on every code path the Phase 8/11/12/13 work
 * touched, plus the security gate (each test runs under decks.elf's tags
 * `app,utility` — utility-level reach, no system).
 */

#include "box/print.h"
#include "box/file.h"
#include "box/system.h"
#include "box/time.h"
#include "box/core/manifest.h"
#include "box/core/crate.h"
#include "box/core/notify.h"
#include "box/string.h"
#include "box/convert.h"
#include "box/error.h"

static int g_passed = 0;
static int g_failed = 0;

static void check(const char *label, bool ok)
{
    if (ok) { print("[decks] PASS  "); g_passed++; }
    else    { print("[decks] FAIL  "); g_failed++; }
    println(label);
}

/* -------------------------------------------------------------------------
 *  Operations Deck — fill + move + hash + cmp in one Manifest
 * ------------------------------------------------------------------------- */

static bool test_operations(void)
{
    /* Manifest:
     *   1. ops.fill     out=crate[0], param=0xAB
     *   2. ops.move     in=crate[0], out=crate[1]
     *   3. ops.cmp      in=crate[0], out=crate[2]   second-input crate index = 1 (in op->params)
     *   crate[2] capacity 1 byte; cmp writes 0 if equal */
    static uint8_t mbuf[256];
    ManifestBuilder mb;
    if (ManifestBuilderInit(&mb, mbuf, sizeof(mbuf)) != 0) return false;

    uint8_t fill = 0xABu;
    if (ManifestBuilderAddOp(&mb, DECK_OPERATIONS, OP_BUF_FILL, 0,
                             CRATE_INDEX_NONE, 0, &fill, 1) != 0) return false;
    if (ManifestBuilderAddOp(&mb, DECK_OPERATIONS, OP_BUF_MOVE, 0,
                             0, 1, NULL, 0) != 0) return false;
    if (ManifestBuilderFinalize(&mb) != 0) return false;

    static uint8_t a[64], b[64];
    memset(a, 0x11, sizeof(a));
    memset(b, 0x22, sizeof(b));

    Crate crates[2];
    CrateSetOutput(&crates[0], a, sizeof(a));
    CrateSetOutput(&crates[1], b, sizeof(b));

    Result r;
    int rc = ManifestSubmit((Manifest *)mbuf, crates, 2, &r);
    if (rc != 0 || r.error_code != 0) return false;

    for (size_t i = 0; i < sizeof(a); i++) {
        if (a[i] != 0xAB || b[i] != 0xAB) return false;
    }
    return true;
}

/* -------------------------------------------------------------------------
 *  Hardware Deck — timer + RTC
 * ------------------------------------------------------------------------- */

static bool test_hw_timer_monotonic(void)
{
    uint64_t t1 = 0, t2 = 0;
    if (time_uptime_ms(&t1) != 0) return false;
    /* burn a small spin so the second sample is later */
    for (volatile uint32_t i = 0; i < 200000; i++) { __asm__ volatile(""); }
    if (time_uptime_ms(&t2) != 0) return false;
    return t2 >= t1;
}

static bool test_hw_rtc_unix(void)
{
    uint64_t secs = 0;
    if (time_get_secs(&secs) != 0) return false;
    /* QEMU RTC is post-epoch — must be > 1 Jan 2000. */
    return secs > 946684800ULL;
}

/* -------------------------------------------------------------------------
 *  System Deck — proc.info self, tag.check, perf
 * ------------------------------------------------------------------------- */

static bool test_sys_proc_info_self(void)
{
    proc_info_t info = {0};
    if (proc_info(0, &info) != 0) return false;
    return info.pid != 0;
}

static bool test_sys_tag_check(void)
{
    bool has = false;
    if (proc_tag_check("app", &has) != 0) return false;
    /* Every program is an app: it wears the key whatever its file is tagged. */
    return has;
}

static bool test_sys_tag_check_negative(void)
{
    bool has = true;
    if (proc_tag_check("nosuchtag", &has) != 0) return false;
    return has == false;
}

/* -------------------------------------------------------------------------
 *  Storage Deck — query (list all files) + getinfo on the first of them
 * ------------------------------------------------------------------------- */

static bool test_storage_query(void)
{
    file_id_t ids[64];
    int n = query(MY_TAGS, NULL, NULL, ids, 64);
    return n > 0 && n <= 64;
}

static bool test_storage_getinfo(void)
{
    file_id_t ids[64];
    int n = query(MY_TAGS, NULL, NULL, ids, 64);
    if (n <= 0) return false;
    /* Just take the first id and check we can pull metadata. */
    file_info_t info = {0};
    if (file_info(MY_TAGS, ids[0], &info) != 0) return false;
    return info.file_id == ids[0] && info.filename[0] != '\0';
}

/* -------------------------------------------------------------------------
 *  Two different refusals, and telling them apart is the point
 *
 *  ‼ THE ERROR CODE IS THE ASSERTION, NOT "SOMETHING WENT WRONG". A test that
 *  accepts any non-zero answer passes for the wrong reason the moment the op
 *  it names stops existing — which is exactly what happened to the op this
 *  pair replaces. It aimed at hw.port.outb, and when hw.port.outb was
 *  withdrawn the test went on passing while proving nothing about the gate.
 *  So each of these two names the code it expects:
 *
 *    ERR_ACCESS_DENIED   — the op is there, and the gate would not let US
 *                          have it. That is the gate working.
 *    ERR_INVALID_OPCODE  — there is no such op on this deck at all. That is
 *                          the registry knowing what it does not offer.
 * ------------------------------------------------------------------------- */

static bool test_auth_denies_system_op(void)
{
    /* hw.usb.port.query is registered with OP_AUTH_SYSTEM; decks.elf carries
     * app+utility, so the kernel must refuse it. Chosen because it is
     * read-only and takes the controller number in one byte: if the gate ever
     * DID fail open, the worst that happens is five bytes written into our own
     * crate — and the answer would then not be ERR_ACCESS_DENIED, so this
     * still fails, loudly, which is what a negative test is for. */
    static uint8_t  mbuf[64];
    static uint8_t  out[8];
    ManifestBuilder mb;
    if (ManifestBuilderInit(&mb, mbuf, sizeof(mbuf)) != 0) return false;

    uint8_t params[1] = { 0 };                 /* the first controller */
    if (ManifestBuilderAddOp(&mb, DECK_HARDWARE, HW_USB_PORT_QUERY, 0,
                             CRATE_INDEX_NONE, 0,
                             params, sizeof(params)) != 0) return false;
    if (ManifestBuilderFinalize(&mb) != 0) return false;

    Crate crates[1];
    CrateSetOutput(&crates[0], out, sizeof(out));

    Result r;
    int rc = ManifestSubmit((Manifest *)mbuf, crates, 1, &r);
    if (rc < 0) return false;                  /* never reached the gate */
    return r.error_code == (uint32_t)ERR_ACCESS_DENIED;
}

static bool test_unnamed_opcode_is_unknown(void)
{
    /* No section of the hardware deck reaches 0xFF (boxos_decks.h), so the
     * deck must answer that there is no such op — a different refusal from
     * the one above, and the difference is the whole reason both tests
     * exist. */
    static uint8_t mbuf[64];
    ManifestBuilder mb;
    if (ManifestBuilderInit(&mb, mbuf, sizeof(mbuf)) != 0) return false;

    uint8_t params[3] = { 0x80, 0x00, 0x00 };
    if (ManifestBuilderAddOp(&mb, DECK_HARDWARE, 0xFF /* named by no section */, 0,
                             CRATE_INDEX_NONE, CRATE_INDEX_NONE,
                             params, sizeof(params)) != 0) return false;
    if (ManifestBuilderFinalize(&mb) != 0) return false;

    Result r;
    int rc = ManifestSubmit((Manifest *)mbuf, NULL, 0, &r);
    if (rc < 0) return false;
    return r.error_code == (uint32_t)ERR_INVALID_OPCODE;
}

/* -------------------------------------------------------------------------
 *  Driver
 * ------------------------------------------------------------------------- */

int main(void)
{
    /* Standalone — said straight into the log ring, no display server. */
    io_set_mode(IO_MODE_RING);

    println("=== BoxOS Decks Stress Test ===");

    check("Operations: fill + move",   test_operations());
    check("Hardware:   timer.ms",      test_hw_timer_monotonic());
    check("Hardware:   rtc.unix64",    test_hw_rtc_unix());
    check("System:     proc.info self",test_sys_proc_info_self());
    check("System:     tag.check (+)", test_sys_tag_check());
    check("System:     tag.check (-)", test_sys_tag_check_negative());
    check("Storage:    query all",     test_storage_query());
    check("Storage:    getinfo",       test_storage_getinfo());
    check("Auth gate:  deny usb.port.query", test_auth_denies_system_op());
    check("Unnamed:    hw 0xFF is no op",     test_unnamed_opcode_is_unknown());

    print("[decks] ");
    print_int(g_passed);
    print(" passed, ");
    print_int(g_failed);
    println(" failed");

    exit(g_failed == 0 ? 0 : 1);
}
