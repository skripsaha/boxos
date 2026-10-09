#include "hold_ground.h"
#include "../helm/helm.h"
#include "atomics.h"
#include "cpu_calibrate.h"

/*
 * HOLDGROUND=on — a caller who is inside the volume when its medium leaves.
 *
 * ‼ A REPRODUCTION, NOT A FEATURE. Compiled in by `make HOLDGROUND=on` and by
 * nothing else.
 *
 * The window the volume's door exists for is microseconds wide and QEMU never
 * lands in it: on a desk the volume comes back while the machine is idle, so
 * there is nobody inside to be protected and every check about the door would
 * be green by construction. This puts somebody there — at the exact moment the
 * medium is noticed gone, one caller steps into the volume and stays for two
 * seconds, which is the situation a take-down has to survive when the open
 * book is given up (mount/away.h). A book taken up takes nothing down, and the
 * caller steps out then.
 *
 * It also holds a file open ACROSS the departure, which is the other half:
 * a handle carries its own copy of the file's extents, and the table of open
 * files used to be emptied out from under it by the next mount.
 *
 * Three facts come out of a book given up, and each is the failure of a
 * different line:
 *
 *   the take-down says it is waiting and then that the last caller is out
 *          — take the "somebody is inside" refusal out of tagfs_abandon;
 *   a read through the old handle is refused by name
 *          — take the mounting stamp off the handle;
 *   the entry that the old handle owns is STILL IN the open-file table
 *          — put the memset back into tagfs_init.
 *
 * And one out of a book taken up: the read through the handle from before
 * answers as it always did — the mounting it was opened on is the one up.
 */
#if CONFIG_TAGFS_HOLD_GROUND

#define TAGFS_HOLD_GROUND_MS 2000u

static bool             g_hold_inside  = false;
static uint64_t         g_hold_until   = 0;
static TagFSFileHandle *g_hold_handle  = NULL;

/* Is the entry this handle owns still the one the table hands out for its
 * file? A second entry for one file is a second Helm for one file, which is
 * two holders of its extents at once — and it leaves no trace of its own. */
static bool hold_entry_still_listed(const TagFSFileHandle *handle, uint32_t *out_refs)
{
    return handle && ofe_listed(handle->ofe, out_refs);
}

/* A file is held open from the moment the volume is first mounted, so that it
 * is a handle from the PREVIOUS mounting when the volume comes back. */
void TagFSHoldGroundOpenFile(void)
{
    if (g_hold_handle) {
        return;
    }
    uint64_t  ids[1];
    TagFSPage page;
    if (TagFSListPage(1, ids, 1, false, &page) != OK || page.delivered == 0) {
        kprintf("[TagFS HOLDGROUND] this volume has no file to hold open\n");
        return;
    }
    g_hold_handle = tagfs_open(ids[0], 0);
    kprintf("[TagFS HOLDGROUND] holding file %llu open across whatever happens "
            "to the medium\n", (unsigned long long)ids[0]);
}

/* And a caller inside the volume at the moment its medium is noticed gone. */
void TagFSHoldGroundTake(void)
{
    if (g_hold_inside) {
        return;
    }
    if (!tagfs_enter()) {
        return;                     /* already shut — nothing to demonstrate */
    }
    g_hold_inside = true;
    g_hold_until  = rdtsc() + cpu_ms_to_tsc(TAGFS_HOLD_GROUND_MS);
    kprintf("[TagFS HOLDGROUND] a caller was inside the volume when its medium "
            "left, and does not step out until the volume is back and at "
            "least %u ms have passed\n", TAGFS_HOLD_GROUND_MS);
}

/* Reached only once a return is waiting to be finished, which is what makes
 * this deterministic: the caller is inside when the volume comes back, every
 * time, rather than whenever a clock happens to land. */
void TagFSHoldGroundRelease(void)
{
    if (!g_hold_inside || (int64_t)(rdtsc() - g_hold_until) < 0) {
        return;
    }
    g_hold_inside = false;
    tagfs_leave();
    kprintf("[TagFS HOLDGROUND] the caller has stepped out of the old volume\n");
}

/* And what the volume looks like to what was held across it. */
void TagFSHoldGroundAfterReturn(void)
{
    if (!g_hold_handle || !tagfs_get_state()->initialized) {
        return;
    }

    uint8_t  probe[16];
    uint64_t got = 0;
    error_t  rc  = tagfs_read(g_hold_handle, probe, sizeof(probe), &got);
    kprintf("[TagFS HOLDGROUND] a read through the handle from before answered "
            "%u\n", (unsigned)rc);

    uint32_t refs = 0;
    bool listed = hold_entry_still_listed(g_hold_handle, &refs);
    kprintf("[TagFS HOLDGROUND] the open-file entry that handle owns is %s the "
            "table, with %u reference(s)\n",
            listed ? "still in" : "NO LONGER in", refs);

    TagFSFileHandle *again = tagfs_open(g_hold_handle->file_id, 0);
    if (again) {
        refs = 0;
        listed = hold_entry_still_listed(g_hold_handle, &refs);
        kprintf("[TagFS HOLDGROUND] opening the same file again %s the same "
                "entry, now with %u reference(s)\n",
                (again->ofe == g_hold_handle->ofe) ? "found" : "made a SECOND",
                refs);
        tagfs_close(again);
    }

    tagfs_close(g_hold_handle);
    g_hold_handle = NULL;
}

/* The book taken up: nothing is taken down, so the caller steps out now, and
 * the handle from before is asked what it reads. */
void TagFSHoldGroundResumed(void)
{
    if (g_hold_inside) {
        g_hold_inside = false;
        tagfs_leave();
        kprintf("[TagFS HOLDGROUND] the volume's book was taken up — nothing is taken down, "
                "and the caller steps out\n");
    }
    TagFSHoldGroundAfterReturn();
}

#endif  /* CONFIG_TAGFS_HOLD_GROUND */
