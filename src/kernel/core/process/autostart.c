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
#include "buffer_registry.h"

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

/* Collect a file's tags into the comma-separated string process_create wants.
 * The names are the volume's, so they are asked of it one at a time and copied
 * out: the registry they live in belongs to a mount, and a mount ends. */
static void autostart_collect_tags(const TagFSMetadata *meta,
                                   char *out, size_t out_size)
{
    size_t pos = 0;
    for (uint16_t t = 0; t < meta->tag_count; t++) {
        char key[128];
        if (!tagfs_tag_key(meta->tag_ids[t], key, sizeof(key))) continue;
        size_t klen = strlen(key);
        if (pos + klen + 2 > out_size) break;
        if (pos > 0) out[pos++] = ',';
        memcpy(out + pos, key, klen);
        pos += klen;
    }
    out[pos] = '\0';
}

static bool autostart_wanted(const TagFSMetadata *meta)
{
    bool has_autostart = false;
    bool has_exec_tag  = false;
    for (uint16_t t = 0; t < meta->tag_count; t++) {
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
static process_t *autostart_launch_one(uint32_t file_id, TagFSMetadata *meta,
                                       const char *tags)
{
    uint64_t file_size = meta->size;
    if (file_size == 0 || file_size > CONFIG_PROC_MAX_BINARY_SIZE) {
        kprintf("[AUTOSTART] Skip '%s': invalid size %lu\n",
                meta->filename, file_size);
        return NULL;
    }

    size_t pages_needed = (file_size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    void  *phys_buf = pmm_alloc_zero(pages_needed);
    if (!phys_buf) {
        kprintf("[AUTOSTART] Skip '%s': memory allocation failed\n", meta->filename);
        return NULL;
    }
    void *virt_buf = vmm_phys_to_virt((uintptr_t)phys_buf);

    TagFSFileHandle *fh = tagfs_open(file_id, TAGFS_HANDLE_READ);
    if (!fh) {
        pmm_free(phys_buf, pages_needed);
        kprintf("[AUTOSTART] Skip '%s': tagfs_open failed\n", meta->filename);
        return NULL;
    }
    /*
     * ‼ THE WHOLE IMAGE, OR NONE OF IT
     *
     * tagfs_read returns the number of bytes it managed. This tested it for
     * being negative, which catches a read that failed outright and misses the
     * one that stopped early — and the buffer underneath was allocated zeroed,
     * so a short read hands over an image that is partly, or entirely, zeros.
     *
     * A page of zeros is not an empty program. 0x00 0x00 decodes as
     * `add [rax], al`, so the process starts, writes to address zero with
     * every register still clear, and dies with a page fault at 0x0 — which is
     * exactly what a machine whose flash drive stopped answering showed on the
     * screen, having first announced that it had started the program.
     */
    int read_result = tagfs_read(fh, virt_buf, file_size);
    tagfs_close(fh);
    if (read_result < 0 || (uint64_t)read_result != file_size) {
        pmm_free(phys_buf, pages_needed);
        kprintf("[AUTOSTART] not starting '%s': %d of %llu bytes came back — "
                "an image that is not all here is not started\n",
                meta->filename, read_result, (unsigned long long)file_size);
        return NULL;
    }

    process_t *proc = process_create(tags);
    if (!proc) {
        pmm_free(phys_buf, pages_needed);
        kprintf("[AUTOSTART] Skip '%s': process_create failed\n", meta->filename);
        return NULL;
    }

    int load_result = process_load_binary(proc, virt_buf, (size_t)file_size);
    pmm_free(phys_buf, pages_needed);
    if (load_result != 0) {
        process_destroy(proc);
        kprintf("[AUTOSTART] Skip '%s': load_binary failed (%d)\n",
                meta->filename, load_result);
        return NULL;
    }

    return proc;
}

int AutostartLaunchFromVolume(process_t **out_first, bool scheduler_live)
{
    if (out_first) *out_first = NULL;

    /* Is there a volume? Asked of the flag rather than of the registry
     * pointer beside it, which a re-mount frees. */
    TagFSState *fs = tagfs_get_state();
    if (!fs || !fs->initialized) return 0;

    uint32_t max_files = (fs->ledger.total_files > 0)
                         ? fs->ledger.total_files : TAGFS_MAX_FILES;
    uint32_t *file_ids = kmalloc(sizeof(uint32_t) * max_files);
    if (!file_ids) {
        kprintf("[AUTOSTART] no memory to list the volume's files\n");
        return 0;
    }

    int file_count = tagfs_list_all_files(file_ids, max_files);
    int launched   = 0;

    for (int i = 0; i < file_count; i++) {
        TagFSMetadata meta;
        if (tagfs_get_metadata(file_ids[i], &meta) != 0) continue;
        if (!(meta.flags & TAGFS_FILE_ACTIVE) ||
            !autostart_wanted(&meta)) {
            tagfs_metadata_free(&meta);
            continue;
        }

        char tags[PROCESS_TAG_SIZE];
        autostart_collect_tags(&meta, tags, sizeof(tags));

        process_t *proc = autostart_launch_one(file_ids[i], &meta, tags);
        if (!proc) {
            tagfs_metadata_free(&meta);
            continue;
        }

        /*
         * At boot the scheduler is not running yet and every WORKING process is
         * swept onto a run queue in one pass afterwards, so the state is set
         * directly — exactly as it always was. Once the machine is up there is
         * no such pass, and process_set_state is what enqueues; it is the same
         * call the runtime spawn path makes.
         */
        if (scheduler_live) {
            process_set_state(proc, PROC_WORKING);
        } else {
            proc->state = PROC_WORKING;
        }

        launched++;
        kprintf("[AUTOSTART] Started '%s' (PID %u, tags: %s)\n",
                meta.filename, proc->pid, tags);
        tagfs_metadata_free(&meta);

        if (out_first && !*out_first) *out_first = proc;
    }

    kfree(file_ids);
    return launched;
}

/*
 * The relief has arrived, so the stand-in hands over the watch.
 *
 * A machine that came up with no volume is running the embedded shell. When
 * its own volume turns up carrying a display daemon and a shell of its own,
 * leaving both running would put two shells on one keyboard and two writers on
 * one screen. The one that was only ever standing in is the one that goes.
 *
 * Ended the way SysProcKill ends anything: the Touch subscriptions are torn
 * down and the disposition claimed BEFORE the state changes, so the reaper on
 * another core cannot get there first and call an orderly hand-over a crash.
 * The code is a clean zero — this process did nothing wrong.
 */
static void autostart_relieve_stand_in(void)
{
    uint32_t pid = __atomic_exchange_n(&g_stand_in_pid, 0u, __ATOMIC_ACQ_REL);
    if (pid == 0) return;

    process_t *p = process_find_ref(pid);
    if (!p) return;

    kprintf("[AUTOSTART] the volume brought its own shell — the stand-in "
            "(PID %u) hands over\n", pid);

    TouchCleanupProcess(p, 0);
    process_set_state(p, PROC_DONE);
    __sync_synchronize();
    BufferRegistryCleanupProcess(pid);
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
        int n = AutostartLaunchFromVolume(NULL, true);
        if (n > 0) {
            __atomic_store_n(&g_volume_launched, 1u, __ATOMIC_RELEASE);
            kprintf("[AUTOSTART] a volume arrived after the boot had given up "
                    "— %d process(es) started from it\n", n);
            autostart_relieve_stand_in();
        }
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
