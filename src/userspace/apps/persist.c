/*
 * persist — two-phase TagFS persistence verification, one phase per word.
 *
 *   persist write
 *     deletes every earlier `_persist_test` and anchors the deletes, then
 *     creates `_persist_test` tagged "persist", writes a known marker
 *     (16 bytes: magic + sequence + checksum) and exits WITHOUT anchoring
 *     it. User then runs `bye` (or `reboot`) and re-launches BoxOS; or
 *     tools/powercut.sh pulls the power instead (its `noanchor` scenario).
 *
 *   persist verify
 *     finds the file by name, reads back the 16 bytes, checks magic and
 *     checksum, and never writes. PASS means the write reached the medium
 *     with its name before the machine went down.
 *
 * The phase is said, not guessed. It used to be chosen by whether the file
 * already existed, so a file lost to a power cut turned "verify" into a new
 * write that printed WROTE — never FAIL — and the loss looked like a pass.
 * And every write carries the same seq-1 record, so an earlier file left
 * beside a lost write would read back as that pass: the earlier ones are
 * deleted durably before the write, and verify refuses to answer for a
 * volume holding more than one.
 *
 * Why this matters: "OS that loses your file on shutdown" is not a
 * production OS.
 */

#include "box/file.h"
#include "box/debug.h"
#include "box/luggage.h"
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
    /* The deletes are anchored before the write under test, so a cut that
     * loses the write cannot bring an earlier file back in its place. On
     * PowerCut's settled base there is none: nothing is deleted, nothing
     * anchored, and the record holds the write under test alone. */
    file_id_t earlier[8];
    int      gone = 0;
    for (;;) {
        int found = find_file_by_name(PERSIST_NAME, "trashed,hidden", earlier, NULL, 8);
        if (found < 0) {
            kdbg_print("[PERSIST] FAIL: could not ask the volume for an earlier '%s' (error %d)",
                       PERSIST_NAME, found);
            return 1;
        }
        if (found == 0)
            break;
        for (int i = 0; i < found && i < 8; i++, gone++) {
            int drc = delete(earlier[i]);
            if (drc != 0) {
                kdbg_print("[PERSIST] FAIL: the earlier '%s' fid=%lu would not be deleted (rc=%d)",
                           PERSIST_NAME, earlier[i], drc);
                return 1;
            }
        }
    }
    if (gone > 0) {
        int arc = anchor(0);
        if (arc != 0) {
            kdbg_print("[PERSIST] FAIL: %d earlier '%s' deleted, and the anchor that makes it "
                       "durable answered %d", gone, PERSIST_NAME, arc);
            return 1;
        }
        kdbg_print("[PERSIST] %d earlier '%s' deleted and anchored before the write",
                   gone, PERSIST_NAME);
    }

    int64_t fid = create(PERSIST_NAME, "persist");
    if (fid < 0) {
        kdbg_print("[PERSIST] create failed rc=%ld", fid);
        return 1;
    }

    PersistRecord rec;
    rec.magic     = PERSIST_MAGIC;
    rec.sequence  = sequence;
    rec.checksum  = fold_checksum(PERSIST_MAGIC, sequence);
    rec._reserved = 0;

    int wrc = fwrite((file_id_t)fid, 0, &rec, sizeof(rec));
    if (wrc < 0) {
        kdbg_print("[PERSIST] fwrite failed rc=%d", wrc);
        return 1;
    }

    kdbg_print("[PERSIST] WROTE fid=%ld seq=%u magic=0x%x checksum=0x%x",
               fid, rec.sequence, rec.magic, rec.checksum);
    kdbg_print("[PERSIST] now run: bye, then start BoxOS again, then: persist verify");
    return 0;
}

static int do_verify(void)
{
    file_id_t  fids[8];
    file_info_t infos[8];
    int found = find_file_by_name(PERSIST_NAME, "trashed,hidden", fids, infos, 8);
    if (found < 0) {
        kdbg_print("[PERSIST] FAIL: could not ask the volume for '%s' (error %d)",
                   PERSIST_NAME, found);
        return 1;
    }
    if (found == 0) {
        kdbg_print("[PERSIST] FAIL: file '%s' not found after reboot (rc=0)", PERSIST_NAME);
        return 1;
    }
    if (found > 1) {
        kdbg_print("[PERSIST] FAIL: the volume holds %d files called '%s', so a record read "
                   "from one of them says nothing of which write it is", found, PERSIST_NAME);
        return 1;
    }

    file_info_t info0 = infos[0];
    kdbg_print("[PERSIST] found fid=%lu found_count=%d info_size=%lu",
               fids[0], found, (unsigned long)info0.size);

    PersistRecord rec;
    memset(&rec, 0, sizeof(rec));
    int rrc = fread(fids[0], 0, &rec, sizeof(rec));
    if (rrc < 0) {
        kdbg_print("[PERSIST] FAIL: fread fid=%lu rc=%d", fids[0], rrc);
        return 1;
    }

    uint32_t want_cs = fold_checksum(rec.magic, rec.sequence);
    bool magic_ok = (rec.magic == PERSIST_MAGIC);
    bool cs_ok    = (rec.checksum == want_cs);

    kdbg_print("[PERSIST] READ  fid=%lu rrc=%d seq=%u magic=0x%x checksum=0x%x (want 0x%x)",
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
    const char *mode = luggage_word_count() > 1 ? luggage_word(1) : "";

    if (strcmp(mode, "write") == 0)
        return do_write(/*sequence=*/1);
    if (strcmp(mode, "verify") == 0)
        return do_verify();
    kdbg_print("[PERSIST] say what to do: persist write | persist verify (got '%s')", mode);
    return 2;
}
