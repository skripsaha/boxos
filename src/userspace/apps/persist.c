/*
 * persist — two-phase TagFS persistence verification.
 *
 * Phase A (no args / "write"):
 *   creates `_persist_test.txt` tagged "persist", writes a known marker
 *   (16 bytes: magic + sequence + checksum), exits. User then runs
 *   `bye` (or `reboot`) and re-launches BoxOS. The disk image is NOT
 *   formatted on boot — TagFS replays from the existing image.
 *
 * Phase B ("verify"):
 *   re-opens the file by name, reads back the 16 bytes, checks magic
 *   and checksum. PASS means TagFS journaling + dirty-page flush
 *   actually committed the writes to the underlying disk before halt.
 *
 * Why this matters: "OS that loses your file on shutdown" is not a
 * production OS. This is the foundation test before async storage —
 * we need to know the existing sync path is correctness-clean.
 */

#include "box/file.h"
#include "box/debug.h"
#include "box/string.h"

#define PERSIST_NAME    "_persist_test"
#define PERSIST_MAGIC   0x42504552u   /* 'BPER' = Box PERsist */
#define PERSIST_BYTES   16

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t sequence;
    uint32_t checksum;
    uint32_t _reserved;
} PersistRecord;

static uint32_t fold_checksum(uint32_t magic, uint32_t seq)
{
    /* Trivial mix — strong enough for "did the bytes survive a reboot
     * intact?" without dragging in a hash library. */
    uint32_t x = magic ^ (seq * 2654435761u);
    x ^= x >> 16;
    return x * 0x85ebca6b;
}

static int do_write(uint32_t sequence)
{
    int fid = create(PERSIST_NAME, "persist");
    if (fid < 0) {
        kdbg_print("[PERSIST] create failed rc=%d", fid);
        return 1;
    }

    PersistRecord rec;
    rec.magic     = PERSIST_MAGIC;
    rec.sequence  = sequence;
    rec.checksum  = fold_checksum(PERSIST_MAGIC, sequence);
    rec._reserved = 0;

    int wrc = fwrite((uint32_t)fid, 0, &rec, sizeof(rec));
    if (wrc < 0) {
        kdbg_print("[PERSIST] fwrite failed rc=%d", wrc);
        return 1;
    }

    kdbg_print("[PERSIST] WROTE fid=%d seq=%u magic=0x%x checksum=0x%x",
               fid, rec.sequence, rec.magic, rec.checksum);
    kdbg_print("[PERSIST] now run: bye, then start BoxOS again, then: persist verify");
    return 0;
}

static int do_verify(void)
{
    uint32_t   fids[8];
    file_info_t infos[8];
    int found = find_file_by_name(PERSIST_NAME, fids, infos, 8);
    if (found <= 0) {
        kdbg_print("[PERSIST] FAIL: file '%s' not found after reboot (rc=%d)",
                   PERSIST_NAME, found);
        return 1;
    }

    file_info_t info0 = infos[0];
    kdbg_print("[PERSIST] found fid=%u found_count=%d info_size=%lu",
               fids[0], found, (unsigned long)info0.size);

    PersistRecord rec;
    memset(&rec, 0, sizeof(rec));
    int rrc = fread(fids[0], 0, &rec, sizeof(rec));
    if (rrc < 0) {
        kdbg_print("[PERSIST] FAIL: fread fid=%u rc=%d", fids[0], rrc);
        return 1;
    }

    uint32_t want_cs = fold_checksum(rec.magic, rec.sequence);
    bool magic_ok = (rec.magic == PERSIST_MAGIC);
    bool cs_ok    = (rec.checksum == want_cs);

    kdbg_print("[PERSIST] READ  fid=%u rrc=%d seq=%u magic=0x%x checksum=0x%x (want 0x%x)",
               fids[0], rrc, rec.sequence, rec.magic, rec.checksum, want_cs);

    if (magic_ok && cs_ok) {
        kdbg_print("[PERSIST] PASS: file survived reboot, payload intact");
        return 0;
    }
    kdbg_print("[PERSIST] FAIL: %s%s",
               magic_ok ? "" : "magic_mismatch ",
               cs_ok    ? "" : "checksum_mismatch");
    return 1;
}

int main(void)
{
    /* No real arg parsing — the shell launches us via `proc_exec` and
     * doesn't pass argv yet. Mode is decided by whether the marker
     * already exists: if not, write; if yes, verify. Idempotent enough
     * for a manual reboot loop. */
    uint32_t   fids[4];
    file_info_t infos[4];
    int found = find_file_by_name(PERSIST_NAME, fids, infos, 4);

    if (found <= 0) {
        /* First run after a fresh boot or after delete — write phase. */
        return do_write(/*sequence=*/1);
    }
    /* File exists — assume reboot has happened, verify phase. */
    return do_verify();
}
