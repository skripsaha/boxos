/*
 * write_stress — exercises the Stage 4 async ObjWrite path:
 *
 *   S1  multi-block write (16 KB across 4 disk blocks)
 *   S2  same-file overwrite churn (200 sequential writes through token handoff)
 *   S3  append loop (10× 64-byte appends → 640-byte file, alloc + meta-commit)
 *
 * Each scenario writes deterministic content and reads it back to verify
 * the bytes survived the async pump (W_LOCATE → W_DMA_FILL → W_AHCI_SUBMIT
 * → W_AHCI_DONE → W_LOG_META → W_PUBLISH → W_RELEASE_TOKEN → W_DONE).
 */

#include "box/file.h"
#include "box/debug.h"
#include "box/string.h"

#define S1_BYTES   16384       /* 4 disk blocks, multi-page user buffer */
#define S2_LOOPS   200
#define S2_BYTES   256
#define S3_LOOPS   10
#define S3_BYTES   64

static char g_buf[S1_BYTES];
static char g_rbuf[S1_BYTES];

static int s1_multi_block(void)
{
    for (uint32_t i = 0; i < S1_BYTES; i++) g_buf[i] = (char)(i & 0xFF);

    int fid = create("_wstress_mb", "wstress");
    if (fid < 0) {
        kdbg_print("[WS S1] create rc=%d", fid);
        return 1;
    }

    int wrc = fwrite((uint32_t)fid, 0, g_buf, S1_BYTES);
    if (wrc != S1_BYTES) {
        kdbg_print("[WS S1] write rc=%d expected=%d", wrc, S1_BYTES);
        delete((uint32_t)fid);
        return 1;
    }

    memset(g_rbuf, 0, S1_BYTES);
    int rrc = fread((uint32_t)fid, 0, g_rbuf, S1_BYTES);
    if (rrc != S1_BYTES) {
        kdbg_print("[WS S1] read rc=%d expected=%d", rrc, S1_BYTES);
        delete((uint32_t)fid);
        return 1;
    }

    for (uint32_t i = 0; i < S1_BYTES; i++) {
        if (g_rbuf[i] != (char)(i & 0xFF)) {
            kdbg_print("[WS S1] mismatch at %u (got=0x%x)", i, (unsigned)(uint8_t)g_rbuf[i]);
            delete((uint32_t)fid);
            return 1;
        }
    }

    delete((uint32_t)fid);
    kdbg_print("[WS S1] PASS — 16 KB multi-block write+verify");
    return 0;
}

static int s2_overwrite_churn(void)
{
    int fid = create("_wstress_ov", "wstress");
    if (fid < 0) {
        kdbg_print("[WS S2] create rc=%d", fid);
        return 1;
    }

    char buf[S2_BYTES];
    for (int n = 0; n < S2_LOOPS; n++) {
        for (uint32_t i = 0; i < S2_BYTES; i++) {
            buf[i] = (char)((n * 17 + i) & 0xFF);
        }
        int wrc = fwrite((uint32_t)fid, 0, buf, S2_BYTES);
        if (wrc != S2_BYTES) {
            kdbg_print("[WS S2] write rc=%d at iter=%d", wrc, n);
            delete((uint32_t)fid);
            return 1;
        }
    }

    char rbuf[S2_BYTES];
    int rrc = fread((uint32_t)fid, 0, rbuf, S2_BYTES);
    if (rrc != S2_BYTES) {
        kdbg_print("[WS S2] final read rc=%d", rrc);
        delete((uint32_t)fid);
        return 1;
    }

    int last = S2_LOOPS - 1;
    for (uint32_t i = 0; i < S2_BYTES; i++) {
        char want = (char)((last * 17 + i) & 0xFF);
        if (rbuf[i] != want) {
            kdbg_print("[WS S2] final mismatch at %u (got=0x%x want=0x%x)",
                       i, (unsigned)(uint8_t)rbuf[i], (unsigned)(uint8_t)want);
            delete((uint32_t)fid);
            return 1;
        }
    }

    delete((uint32_t)fid);
    kdbg_print("[WS S2] PASS — %d overwrites + final-state verify", S2_LOOPS);
    return 0;
}

static int s3_append(void)
{
    int fid = create("_wstress_ap", "wstress");
    if (fid < 0) {
        kdbg_print("[WS S3] create rc=%d", fid);
        return 1;
    }

    char buf[S3_BYTES];
    for (int n = 0; n < S3_LOOPS; n++) {
        for (uint32_t i = 0; i < S3_BYTES; i++) {
            buf[i] = (char)('A' + ((n + i) % 26));
        }

        file_info_t info;
        if (file_info((uint32_t)fid, &info) != 0) {
            kdbg_print("[WS S3] file_info FAIL at iter=%d", n);
            delete((uint32_t)fid);
            return 1;
        }

        int wrc = fwrite((uint32_t)fid, info.size, buf, S3_BYTES);
        if (wrc != S3_BYTES) {
            kdbg_print("[WS S3] write rc=%d at iter=%d", wrc, n);
            delete((uint32_t)fid);
            return 1;
        }
    }

    file_info_t info;
    if (file_info((uint32_t)fid, &info) != 0) {
        delete((uint32_t)fid);
        return 1;
    }
    uint64_t expected = (uint64_t)S3_LOOPS * (uint64_t)S3_BYTES;
    if (info.size != expected) {
        kdbg_print("[WS S3] size %lu != expected %lu",
                   (unsigned long)info.size, (unsigned long)expected);
        delete((uint32_t)fid);
        return 1;
    }

    /* Verify the last-appended chunk is intact at the end of file. */
    char rbuf[S3_BYTES];
    uint64_t tail_off = expected - S3_BYTES;
    int rrc = fread((uint32_t)fid, tail_off, rbuf, S3_BYTES);
    if (rrc != S3_BYTES) {
        kdbg_print("[WS S3] tail read rc=%d", rrc);
        delete((uint32_t)fid);
        return 1;
    }
    int last = S3_LOOPS - 1;
    for (uint32_t i = 0; i < S3_BYTES; i++) {
        char want = (char)('A' + ((last + i) % 26));
        if (rbuf[i] != want) {
            kdbg_print("[WS S3] tail mismatch at %u (got=0x%x want=0x%x)",
                       i, (unsigned)(uint8_t)rbuf[i], (unsigned)(uint8_t)want);
            delete((uint32_t)fid);
            return 1;
        }
    }

    delete((uint32_t)fid);
    kdbg_print("[WS S3] PASS — %d appends, final size %lu",
               S3_LOOPS, (unsigned long)expected);
    return 0;
}

int main(void)
{
    kdbg_print("[WS] Write Stress starting");

    int fails = 0;
    if (s1_multi_block()      != 0) fails++;
    if (s2_overwrite_churn()  != 0) fails++;
    if (s3_append()           != 0) fails++;

    if (fails == 0) {
        kdbg_print("[WS SUMMARY] all 3 PASS");
        return 0;
    }
    kdbg_print("[WS SUMMARY] %d FAIL", fails);
    return 1;
}
