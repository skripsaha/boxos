
#include "box/bay.h"
#include "box/file.h"
#include "box/system.h"
#include "box/core/cabin.h"
#include "box/core/result.h"
#include "box/debug.h"
#include "box/string.h"

#define WC_NAME       "_wconc_test"
#define WC_TAG        "wconc"
#define WC_CLAIM_TAG  "wconc:claim"
#define PAGE_SIZE     4096
#define CHILD_COUNT   2
#define CHILD_LOOPS   50

static char wbuf[PAGE_SIZE];
static char rbuf[PAGE_SIZE];

static int child_role(uint32_t my_pid)
{
    uint32_t *claim = (uint32_t *)bay_open(WC_CLAIM_TAG, 0, BAY_OPEN);
    if (!claim) {
        kdbg_print("[WC child %u] claim bay missing", my_pid);
        return 1;
    }
    uint32_t slice_idx = __sync_fetch_and_add(claim, 1u);
    bay_release(claim);
    if (slice_idx >= CHILD_COUNT) {
        kdbg_print("[WC child %u] claim idx=%u out of range", my_pid, slice_idx);
        return 1;
    }
    uint64_t offset = (uint64_t)slice_idx * PAGE_SIZE;

    uint32_t   fids[4];
    file_info_t infos[4];
    int found = find_file_by_name(WC_NAME, fids, infos, 4);
    if (found <= 0) {
        kdbg_print("[WC child %u] file not found", my_pid);
        return 1;
    }
    uint32_t fid = fids[0];

    for (uint32_t i = 0; i < PAGE_SIZE; i++) {
        wbuf[i] = (char)((my_pid + i) & 0xFF);
    }

    for (int n = 0; n < CHILD_LOOPS; n++) {
        int wrc = fwrite(fid, offset, wbuf, PAGE_SIZE);
        if (wrc != PAGE_SIZE) {
            kdbg_print("[WC child %u] write rc=%d at iter=%d", my_pid, wrc, n);
            return 1;
        }
    }

    kdbg_print("[WC child %u] %d iterations done at slice %u (offset %lu)",
               my_pid, CHILD_LOOPS, slice_idx, (unsigned long)offset);
    return 0;
}

int main(void)
{
    CabinInfo *ci = cabin_info();
    uint32_t my_pid = ci ? ci->pid : 0;

    if (ci && ci->spawner_pid != 0 && ci->spawner_pid != 2) {
        return child_role(my_pid);
    }

    int fid = create(WC_NAME, WC_TAG);
    if (fid < 0) {
        kdbg_print("[WC] create rc=%d", fid);
        return 1;
    }

    char zeros[PAGE_SIZE];
    memset(zeros, 0, PAGE_SIZE);
    for (int i = 0; i < CHILD_COUNT; i++) {
        if (fwrite(fid, (uint64_t)i * PAGE_SIZE, zeros, PAGE_SIZE) != PAGE_SIZE) {
            kdbg_print("[WC] pre-fill slice %d FAIL", i);
            delete(fid);
            return 1;
        }
    }

    uint32_t *claim = (uint32_t *)bay_open(WC_CLAIM_TAG, PAGE_SIZE, BAY_CREATE);
    if (!claim) {
        kdbg_print("[WC] claim bay create FAIL");
        delete(fid);
        return 1;
    }
    *claim = 0;

    int      kids[CHILD_COUNT];
    uint32_t gens[CHILD_COUNT];
    for (int i = 0; i < CHILD_COUNT; i++) {
        kids[i] = proc_exec_gen("write_concurrent", NULL, &gens[i]);
        if (kids[i] <= 0) {
            kdbg_print("[WC] spawn child %d rc=%d", i, kids[i]);
            bay_release(claim);
            delete(fid);
            return 1;
        }
    }

    for (int i = 0; i < CHILD_COUNT; i++) {
        int32_t child_exit = 0;
        int     gone       = process_gone((uint32_t)kids[i], gens[i], &child_exit);
        if (gone != 0)
            kdbg_print("[WC] child %d (pid=%d gen=%u): process.gone refused rc=%d",
                       i, kids[i], gens[i], gone);
        else
            kdbg_print("[WC] child %d (pid=%d) ended with %d", i, kids[i], (int)child_exit);
    }

    uint32_t claimed = *claim;
    bay_release(claim);
    if (claimed != CHILD_COUNT) {
        kdbg_print("[WC] FAIL: %u of %d slices claimed", claimed, CHILD_COUNT);
        delete(fid);
        return 1;
    }

    int fails = 0;
    for (int slice = 0; slice < CHILD_COUNT; slice++) {
        if (fread(fid, (uint64_t)slice * PAGE_SIZE, rbuf, PAGE_SIZE) != PAGE_SIZE) {
            kdbg_print("[WC] verify read slice=%d FAIL", slice);
            fails++;
            continue;
        }
        bool written = false;
        for (uint32_t j = 0; j < PAGE_SIZE; j++) {
            if (rbuf[j] != 0) { written = true; break; }
        }
        if (!written) {
            kdbg_print("[WC] slice=%d NEVER WRITTEN (still pre-fill zeros)", slice);
            fails++;
            continue;
        }

        uint8_t pid_low = (uint8_t)rbuf[0];
        bool clean = true;
        for (uint32_t j = 1; j < PAGE_SIZE; j++) {
            uint8_t want = (uint8_t)((pid_low + j) & 0xFF);
            if ((uint8_t)rbuf[j] != want) {
                kdbg_print("[WC] slice=%d torn at j=%u (got=0x%x want=0x%x pid_low=0x%x)",
                           slice, j, (unsigned)(uint8_t)rbuf[j], want, pid_low);
                clean = false;
                break;
            }
        }
        if (!clean) fails++;
    }

    delete(fid);

    if (fails == 0) {
        kdbg_print("[WC] PASS — %d children × %d iters, %d slices clean",
                   CHILD_COUNT, CHILD_LOOPS, CHILD_COUNT);
        return 0;
    }
    kdbg_print("[WC] FAIL — %d bad slice(s)", fails);
    return 1;
}