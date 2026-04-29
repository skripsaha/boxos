/*
 * mtest — Manifest-mode end-to-end demonstration.
 *
 * Builds a tiny Manifest with two operations on the Operations Deck:
 *   1. ops.fill   — fill crate[0] with byte 0xAB
 *   2. ops.move   — copy crate[0] -> crate[1]
 *
 * Submits via ManifestSubmit. Verifies both buffers contain 0xAB on return.
 *
 * Proves the whole chain end-to-end:
 *   userspace builder -> POCKET_FLAG_MANIFEST -> guide_process_manifest_pocket
 *   -> ManifestExecuteOnce -> OpRegistryLookup -> OpBufFill / OpBufMove
 *   -> Result back into ResultRing.
 */

#include "box/manifest.h"
#include "box/print.h"
#include "box/system.h"
#include "box/string.h"
#include "box/notify.h"

#define BUF_BYTES   64u
#define MBUF_BYTES  256u

#define OPS_FILL  0x02u
#define OPS_MOVE  0x01u

int main(void)
{
    static uint8_t  mbuf[MBUF_BYTES];
    static uint8_t  buf_a[BUF_BYTES];
    static uint8_t  buf_b[BUF_BYTES];

    /* Pre-poison so we can detect untouched bytes. */
    memset(buf_a, 0x11, sizeof(buf_a));
    memset(buf_b, 0x22, sizeof(buf_b));

    ManifestBuilder mb;
    if (ManifestBuilderInit(&mb, mbuf, sizeof(mbuf)) != 0) {
        print("[mtest] builder init fail\n");
        exit(1);
    }

    uint8_t fill_byte = 0xABu;
    if (ManifestBuilderAddOp(&mb, DECK_OPERATIONS, OPS_FILL, 0,
                             CRATE_INDEX_NONE, 0, &fill_byte, 1) != 0) {
        print("[mtest] add fill fail\n");
        exit(1);
    }
    if (ManifestBuilderAddOp(&mb, DECK_OPERATIONS, OPS_MOVE, 0,
                             0, 1, NULL, 0) != 0) {
        print("[mtest] add move fail\n");
        exit(1);
    }
    if (ManifestBuilderFinalize(&mb) != 0) {
        print("[mtest] finalize fail\n");
        exit(1);
    }

    Crate crates[2];
    CrateSetOutput(&crates[0], buf_a, sizeof(buf_a));
    CrateSetOutput(&crates[1], buf_b, sizeof(buf_b));

    Result r;
    int rc = ManifestSubmit((Manifest *)mbuf, crates, 2, &r);
    if (rc != 0) {
        print("[mtest] submit failed\n");
        exit(1);
    }

    bool ok_a = true;
    bool ok_b = true;
    for (uint32_t i = 0; i < BUF_BYTES; i++) {
        if (buf_a[i] != 0xABu) { ok_a = false; break; }
    }
    for (uint32_t i = 0; i < BUF_BYTES; i++) {
        if (buf_b[i] != 0xABu) { ok_b = false; break; }
    }

    if (!(ok_a && ok_b)) {
        print("[mtest] FAIL — buffer contents wrong\n");
        exit(1);
    }
    print("[mtest] PASS — fill+move via Manifest worked\n");

    /* Phase 2 of demo: print a message via Hardware Deck PUTSTRING using the
     * Manifest path — proves hw ops are reachable through the new dispatch. */
    static uint8_t mbuf2[128];
    static const char hw_msg[] = "[mtest] HW VGA via Manifest works\n";
    uint8_t  hw_params[2] = { 0x07u, 0x00u };  /* color=light gray, flags=0 */

    ManifestBuilder mb2;
    if (ManifestBuilderInit(&mb2, mbuf2, sizeof(mbuf2)) != 0) exit(1);
    if (ManifestBuilderAddOp(&mb2, DECK_HARDWARE, 0x71u /* HW_VGA_PUTSTRING */,
                             0, 0, CRATE_INDEX_NONE,
                             hw_params, sizeof(hw_params)) != 0) exit(1);
    if (ManifestBuilderFinalize(&mb2) != 0) exit(1);

    Crate vga_in;
    /* PUTSTRING reads in_crate.size bytes; do not include the trailing NUL. */
    CrateSetInput(&vga_in, (void *)hw_msg, sizeof(hw_msg) - 1);

    Result r2;
    int rc2 = ManifestSubmit((Manifest *)mbuf2, &vga_in, 1, &r2);
    if (rc2 != 0) {
        print("[mtest] HW manifest submit failed\n");
        exit(1);
    }

    exit(0);
}
