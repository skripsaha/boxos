#include "autostart.h"
#include "process.h"
#include "tagfs.h"
#include "tag_registry/tag_registry.h"
#include "touch.h"
#include "logbook.h"
#include "pmm.h"
#include "vmm.h"
#include "klib.h"
#include "kernel_config.h"
#include "proc_exit.h"

/*
 * Autostart — the programs the volume asks for, whenever the volume turns up.
 *
 * This used to be a hundred and ninety lines in the middle of kernel_main,
 * executed once, at a moment chosen by where it happened to sit in the boot.
 * A machine whose medium finished enumerating a second later came up with
 * nothing on it and stayed that way: the files were there, readable, tagged
 * autostart, and nobody ever looked again.
 *
 * So it is a function, and it runs when a volume is mounted rather than when
 * the boot reaches a particular line. Twice at most: once from the boot, and
 * once more if the boot found no volume and one arrived afterwards.
 */

/* The stand-in the kernel starts when there is nothing to start from a volume.
 * Held as a pid rather than a pointer: by the time the relief arrives this
 * process may have exited on its own and been reaped, and a pid that has gone
 * away simply fails to resolve, where a pointer would be a dangling read. */
static volatile uint32_t g_stand_in_pid = 0;

/* Whether a volume's programs have ever been launched. One-shot: a volume that
 * leaves and comes back does not start second copies of everything, because
 * the first copies are still there. */
static volatile uint32_t g_volume_launched = 0;

/* One core does the late launch and the rest go away rather than queue up
 * behind it — the same arrangement the Boardroom uses for seating, and for
 * the same reason: this creates processes and reads a disk. */
static volatile uint32_t g_busy = 0;

static TouchWatch *g_mount_watch = NULL;

void AutostartNoteStandIn(uint32_t pid)
{
    __atomic_store_n(&g_stand_in_pid, pid, __ATOMIC_RELEASE);
}

void AutostartNoteVolumeLaunched(void)
{
    __atomic_store_n(&g_volume_launched, 1u, __ATOMIC_RELEASE);
}

static bool autostart_wanted(const TagFSMetadata *meta)
{
    bool has_autostart = false;
    bool has_exec_tag  = false;
    for (uint32_t t = 0; t < meta->tag_count; t++) {
        char key[128];
        if (!tagfs_tag_key(meta->tag_ids[t], key, sizeof(key))) continue;
        if (strcmp(key, "autostart") == 0) has_autostart = true;
        if (strcmp(key, "app") == 0 || strcmp(key, "utility") == 0)
            has_exec_tag = true;
    }
    return has_autostart && has_exec_tag;
}

/* Read one file out of the volume and turn it into a process. Returns the
 * process, or NULL with the reason already printed — every refusal here names
 * the file it refused, because a machine that silently starts three of four
 * programs is worse than one that starts none. */
static process_t *autostart_launch_one(uint64_t file_id, TagFSMetadata *meta,
                                       const char *tags)
{
    uint64_t file_size = meta->size;
    if (file_size == 0 || file_size > CONFIG_PROC_MAX_BINARY_SIZE) {
        kprintf("[AUTOSTART] Skip '%s': invalid size %lu\n",
                meta->filename, file_size);
        return NULL;
    }

    /* The file is read where the loader asks: a box only as far as starting
     * it needs, an image of another kind whole (image_load.h). */
    TagFSFileHandle *fh = tagfs_open(file_id, TAGFS_HANDLE_READ);
    if (!fh) {
        kprintf("[AUTOSTART] Skip '%s': tagfs_open failed\n", meta->filename);
        return NULL;
    }

    process_t *proc = process_create(tags);
    if (!proc) {
        tagfs_close(fh);
        kprintf("[AUTOSTART] Skip '%s': process_create failed\n", meta->filename);
        return NULL;
    }

    /* Raw code only when the FILE says so: the volume's word, never one the
     * process could be given some other way. */
    ImageSource source     = { file_size, fh, NULL, file_id };
    bool        tagged_raw = TagFSKeyAmong(meta->tag_ids, meta->tag_count, "raw");
    int         load_result = process_load_binary(proc, &source, meta->filename, tagged_raw);
    tagfs_close(fh);
    if (load_result != 0) {
        process_destroy(proc);
        kprintf("[AUTOSTART] Skip '%s': load_binary failed (%d)\n",
                meta->filename, load_result);
        return NULL;
    }

    return proc;
}

