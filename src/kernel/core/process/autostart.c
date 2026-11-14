#include "autostart.h"
#include "process.h"
#include "rise.h"
#include "tagfs.h"
#include "touch.h"
#include "logbook.h"
#include "pmm.h"
#include "vmm.h"
#include "klib.h"
#include "kernel_config.h"

/*
 * Autostart — the system's start set, whenever the volume turns up.
 *
 * The system starts three programs of its own and no others, named here by
 * their files and never by what anyone has labelled them with: the display
 * (it holds the screen), the hub (the person's prompt) and autostart.box,
 * which reads the person's list of programs from its Settings and starts
 * each of them itself. A file's tags are only the person's words for finding
 * it; nothing is started because of one.
 *
 * It used to be a hundred and ninety lines in the middle of kernel_main,
 * executed once, at a moment chosen by where it happened to sit in the boot.
 * A machine whose medium finished enumerating a second later came up with
 * nothing on it and stayed that way: the files were there, readable, and
 * nobody ever looked again.
 *
 * So it is a function, and it runs when a volume is mounted rather than when
 * the boot reaches a particular line. Twice at most: once from the boot, and
 * once more if the boot found no volume and one arrived afterwards.
 */

/* The start set, in the order it is started: the display first, so the
 * programs after it find the screen held. */
static const char *const g_start_set[] = { "display.box", "hub.box", AUTOSTART_PROGRAM };

/* Whether a volume's programs have ever been launched. One-shot: a volume that
 * leaves and comes back does not start second copies of everything, because
 * the first copies are still there. */
static volatile uint32_t g_volume_launched = 0;

/* One core does the late launch and the rest go away rather than queue up
 * behind it — the same arrangement the Boardroom uses for seating, and for
 * the same reason: this creates processes and reads a disk. */
static volatile uint32_t g_busy = 0;

static TouchWatch *g_mount_watch = NULL;

void AutostartNoteVolumeLaunched(void)
{
    __atomic_store_n(&g_volume_launched, 1u, __ATOMIC_RELEASE);
}

/* The cabin of autostart.box as started by the system — its pid and
 * generation packed into one word, 0 while there is none: what the programs
 * it starts on the system's behalf are told by (AutostartStartedBy). */
static volatile uint64_t g_launcher = 0;

static uint64_t autostart_pack(const process_t *proc)
{
    return ((uint64_t)proc->generation << 32) | proc->pid;
}

bool AutostartStartedBy(const process_t *spawner)
{
    uint64_t launcher = __atomic_load_n(&g_launcher, __ATOMIC_ACQUIRE);
    return launcher != 0 && spawner && autostart_pack(spawner) == launcher;
}

/* The live file named `name` — the first of them, by file id — or 0 when the
 * volume holds none. Asked of the volume by the stem of the name, the label
 * every file is made with, and held to the whole name here. */
static uint64_t autostart_find(const char *name)
{
    const char *dot    = strrchr(name, '.');
    size_t      length = dot ? (size_t)(dot - name) : strlen(name);
    TagSpelling stem;
    if (TagSpellKey(name, length, &stem) != OK) return 0;

    uint64_t  file_ids[32];
    TagFSPage page;
    uint64_t  from = 1;
    do {
        if (TagFSQueryPage(&stem, 1, NULL, 0, from, file_ids, ARRAY_SIZE(file_ids), false, &page) != OK)
            return 0;
        for (uint32_t i = 0; i < page.delivered; i++) {
            TagFSMetadata meta;
            if (tagfs_get_metadata(file_ids[i], &meta) != 0) continue;
            bool found = (meta.flags & TAGFS_FILE_ACTIVE) && strcmp(meta.filename, name) == 0;
            tagfs_metadata_free(&meta);
            if (found) return file_ids[i];
        }
        from = page.next;
    } while (from != 0);
    return 0;
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

    ImageSource source      = { file_size, fh, NULL, file_id };
    int         load_result = process_load_binary(proc, &source, meta->filename);
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
 * three, the prompt buried in the middle of the console. So the late launch
 * holds what it makes, says the launch, and only then lets them run, in the
 * order made. Grown as they are found: a volume asks for as many as it likes.
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

/* One file of the volume made into a process: the process, loaded, not yet
 * let run and referenced for the walk — its caller lets it run and gives the
 * reference back — or NULL when it is no longer a live file or could not be
 * started (said by the launch). The reference is taken before the line names
 * the PID: a kill by that PID cannot free what the walk still holds. A program
 * whose box says rise is watched from here, before it runs (rise.h); `again`
 * says it is a rise's start, not the volume's first. */
static process_t *autostart_consider(uint64_t file_id, bool again)
{
    TagFSMetadata meta;
    if (tagfs_get_metadata(file_id, &meta) != 0) return NULL;
    if (!(meta.flags & TAGFS_FILE_ACTIVE)) {
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
        RiseWatch(proc, file_id, meta.filename);
        if (strcmp(meta.filename, AUTOSTART_PROGRAM) == 0)
            __atomic_store_n(&g_launcher, autostart_pack(proc), __ATOMIC_RELEASE);
        kprintf(again ? "[RISE] '%s' raised again (PID %u, tags: %s)\n"
                      : "[AUTOSTART] Started '%s' (PID %u, tags: %s)\n",
                meta.filename, proc->pid, tags);
    }
    kfree(tags);
    tagfs_metadata_free(&meta);
    return proc;
}

/* Every program of the start set, made into a process: with `held` NULL the
 * boot's — the state set directly, the sweep after the boot's last line
 * enqueuing it, exactly as it always was — else the late launch's, held. How
 * many were started. A program the volume does not hold is said, by name. */
static int AutostartWalk(process_t **out_first, AutostartHeld *held)
{
    if (out_first) *out_first = NULL;

    if (!AutostartVolumeHere()) return 0;

    int launched = 0;
    for (size_t i = 0; i < ARRAY_SIZE(g_start_set); i++) {
        uint64_t file_id = autostart_find(g_start_set[i]);
        if (file_id == 0) {
            kprintf("[AUTOSTART] Skip '%s': the volume holds no such file\n", g_start_set[i]);
            continue;
        }
        process_t *proc = autostart_consider(file_id, false);
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

    return launched;
}

/* Asked of the flag rather than of the registry pointer beside it, which a
 * re-mount frees. */
bool AutostartVolumeHere(void)
{
    TagFSState *fs = tagfs_get_state();
    return fs && fs->initialized;
}

int AutostartLaunchFromVolume(process_t **out_first)
{
    return AutostartWalk(out_first, NULL);
}

process_t *AutostartAgain(uint64_t file_id)
{
    return AutostartVolumeHere() ? autostart_consider(file_id, true) : NULL;
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
