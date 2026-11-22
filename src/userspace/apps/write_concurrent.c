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
#include "box/system.h"
#include "box/luggage.h"
#include "box/core/cabin.h"
#include "box/debug.h"
#include "box/string.h"

#define WC_NAME       "_wconc_test"
#define WC_TAG        "wconc"
#define WC_CLAIM_TAG  "wconc:claim"
#define PAGE_SIZE     4096
#define CHILD_COUNT   2
#define CHILD_LOOPS   50

/* A child's role is its LUGGAGE — the line it was started with, in its cabin
 * before its first instruction.
 *
 * This used to be "my spawner's pid is not 2, so I must be a child": the
 * shell's seat number written as a literal, standing in for the question
 * actually being asked. The shell is a program like any other and its pid is
 * whatever the boot happened to hand it, so any boot that seats it elsewhere
 * turns the parent run into a child run — a child that creates the file, the
 * claim Bay and children of its OWN while the real parent waits on it. Word 1
 * is there or this program is the parent, and there is nothing in between; the
 * matrix starts `write_concurrent` with no second word, and only this file
 * adds one. */
#define LINE_CHILD       "write_concurrent slice"
#define ROLE_WORD_SLICE  "slice"

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

    file_id_t  fids[4];
    file_info_t infos[4];
    int found = find_file_by_name(MY_TAGS, WC_NAME, NULL, fids, infos, 4);
    if (found <= 0) {
        kdbg_print("[WC child %u] file not found", my_pid);
        return 1;
    }
    file_id_t fid = fids[0];

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
    uint32_t my_pid = cabin_info()->pid;

    /* Word 1 of the luggage names the role, and it is already there. A word we
     * do not know is a caller's mistake — say so and end, rather than run the
     * parent half a second time inside a child. */
    const char *role = luggage_word(1);
    if (role) {
        if (strcmp(role, ROLE_WORD_SLICE) == 0) return child_role(my_pid);
        kdbg_print("[WC C-?] unknown role '%s'", role);
        return 255;
    }

    /* Parent. Pre-create the file with N×PAGE_SIZE bytes by writing once
     * to each slice — guarantees extents exist before children race. */
    int64_t fid = create(MY_TAGS, WC_NAME, WC_TAG);
    if (fid < 0) {
        kdbg_print("[WC] create rc=%ld", fid);
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

    /* Each child is spawned as an INCARNATION — (pid, generation) — because a
     * pid alone names a seat and seats are re-let. */
    int      kids[CHILD_COUNT];
    uint32_t gens[CHILD_COUNT];
    for (int i = 0; i < CHILD_COUNT; i++) {
        kids[i] = proc_exec_gen(LINE_CHILD, NULL, &gens[i]);
        if (kids[i] <= 0) {
            kdbg_print("[WC] spawn child %d rc=%d", i, kids[i]);
            bay_release(claim);
            delete(fid);
            return 1;
        }
    }

    /* Wait for each child as a STATE, one after the other.
     *
     * This was a claim on the process:died multicast drained by 200 turns of a
     * 200 ms await — a forty-second guess that printed "children still alive
     * after timeout" on a host that was merely loaded, and that could also
     * simply MISS a death: a multicast is best-effort, and a claim armed after
     * a death never sees it at all. process_gone asks the child's own record
     * instead: a child that has already finished answers at once, a live one is
     * parked on, and there is no window between asking and being registered.
     * A death that never comes is now a wedge Nightwatch convicts on facts,
     * not a red run on a busy machine.
     *
     * The claim Bay and the file stay held across every one of these waits —
     * a child still writing must not find either re-keyed under it. */
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
