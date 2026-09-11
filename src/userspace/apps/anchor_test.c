
#include "box/file.h"
#include "box/touch.h"
#include "box/debug.h"
#include "box/string.h"

#define AT_NAME    "_anchor_test"
#define AT_TAG     "anchortest"
#define AT_BYTES   128
#define AT_MAGIC   0x414e4348u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t seq;
    uint32_t checksum;
    uint8_t  pad[AT_BYTES - 12];
} AnchorRecord;

static uint32_t mix(uint32_t a, uint32_t b)
{
    uint32_t x = a ^ (b * 2654435761u);
    x ^= x >> 16;
    return x * 0x85ebca6b;
}

int main(void)
{
    uint32_t   fids[4];
    file_info_t infos[4];
    int found = find_file_by_name(AT_NAME, fids, infos, 4);

    if (found <= 0) {
        int fid = create(AT_NAME, AT_TAG);
        if (fid < 0) {
            kdbg_print("[ANCHOR] create rc=%d", fid);
            return 1;
        }
        AnchorRecord rec;
        memset(&rec, 0, sizeof(rec));
        rec.magic    = AT_MAGIC;
        rec.seq      = 1;
        rec.checksum = mix(rec.magic, rec.seq);

        int wrc = fwrite((uint32_t)fid, 0, &rec, sizeof(rec));
        if (wrc != sizeof(rec)) {
            kdbg_print("[ANCHOR] fwrite rc=%d", wrc);
            return 1;
        }

        int arc = anchor((uint32_t)fid);
        if (arc != 0) {
            kdbg_print("[ANCHOR] anchor rc=%d", arc);
            return 1;
        }

        kdbg_print("[ANCHOR] WROTE+ANCHORED fid=%d. Now kill QEMU and re-run.",
                   fid);
        return 0;
    }

    AnchorRecord rec;
    memset(&rec, 0, sizeof(rec));
    int rrc = fread(fids[0], 0, &rec, sizeof(rec));
    if (rrc != sizeof(rec)) {
        kdbg_print("[ANCHOR] read rc=%d", rrc);
        return 1;
    }

    uint32_t want_cs = mix(rec.magic, rec.seq);
    if (rec.magic != AT_MAGIC || rec.checksum != want_cs) {
        kdbg_print("[ANCHOR] FAIL: magic=0x%x seq=%u cs=0x%x want=0x%x",
                   rec.magic, rec.seq, rec.checksum, want_cs);
        delete(fids[0]);
        return 1;
    }

    kdbg_print("[ANCHOR] PASS — anchored write survived hard crash (seq=%u)",
               rec.seq);
    delete(fids[0]);
    return 0;
}