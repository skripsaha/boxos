
#include "box/core/manifest.h"
#include "box/core/crate.h"
#include "box/core/notify.h"
#include "box/print.h"
#include "box/system.h"
#include "box/time.h"
#include "box/string.h"
#include "box/convert.h"

#define HTEST_ITERS  50u
#define BUF_BYTES    32u
#define MBUF_BYTES   (16u + 2u * (12u + 1u))

#define DECK_OPS     0x01u
#define OPS_FILL     0x02u
#define OPS_MOVE     0x01u

static int build_inner_manifest(uint8_t *mbuf, uint32_t mbuf_size,
                                uint8_t fill_byte)
{
    ManifestBuilder mb;
    if (ManifestBuilderInit(&mb, mbuf, mbuf_size) != 0)            return -1;
    if (ManifestBuilderAddOp(&mb, DECK_OPS, OPS_FILL, 0,
                             CRATE_INDEX_NONE, 0, &fill_byte, 1) != 0) return -2;
    if (ManifestBuilderAddOp(&mb, DECK_OPS, OPS_MOVE, 0, 0, 1, NULL, 0) != 0) return -3;
    if (ManifestBuilderFinalize(&mb) != 0)                          return -4;
    return 0;
}

static int verify_buf(const uint8_t *buf, uint8_t expected)
{
    for (uint32_t i = 0; i < BUF_BYTES; i++) {
        if (buf[i] != expected) return -1;
    }
    return 0;
}

int main(void)
{
    io_set_mode(IO_MODE_VGA);

    print("[htest] start — ");
    print_int(HTEST_ITERS);
    println(" iters per mode");

    static uint8_t mbuf[MBUF_BYTES];
    static uint8_t buf_a[BUF_BYTES];
    static uint8_t buf_b[BUF_BYTES];

    uint32_t passed_bytes   = 0;
    uint32_t passed_handle  = 0;
    uint64_t t_bytes_total  = 0;
    uint64_t t_handle_total = 0;

    for (uint32_t i = 0; i < HTEST_ITERS; i++) {
        uint8_t fill = (uint8_t)(0xA0u + (i & 0x0Fu));
        if (build_inner_manifest(mbuf, sizeof(mbuf), fill) != 0) {
            println("[htest] FAIL — bytes build");
            exit(1);
        }
        memset(buf_a, 0x11, sizeof(buf_a));
        memset(buf_b, 0x22, sizeof(buf_b));

        Crate crates[2];
        CrateSetOutput(&crates[0], buf_a, sizeof(buf_a));
        CrateSetOutput(&crates[1], buf_b, sizeof(buf_b));

        uint64_t t0 = 0, t1 = 0;
        time_uptime_ms(&t0);
        Result r;
        int rc = ManifestSubmit((Manifest *)mbuf, crates, 2, &r);
        time_uptime_ms(&t1);
        t_bytes_total += (t1 - t0);

        if (rc != OK || r.error_code != OK)              continue;
        if (verify_buf(buf_b, fill) != 0)                continue;
        passed_bytes++;
    }

    if (passed_bytes != HTEST_ITERS) {
        print("[htest] FAIL — bytes mode passed ");
        print_int((int)passed_bytes);
        print(" / ");
        print_int((int)HTEST_ITERS);
        println("");
        exit(1);
    }
    println("[htest] bytes-mode 50/50 OK");

    for (uint32_t i = 0; i < HTEST_ITERS; i++) {
        uint8_t fill = (uint8_t)(0xA0u + (i & 0x0Fu));
        if (build_inner_manifest(mbuf, sizeof(mbuf), fill) != 0) {
            println("[htest] FAIL — handle build");
            exit(1);
        }
        memset(buf_a, 0x11, sizeof(buf_a));
        memset(buf_b, 0x22, sizeof(buf_b));

        ManifestHandle handle = 0;
        if (ManifestCompileHandle((Manifest *)mbuf, &handle) != OK || handle == 0) {
            println("[htest] FAIL — compile");
            exit(1);
        }

        Crate crates[2];
        CrateSetOutput(&crates[0], buf_a, sizeof(buf_a));
        CrateSetOutput(&crates[1], buf_b, sizeof(buf_b));

        uint64_t t0 = 0, t1 = 0;
        time_uptime_ms(&t0);
        Result r;
        int rc = ManifestSubmitHandle(handle, crates, 2, &r);
        time_uptime_ms(&t1);
        t_handle_total += (t1 - t0);

        bool ok = (rc == OK) && (r.error_code == OK) && (verify_buf(buf_b, fill) == 0);

        (void)ManifestReleaseHandle(handle);

        if (!ok) continue;
        passed_handle++;
    }

    if (passed_handle != HTEST_ITERS) {
        print("[htest] FAIL — handle mode passed ");
        print_int((int)passed_handle);
        print(" / ");
        print_int((int)HTEST_ITERS);
        println("");
        exit(1);
    }
    println("[htest] handle-mode 50/50 OK");

    {
        uint8_t fill = 0xC3u;
        if (build_inner_manifest(mbuf, sizeof(mbuf), fill) != 0) {
            println("[htest] FAIL — revoke build");
            exit(1);
        }
        ManifestHandle handle = 0;
        if (ManifestCompileHandle((Manifest *)mbuf, &handle) != OK || handle == 0) {
            println("[htest] FAIL — revoke compile");
            exit(1);
        }
        if (ManifestReleaseHandle(handle) != OK) {
            println("[htest] FAIL — revoke release");
            exit(1);
        }

        memset(buf_a, 0x11, sizeof(buf_a));
        memset(buf_b, 0x22, sizeof(buf_b));
        Crate crates[2];
        CrateSetOutput(&crates[0], buf_a, sizeof(buf_a));
        CrateSetOutput(&crates[1], buf_b, sizeof(buf_b));

        Result r;
        int rc = ManifestSubmitHandle(handle, crates, 2, &r);
        bool revoked = (rc != OK) || (r.error_code != OK);
        if (!revoked) {
            println("[htest] FAIL — post-release submit succeeded (handle not revoked)");
            exit(1);
        }
        println("[htest] post-release revocation OK");
    }

    print("[htest] bytes total ms=");
    print_int((int)t_bytes_total);
    print(" handle total ms=");
    print_int((int)t_handle_total);
    println("");
    println("[htest] PASS");
    return 0;
}