/*
 * cow_test — snapshots on a format-v1 volume, as a program meets them.
 *
 *   1. create a file, write pattern A
 *   2. snap_create of the file, and of the whole volume — each answered
 *      -ERR_UNSUPPORTED, with no id handed back
 *   3. snap_list, snap_info, snap_delete — each answered -ERR_UNSUPPORTED
 *      too, never an empty list or a "not found"
 *   4. write pattern B over A, read it back — the write lands in place
 *
 * Pass means: every snapshot request is refused out loud, and a write to the
 * file it would have been taken of reads back as written. Snapshots arrive
 * with format v2.
 */

#include "box/file.h"
#include "box/error.h"
#include "box/debug.h"
#include "box/string.h"

#define COW_NAME   "_cow_test"
#define COW_TAG    "cowtest"
#define COW_BYTES  256

static char buf_a[COW_BYTES];
static char buf_b[COW_BYTES];
static char rbuf[COW_BYTES];

/* Whether a snapshot request came back as the refusal format v1 gives. */
static bool refused(const char *what, int rc)
{
    if (rc == -ERR_UNSUPPORTED)
        return true;
    kdbg_print("[COW] FAIL: %s answered %d, not -ERR_UNSUPPORTED (%d)", what, rc,
               -ERR_UNSUPPORTED);
    return false;
}

static bool snapshots_refused(uint32_t fid)
{
    uint32_t    snap_id = 0;
    uint32_t    ids[4];
    uint32_t    count   = 0;
    snap_info_t info;

    if (!refused("snap_create of the file", snap_create("cow_pre_b", fid, &snap_id)))
        return false;
    if (!refused("snap_create of the volume", snap_create("cow_all", 0, &snap_id)))
        return false;
    if (snap_id != 0) {
        kdbg_print("[COW] FAIL: a refused snap_create handed back id %u", snap_id);
        return false;
    }
    if (!refused("snap_list", snap_list(ids, 4, &count)))
        return false;
    if (count != 0) {
        kdbg_print("[COW] FAIL: a refused snap_list counted %u", count);
        return false;
    }
    return refused("snap_info", snap_info(1, &info)) &&
           refused("snap_delete", snap_delete(1));
}

int main(void)
{
    for (uint32_t i = 0; i < COW_BYTES; i++) {
        buf_a[i] = (char)('A' + (i % 26));
        buf_b[i] = (char)('Z' - (i % 26));
    }

    int fid = create(COW_NAME, COW_TAG);
    if (fid < 0) {
        kdbg_print("[COW] create rc=%d", fid);
        return 1;
    }

    if (fwrite((uint32_t)fid, 0, buf_a, COW_BYTES) != COW_BYTES) {
        kdbg_print("[COW] initial write FAIL");
        delete((uint32_t)fid);
        return 1;
    }

    if (!snapshots_refused((uint32_t)fid)) {
        delete((uint32_t)fid);
        return 1;
    }
    kdbg_print("[COW] every snapshot request is refused (ERR_UNSUPPORTED) on format v1");

    if (fwrite((uint32_t)fid, 0, buf_b, COW_BYTES) != COW_BYTES) {
        kdbg_print("[COW] overwrite FAIL");
        delete((uint32_t)fid);
        return 1;
    }
    if (fread((uint32_t)fid, 0, rbuf, COW_BYTES) != COW_BYTES) {
        kdbg_print("[COW] read after overwrite FAIL");
        delete((uint32_t)fid);
        return 1;
    }
    if (memcmp(rbuf, buf_b, COW_BYTES) != 0) {
        kdbg_print("[COW] FAIL: live read != pattern B");
        delete((uint32_t)fid);
        return 1;
    }

    delete((uint32_t)fid);
    kdbg_print("[COW] PASS — snapshots refused on format v1, the write lands in place");
    return 0;
}
