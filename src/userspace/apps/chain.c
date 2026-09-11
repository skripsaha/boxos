
#include "box/print.h"
#include "box/core/manifest.h"
#include "box/core/crate.h"
#include "box/core/notify.h"
#include "box/system.h"
#include "box/time.h"
#include "box/string.h"
#include "box/convert.h"

#define CHAIN_OPS         1000
#define CHAIN_BUF_BYTES   64
#define CHAIN_MBUF_BYTES  (16 + CHAIN_OPS * 13 + 64)

#define OP_BUF_FILL  0x02

static uint8_t  s_mbuf[CHAIN_MBUF_BYTES];
static uint8_t  s_buf[CHAIN_BUF_BYTES];

int main(void)
{
    io_set_mode(IO_MODE_VGA);

    print("[chain] building ");
    print_int(CHAIN_OPS);
    println("-op Manifest...");

    ManifestBuilder mb;
    if (ManifestBuilderInit(&mb, s_mbuf, sizeof(s_mbuf)) != 0) {
        println("[chain] FAIL — builder init");
        exit(1);
    }

    for (int i = 0; i < CHAIN_OPS; i++) {
        uint8_t fill_byte = (uint8_t)i;
        if (ManifestBuilderAddOp(&mb,
                                 DECK_OPERATIONS, OP_BUF_FILL,
                                 0,
                                 CRATE_INDEX_NONE,
                                 0,
                                 &fill_byte, 1) != 0) {
            print("[chain] FAIL — add op #");
            print_int(i);
            println("");
            exit(1);
        }
    }

    if (ManifestBuilderFinalize(&mb) != 0) {
        println("[chain] FAIL — finalize");
        exit(1);
    }

    memset(s_buf, 0x33, sizeof(s_buf));

    Crate crates[1];
    CrateSetOutput(&crates[0], s_buf, sizeof(s_buf));

    uint64_t t0 = 0, t1 = 0;
    time_uptime_ms(&t0);

    Result r;
    int rc = ManifestSubmit((Manifest *)s_mbuf, crates, 1, &r);

    time_uptime_ms(&t1);

    if (rc != 0 || r.error_code != 0) {
        print("[chain] FAIL — submit rc=");
        print_int(rc);
        print(" err=");
        print_int((int)r.error_code);
        println("");
        exit(1);
    }

    uint8_t expected = (uint8_t)((CHAIN_OPS - 1) & 0xFF);
    bool ok = true;
    for (int i = 0; i < CHAIN_BUF_BYTES; i++) {
        if (s_buf[i] != expected) { ok = false; break; }
    }

    if (ok) {
        print("[chain] PASS — ");
        print_int(CHAIN_OPS);
        print(" ops in one syscall (");
        print_int((int)(t1 - t0));
        println(" ms)");
        exit(0);
    } else {
        print("[chain] FAIL — buffer mismatch (expected 0x");
        print_hex(expected);
        println(")");
        exit(1);
    }
}