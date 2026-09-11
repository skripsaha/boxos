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


static volatile uint32_t g_stand_in_pid = 0;

static volatile uint32_t g_volume_launched = 0;

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
    process_ref_dec(p);
}

static void autostart_on_volume_mounted(TouchTag tag, const void *payload,
                                        uint32_t plen, uint32_t source_pid,
                                        void *ctx)
{
    (void)tag; (void)payload; (void)plen; (void)source_pid; (void)ctx;

    if (__atomic_load_n(&g_volume_launched, __ATOMIC_ACQUIRE) != 0) return;
    if (__atomic_exchange_n(&g_busy, 1u, __ATOMIC_ACQUIRE) != 0) return;

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