/*
 * ‼ A LATE LAUNCH SAYS WHAT IT STARTED BEFORE WHAT IT STARTED SPEAKS.
 *
 * At boot no program runs before one sweep enqueues them all, after the last
 * line about them (main.c). A volume that turns up later had each program let
 * run the moment it was made, so the volume's shell printed its banner and
 * prompt in among the kernel's lines — on a book on the stick, two runs of
 * three: `~ [AUTOSTART] the volume brought its own shell — the stand-in (PID 1)
 * hands over`, the prompt buried in the middle of the console. So the late
 * launch holds what it makes, says the launch, relieves the stand-in, and only
 * then lets them run, in the order made. Grown as they are found: a volume
 * asks for as many as it likes.
 */
typedef struct {
    process_t **procs;
    uint32_t    count;
    uint32_t    room;
} AutostartHeld;

/* `proc`, by the walk's reference, held until AutostartLetRun; false when no
 * room for it could be had. */
static bool AutostartHold(AutostartHeld *held, process_t *proc)
{
    if (held->count == held->room) {
        uint32_t    room  = held->room ? held->room * 2u : 4u;
        process_t **procs = kmalloc((size_t)room * sizeof(*procs));
        if (!procs) return false;
        if (held->count) memcpy(procs, held->procs, (size_t)held->count * sizeof(*procs));
        kfree(held->procs);
        held->procs = procs;
        held->room  = room;
    }
    held->procs[held->count++] = proc;
    return true;
}

/* A program of the late launch let run as it was made, and the walk's
 * reference given back. Only as made: its PID was said with its line, and a
 * stop or an end decided over it since stands (ProcessStartCreated) — a
 * `stopped` it wears is never run past. */
static void AutostartLetRunOne(process_t *proc)
{
    if (!ProcessStartCreated(proc))
        kprintf("[AUTOSTART] PID %u was stopped or ended before the launch let it run "
                "— left as it stands\n", proc->pid);
    process_ref_dec(proc);
}

static void AutostartLetRun(AutostartHeld *held)
{
    for (uint32_t i = 0; i < held->count; i++)
        AutostartLetRunOne(held->procs[i]);
    kfree(held->procs);
    held->procs = NULL;
    held->count = held->room = 0;
}

/* One file of the volume made into a process when it asks to be: the process,
 * loaded, not yet let run and referenced for the walk — its caller lets it run
 * and gives the reference back — or NULL when it does not ask or could not be
 * started (said by the launch). The reference is taken before the line names
 * the PID: a kill by that PID cannot free what the walk still holds. */
static process_t *autostart_consider(uint64_t file_id)
{
    TagFSMetadata meta;
    if (tagfs_get_metadata(file_id, &meta) != 0) return NULL;
    if (!(meta.flags & TAGFS_FILE_ACTIVE) || !autostart_wanted(&meta)) {
        tagfs_metadata_free(&meta);
        return NULL;
    }

    /* The file's keys, whole: a process started from it wears every one
     * (TagFSKeyList), never the ones that fitted a buffer. */
    char *tags = TagFSKeyList(meta.tag_ids, meta.tag_count);
    if (!tags) {
        kprintf("[AUTOSTART] Skip '%s': no memory for its tags\n", meta.filename);
        tagfs_metadata_free(&meta);
        return NULL;
    }

    process_t *proc = autostart_launch_one(file_id, &meta, tags);
    if (proc) {
        process_ref_inc(proc);
        kprintf("[AUTOSTART] Started '%s' (PID %u, tags: %s)\n",
                meta.filename, proc->pid, tags);
    }
    kfree(tags);
    tagfs_metadata_free(&meta);
    return proc;
}

/* Every file the volume tags to start, made into a process: with `held` NULL
 * the boot's — the state set directly, the sweep after the boot's last line
 * enqueuing it, exactly as it always was — else the late launch's, held. How
 * many were started. */
