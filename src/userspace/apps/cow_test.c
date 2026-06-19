/*
 * cow_test — CoW + snapshot semantic test.
 *
 *   1. create file, write pattern A
 *   2. snap_create — capture frozen view
 *   3. write pattern B (triggers CoW redirect to NEW block)
 *   4. read live file → must be pattern B
 *   5. snap_list  — confirm snapshot present
 *   6. snap_delete → frees redirected OLD blocks
 *   7. snap_list  — confirm gone
 *   8. live read still pattern B
 *
 * Pass means: write paths see fresh content, snapshot list reflects
 * lifecycle, delete completes without corrupting the live file.
 */

#include "box/file.h"
#include "box/debug.h"
#include "box/string.h"

#define COW_NAME   "_cow_test"
#define COW_TAG    "cowtest"
#define COW_BYTES  256

static char buf_a[COW_BYTES];
static char buf_b[COW_BYTES];
static char rbuf[COW_BYTES];

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

    /* Snap before second write — second write must CoW. */
    uint32_t snap_id = 0;
    if (snap_create("cow_pre_b", (uint32_t)fid, &snap_id) != 0 || snap_id == 0) {
        kdbg_print("[COW] snap_create FAIL");
        delete((uint32_t)fid);
        return 1;
    }
    kdbg_print("[COW] created snapshot id=%u", snap_id);

    if (fwrite((uint32_t)fid, 0, buf_b, COW_BYTES) != COW_BYTES) {
        kdbg_print("[COW] CoW write FAIL");
        snap_delete(snap_id);
        delete((uint32_t)fid);
        return 1;
    }

    if (fread((uint32_t)fid, 0, rbuf, COW_BYTES) != COW_BYTES) {
        kdbg_print("[COW] read after CoW FAIL");
        snap_delete(snap_id);
        delete((uint32_t)fid);
        return 1;
    }
    if (memcmp(rbuf, buf_b, COW_BYTES) != 0) {
        kdbg_print("[COW] FAIL: live read != pattern B");
        snap_delete(snap_id);
        delete((uint32_t)fid);
        return 1;
    }
    kdbg_print("[COW] live read returns post-CoW pattern B (OK)");

    /* Buffer sized well above the number of snapshots earlier boot phases
     * (e.g. the box::tagfs::snapshot CXX tests) may leave in the global
     * list — an 8-entry window could exclude our own snapshot and spuriously
     * fail "snap not in list" depending on boot/test ordering (UEFI). */
    uint32_t ids[64];
    uint32_t count = 0;
    if (snap_list(ids, 64, &count) != 0) {
        kdbg_print("[COW] snap_list FAIL");
        snap_delete(snap_id);
        delete((uint32_t)fid);
        return 1;
    }
    bool found = false;
    for (uint32_t i = 0; i < count; i++) if (ids[i] == snap_id) { found = true; break; }
    if (!found) {
        kdbg_print("[COW] FAIL: snap not in list (count=%u)", count);
        snap_delete(snap_id);
        delete((uint32_t)fid);
        return 1;
    }

    if (snap_delete(snap_id) != 0) {
        kdbg_print("[COW] snap_delete FAIL");
        delete((uint32_t)fid);
        return 1;
    }

    /* Live file should still be intact after snapshot deletion. */
    if (fread((uint32_t)fid, 0, rbuf, COW_BYTES) != COW_BYTES) {
        kdbg_print("[COW] read after snap_delete FAIL");
        delete((uint32_t)fid);
        return 1;
    }
    if (memcmp(rbuf, buf_b, COW_BYTES) != 0) {
        kdbg_print("[COW] FAIL: live read != pattern B after snap_delete");
        delete((uint32_t)fid);
        return 1;
    }

    /* List again — snapshot should be gone. */
    count = 0;
    snap_list(ids, 64, &count);
    bool still_there = false;
    for (uint32_t i = 0; i < count; i++) if (ids[i] == snap_id) { still_there = true; break; }
    if (still_there) {
        kdbg_print("[COW] FAIL: snap still in list after delete");
        delete((uint32_t)fid);
        return 1;
    }

    delete((uint32_t)fid);
    kdbg_print("[COW] PASS — create/snap/CoW-write/list/delete all consistent");
    return 0;
}
