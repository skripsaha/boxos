/*
 * write_concurrent — drives the OFE token handoff under contention.
 *
 * Parent creates a fresh file, spawns N children. Each child writes
 * its assigned slice (one PAGE_SIZE block) repeatedly. The kernel's
 * async OFE token serializes them — the file content must always be
 * each child's deterministic pattern at its slice offset, never
 * interleaved bytes from different writers.
 *
 * After all children exit, parent re-reads the file and verifies
 * every slice. If a write was interleaved or torn, bytes will not
 * match a single writer's pattern.
 */

#include "box/bay.h"
#include "box/file.h"
#include "box/touch.h"
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
    /* Claim a slice ATOMICALLY instead of deriving it from the pid.
     *
     * This was `my_pid % CHILD_COUNT`, under a comment claiming pid parity
     * kept the assignment deterministic "even when the kernel allocates pids
     * in different orders". It does the opposite: CONSECUTIVE pids differ in
     * parity, non-consecutive ones need not. Let any other process take a pid
     * between the two spawns — routine as soon as more than one core is
     * running — and both children land on the same slice while the other slice
     * is never written and keeps the parent's pre-fill. The parent then read
     * zeros, reconstructed writer pid 0 from byte 0, and reported the kernel's
     * write path as torn. The requirement is "one writer per slice"; a claim
     * counter states it, pid arithmetic guesses it. */
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

    /* Pattern: byte j = (my_pid + j) & 0xFF — unique per writer. */
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

    /* Child branch — spawned by another instance. Shell PID is 2; if
     * spawner is anything else (and not zero) we're a child. */
    if (ci && ci->spawner_pid != 0 && ci->spawner_pid != 2) {
        return child_role(my_pid);
    }

    /* Parent. Pre-create the file with N×PAGE_SIZE bytes by writing once
     * to each slice — guarantees extents exist before children race. */
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

    /* Slice-claim Bay. Created before the first spawn so no child can find
     * it missing, and held by the parent until every child has exited so the
     * last release cannot re-key it mid-run. */
    uint32_t *claim = (uint32_t *)bay_open(WC_CLAIM_TAG, PAGE_SIZE, BAY_CREATE);
    if (!claim) {
        kdbg_print("[WC] claim bay create FAIL");
        delete(fid);
        return 1;
    }
    *claim = 0;

    /* Subscribe to process:died so we know when children finish. */
    TouchTag died_tag = TOUCH_TAG_ID(TOUCH_TAG_PROCESS_DIED);
    if (touch_claim(died_tag, TOUCH_REST, 0, 0) != 0) {
        kdbg_print("[WC] claim process:died FAIL");
        bay_release(claim);
        delete(fid);
        return 1;
    }

    int kids[CHILD_COUNT];
    int kids_alive = 0;
    for (int i = 0; i < CHILD_COUNT; i++) {
        kids[i] = proc_exec("write_concurrent");
        if (kids[i] < 0) {
            kdbg_print("[WC] spawn child %d rc=%d", i, kids[i]);
            touch_release(died_tag);
            bay_release(claim);
            delete(fid);
            return 1;
        }
        kids_alive++;
    }

    /* Wait for all children to die. Bounded poll with deadline. */
    int seen[CHILD_COUNT] = {0};
    for (int spin = 0; spin < 200 && kids_alive > 0; spin++) {
        Touch t;
        int rc = touch_await(died_tag, &t, 200);
        if (rc != 0) continue;
        const uint8_t *p = t.payload;
        if (t.payload_len < 8) continue;
        uint32_t dead_pid;
        memcpy(&dead_pid, p, 4);
        for (int i = 0; i < CHILD_COUNT; i++) {
            if (kids[i] == (int)dead_pid && !seen[i]) {
                seen[i] = 1;
                kids_alive--;
                kdbg_print("[WC] child %d (pid=%u) exited", i, dead_pid);
                break;
            }
        }
    }

    touch_release(died_tag);

    if (kids_alive > 0) {
        kdbg_print("[WC] FAIL: %d children still alive after timeout", kids_alive);
        bay_release(claim);
        delete(fid);
        return 1;
    }

    uint32_t claimed = *claim;
    bay_release(claim);
    if (claimed != CHILD_COUNT) {
        kdbg_print("[WC] FAIL: %u of %d slices claimed", claimed, CHILD_COUNT);
        delete(fid);
        return 1;
    }

    /* Verify: each slice must match exactly ONE writer's pattern. The
     * winning writer is whoever did the last write to that slice — they
     * all wrote the same content for their assigned slice every time,
     * so the slice = pattern_of(writer_of_that_slice). */
    int fails = 0;
    for (int slice = 0; slice < CHILD_COUNT; slice++) {
        if (fread(fid, (uint64_t)slice * PAGE_SIZE, rbuf, PAGE_SIZE) != PAGE_SIZE) {
            kdbg_print("[WC] verify read slice=%d FAIL", slice);
            fails++;
            continue;
        }
        /* An all-zero slice is the parent's pre-fill, not a torn write: no
         * writer can produce it, since a writer's bytes step by one from its
         * own pid. Calling that "torn at j=1" is what sent the last hunt after
         * the kernel's write path instead of after the missing writer. */
        bool written = false;
        for (uint32_t j = 0; j < PAGE_SIZE; j++) {
            if (rbuf[j] != 0) { written = true; break; }
        }
        if (!written) {
            kdbg_print("[WC] slice=%d NEVER WRITTEN (still pre-fill zeros)", slice);
            fails++;
            continue;
        }

        /* Reconstruct writer PID from byte 0: byte0 = (pid + 0) & 0xFF. */
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