static int AutostartWalk(process_t **out_first, AutostartHeld *held)
{
    if (out_first) *out_first = NULL;

    /* Is there a volume? Asked of the flag rather than of the registry
     * pointer beside it, which a re-mount frees. */
    TagFSState *fs = tagfs_get_state();
    if (!fs || !fs->initialized) return 0;

    /* Every live file, a page at a time: what stood here was one array as
     * long as the Ledger's count of files — or 1024 when it said none — and
     * the files past it were never asked whether they start. */
    uint64_t  file_ids[32];
    TagFSPage page;
    uint64_t  from     = 1;
    int       launched = 0;
    do {
        if (TagFSListPage(from, file_ids, ARRAY_SIZE(file_ids), false, &page) != OK) break;
        for (uint32_t i = 0; i < page.delivered; i++) {
            process_t *proc = autostart_consider(file_ids[i]);
            if (!proc) continue;
            launched++;
            if (out_first && !*out_first) *out_first = proc;
            if (!held) {
                proc->state = PROC_WORKING;
                process_ref_dec(proc);
            } else if (!AutostartHold(held, proc)) {
                kprintf("[AUTOSTART] no memory to hold PID %u back until the launch is said "
                        "— it runs at once\n", proc->pid);
                AutostartLetRunOne(proc);
            }
        }
        from = page.next;
    } while (from != 0);

    return launched;
}

int AutostartLaunchFromVolume(process_t **out_first)
{
    return AutostartWalk(out_first, NULL);
}

/*
 * The relief has arrived, so the stand-in hands over the watch.
 *
 * A machine that came up with no volume is running the embedded shell. When
 * its own volume turns up carrying a display daemon and a shell of its own,
 * leaving both running would put two shells on one keyboard and two writers on
 * one screen. The one that was only ever standing in is the one that goes.
 *
 * Ended the way SysProcKill ends a program: whole (CabinEnd), each strand's
 * disposition claimed BEFORE its state changes, so the reaper on another core
 * cannot get there first and call an orderly hand-over a crash. The code is a
 * clean zero — this program did nothing wrong.
 */
static void autostart_relieve_stand_in(void)
{
    uint32_t pid = __atomic_exchange_n(&g_stand_in_pid, 0u, __ATOMIC_ACQ_REL);
    if (pid == 0) return;

    process_t *p = process_find_ref(pid);
    if (!p) return;

    kprintf("[AUTOSTART] the volume brought its own shell — the stand-in "
            "(PID %u) hands over\n", pid);

    CabinEnd(p, 0);
    __sync_synchronize();
    process_ref_dec(p);
}

static void autostart_on_volume_mounted(TouchTag tag, const void *payload,
                                        uint32_t plen, uint32_t source_pid,
                                        void *ctx)
{
    (void)tag; (void)payload; (void)plen; (void)source_pid; (void)ctx;

    if (__atomic_load_n(&g_volume_launched, __ATOMIC_ACQUIRE) != 0) return;
    if (__atomic_exchange_n(&g_busy, 1u, __ATOMIC_ACQUIRE) != 0) return;

    /* Re-checked inside the gate: two mounts can be announced close enough
     * together that both callers passed the load above. */
    if (__atomic_load_n(&g_volume_launched, __ATOMIC_ACQUIRE) == 0) {
        AutostartHeld held = { NULL, 0, 0 };
        int n = AutostartWalk(NULL, &held);
        if (n > 0) {
            __atomic_store_n(&g_volume_launched, 1u, __ATOMIC_RELEASE);
            kprintf("[AUTOSTART] a volume arrived after the boot had given up "
                    "— %d process(es) started from it\n", n);
            autostart_relieve_stand_in();
        }
        AutostartLetRun(&held);
    }

    __atomic_store_n(&g_busy, 0u, __ATOMIC_RELEASE);
}

void AutostartWatchVolume(void)
{
    if (g_mount_watch) return;

    TouchTag full = TOUCH_TAG_INVALID, bare = TOUCH_TAG_INVALID;
    TouchLogbookResolve("volume:mounted", &full, &bare);
    g_mount_watch = TouchWatchSet(full, autostart_on_volume_mounted, NULL);

    if (!g_mount_watch) {
        kprintf("[AUTOSTART] could not listen for a volume — programs on one "
                "that arrives later will not be started\n");
    }
}
