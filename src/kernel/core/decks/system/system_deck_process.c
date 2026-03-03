#include "system_deck_process.h"
#include "klib.h"
#include "process.h"
#include "vmm.h"
#include "pmm.h"
#include "atomics.h"
#include "tagfs.h"
#include "notify_page.h"

typedef struct {
    uint64_t handle;
    uint64_t phys_addr;
    uint64_t virt_addr;  // mapped virtual address in owner's cabin
    uint64_t size;
    uint32_t owner_pid;
    bool in_use;
} buffer_entry_t;

static buffer_entry_t buffer_table[BUF_MAX_COUNT];
static uint64_t next_buffer_handle = 1;
static spinlock_t buffer_table_lock = {0};

static void deliver_response(Event* event, uint16_t error_code, const void* response_data, size_t response_size) {
    if (!event) {
        return;
    }

    memset(event->data, 0, EVENT_DATA_SIZE);

    if (response_data && response_size > 0) {
        if (response_size > EVENT_DATA_SIZE) {
            debug_printf("[SYSTEM_DECK] ERROR: response_size (%zu) exceeds EVENT_DATA_SIZE (%u)\n",
                    response_size, EVENT_DATA_SIZE);
            response_size = EVENT_DATA_SIZE;
        }
        size_t copy_size = response_size < EVENT_DATA_SIZE ? response_size : EVENT_DATA_SIZE;
        memcpy(event->data, response_data, copy_size);
    }

    __sync_synchronize();

    if (error_code == SYSTEM_ERR_SUCCESS) {
        event->state = EVENT_STATE_COMPLETED;
    } else {
        event->state = EVENT_STATE_ERROR;
    }
}

int system_deck_proc_spawn(Event* event) {
    if (!event) {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: NULL event\n");
        return -1;
    }

    proc_spawn_event_t spawn_event;
    memset(&spawn_event, 0, sizeof(spawn_event));
    memcpy(&spawn_event, event->data, sizeof(proc_spawn_event_t));

    // check BEFORE forcing null termination
    if (strnlen(spawn_event.tags, sizeof(spawn_event.tags)) == sizeof(spawn_event.tags)) {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: Tags not properly null-terminated\n");
        deliver_response(event, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    spawn_event.tags[sizeof(spawn_event.tags) - 1] = '\0';

    // check for empty tags (strnlen already prevents embedded nulls)
    if (spawn_event.tags[0] == '\0') {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: Empty tags not allowed\n");
        deliver_response(event, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    debug_printf("[SYSTEM_DECK] PROC_SPAWN: binary_phys=0x%lx size=%lu tags='%s'\n",
            spawn_event.binary_phys_addr, spawn_event.binary_size, spawn_event.tags);

    if (spawn_event.binary_phys_addr == 0 || spawn_event.binary_size == 0) {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: Invalid binary address or size\n");
        deliver_response(event, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    if (spawn_event.binary_phys_addr & 0xFFF) {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: Binary address not page-aligned (0x%lx)\n",
                spawn_event.binary_phys_addr);
        deliver_response(event, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    if (spawn_event.binary_phys_addr + spawn_event.binary_size < spawn_event.binary_phys_addr) {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: Binary address range overflows\n");
        deliver_response(event, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    if (spawn_event.binary_phys_addr > PROC_SPAWN_MAX_PHYS_ADDR) {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: Binary address too high (0x%lx)\n",
                spawn_event.binary_phys_addr);
        deliver_response(event, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    #define MAX_SAFE_BINARY_SIZE (SIZE_MAX - 4096)
    if (spawn_event.binary_size > MAX_SAFE_BINARY_SIZE) {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: Binary size would cause overflow\n");
        deliver_response(event, SYSTEM_ERR_SIZE_LIMIT, NULL, 0);
        return -1;
    }

    if (spawn_event.binary_size > PROC_SPAWN_MAX_BINARY_SIZE) {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: Binary too large (%lu > %u bytes)\n",
                spawn_event.binary_size, PROC_SPAWN_MAX_BINARY_SIZE);
        deliver_response(event, SYSTEM_ERR_SIZE_LIMIT, NULL, 0);
        return -1;
    }

    if (process_get_count() >= PROCESS_MAX_COUNT) {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: Process limit reached\n");
        deliver_response(event, SYSTEM_ERR_PROCESS_LIMIT, NULL, 0);
        return -1;
    }

    process_t* proc = process_create(spawn_event.tags);
    if (!proc) {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: Failed to create process\n");
        deliver_response(event, SYSTEM_ERR_CABIN_FAILED, NULL, 0);
        return -1;
    }

    proc->spawner_pid = event->pid;
    notify_page_t* spawn_np = (notify_page_t*)vmm_phys_to_virt(proc->notify_page_phys);
    if (spawn_np) {
        spawn_np->magic = NOTIFY_PAGE_MAGIC;
        spawn_np->spawner_pid = event->pid;
    }

    void* binary_virt = vmm_phys_to_virt(spawn_event.binary_phys_addr);

    const uint8_t* elf_hdr = (const uint8_t*)binary_virt;
    if (spawn_event.binary_size < 16 ||
        elf_hdr[0] != 0x7F || elf_hdr[1] != 'E' ||
        elf_hdr[2] != 'L' || elf_hdr[3] != 'F') {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: Invalid ELF magic\n");
        process_destroy(proc);
        deliver_response(event, SYSTEM_ERR_LOAD_FAILED, NULL, 0);
        return -1;
    }

    int load_result = process_load_binary(proc, binary_virt, spawn_event.binary_size);
    if (load_result != 0) {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: Failed to load binary\n");
        process_destroy(proc);
        deliver_response(event, SYSTEM_ERR_LOAD_FAILED, NULL, 0);
        return -1;
    }

    proc_spawn_response_t response;
    memset(&response, 0, sizeof(response));
    response.new_pid = proc->pid;

    debug_printf("[SYSTEM_DECK] PROC_SPAWN: SUCCESS - created PID %u\n", proc->pid);
    deliver_response(event, SYSTEM_ERR_SUCCESS, &response, sizeof(response));
    return 0;
}

int system_deck_proc_kill(Event* event) {
    if (!event) {
        debug_printf("[SYSTEM_DECK] PROC_KILL: NULL event\n");
        return -1;
    }

    proc_kill_event_t kill_event;
    memcpy(&kill_event, event->data, sizeof(proc_kill_event_t));

    // Allow target_pid=0 to mean "kill self" (process exit)
    uint32_t target_pid = kill_event.target_pid;
    bool is_self_exit = (target_pid == 0);

    if (is_self_exit) {
        target_pid = event->pid;
        // Check if this is from box_proc_exit (exit code in data)
        uint32_t exit_code = kill_event.target_pid;  // Reinterpret first field as exit code
        debug_printf("[SYSTEM_DECK] PROC_KILL: Process %u exiting with code %u\n",
                     target_pid, exit_code);
    } else {
        debug_printf("[SYSTEM_DECK] PROC_KILL: Killing target_pid=%u\n", target_pid);
    }

    if (target_pid == PROCESS_INVALID_PID) {
        debug_printf("[SYSTEM_DECK] PROC_KILL: Invalid PID\n");
        deliver_response(event, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    process_t* proc = process_find(target_pid);
    if (!proc) {
        debug_printf("[SYSTEM_DECK] PROC_KILL: Process %u not found\n", target_pid);
        deliver_response(event, SYSTEM_ERR_PROCESS_NOT_FOUND, NULL, 0);
        return -1;
    }

    // For self-exit, use ZOMBIE state (allows in-flight events to complete)
    // For kill, use TERMINATED (forceful termination)
    if (is_self_exit) {
        process_set_state(proc, PROC_DONE);
        debug_printf("[SYSTEM_DECK] PROC_KILL: Marked PID %u as DONE (graceful exit)\n", target_pid);
    } else {
        process_set_state(proc, PROC_CRASHED);
        debug_printf("[SYSTEM_DECK] PROC_KILL: Marked PID %u as CRASHED (forced kill)\n", target_pid);
    }

    __sync_synchronize();

    system_deck_cleanup_process_buffers(target_pid);

    proc_kill_response_t response;
    memset(&response, 0, sizeof(response));
    response.killed_pid = target_pid;

    debug_printf("[SYSTEM_DECK] PROC_KILL: SUCCESS\n");
    deliver_response(event, SYSTEM_ERR_SUCCESS, &response, sizeof(response));
    return 0;
}

int system_deck_proc_info(Event* event) {
    if (!event) {
        debug_printf("[SYSTEM_DECK] PROC_INFO: NULL event\n");
        return -1;
    }

    proc_info_event_t info_event;
    memcpy(&info_event, event->data, sizeof(proc_info_event_t));

    uint32_t target_pid = info_event.target_pid;

    if (target_pid == 0) {
        target_pid = event->pid;
        debug_printf("[SYSTEM_DECK] PROC_INFO: Query self (PID %u)\n", target_pid);
    } else {
        debug_printf("[SYSTEM_DECK] PROC_INFO: Query PID %u\n", target_pid);
    }

    process_t* proc = process_find(target_pid);
    if (!proc) {
        debug_printf("[SYSTEM_DECK] PROC_INFO: Process %u not found\n", target_pid);
        deliver_response(event, SYSTEM_ERR_PROCESS_NOT_FOUND, NULL, 0);
        return -1;
    }

    proc_info_response_t response;
    memset(&response, 0, sizeof(response));

    response.pid = proc->pid;
    response.state = (uint32_t)proc->state;
    response.score = proc->score;
    response.notify_page_phys = proc->notify_page_phys;
    response.result_page_phys = proc->result_page_phys;
    response.code_start = proc->code_start;
    response.code_size = proc->code_size;
    response.result_there = proc->result_there;

    strncpy(response.tags, proc->tags, PROC_INFO_TAGS_SIZE);
    response.tags[PROC_INFO_TAGS_SIZE - 1] = '\0';

    debug_printf("[SYSTEM_DECK] PROC_INFO: SUCCESS - returned info for PID %u\n", proc->pid);
    deliver_response(event, SYSTEM_ERR_SUCCESS, &response, sizeof(response));
    return 0;
}

int system_deck_proc_exec(Event* event) {
    if (!event) {
        debug_printf("[SYSTEM_DECK] PROC_EXEC: NULL event\n");
        return -1;
    }

    proc_exec_event_t exec_event;
    memset(&exec_event, 0, sizeof(exec_event));
    memcpy(&exec_event, event->data, sizeof(proc_exec_event_t));

    if (strnlen(exec_event.filename, sizeof(exec_event.filename))
            == sizeof(exec_event.filename)) {
        debug_printf("[SYSTEM_DECK] PROC_EXEC: filename not null-terminated\n");
        deliver_response(event, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    exec_event.filename[sizeof(exec_event.filename) - 1] = '\0';

    if (exec_event.filename[0] == '\0') {
        debug_printf("[SYSTEM_DECK] PROC_EXEC: empty filename\n");
        deliver_response(event, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    debug_printf("[SYSTEM_DECK] PROC_EXEC: looking for '%s'\n",
            exec_event.filename);

    if (process_get_count() >= PROCESS_MAX_COUNT) {
        debug_printf("[SYSTEM_DECK] PROC_EXEC: process limit reached\n");
        deliver_response(event, SYSTEM_ERR_PROCESS_LIMIT, NULL, 0);
        return -1;
    }

    // heap-allocate file_ids to avoid 4KB+ stack usage on 16KB kernel stack
    #define PROC_EXEC_MAX_SCAN 256
    uint32_t *file_ids = kmalloc(PROC_EXEC_MAX_SCAN * sizeof(uint32_t));
    if (!file_ids) {
        debug_printf("[SYSTEM_DECK] PROC_EXEC: kmalloc for file scan failed\n");
        deliver_response(event, SYSTEM_ERR_NO_MEMORY, NULL, 0);
        return -1;
    }
    int file_count = tagfs_list_all_files(file_ids, PROC_EXEC_MAX_SCAN);

    uint32_t found_id = 0;
    char found_tags[PROCESS_TAG_SIZE];
    found_tags[0] = '\0';

    /* Search by tags, not by filename.
     * A file is executable if it has ALL of:
     *   1. A label tag whose key matches the requested name
     *      (create_tagfs auto-adds key=<stem> for every file)
     *   2. An "app" or "utility" tag (marks it as runnable)
     *
     * When found, ALL system tags are copied to the process so it
     * inherits the file's full permission set (e.g. "system" tag
     * grants VGA/keyboard access).
     */
    for (int i = 0; i < file_count; i++) {
        TagFSMetadata* meta = tagfs_get_metadata(file_ids[i]);
        if (!meta || !(meta->flags & TAGFS_FILE_ACTIVE)) {
            continue;
        }

        bool has_name  = false;
        bool has_exec_tag = false;

        for (uint8_t t = 0; t < meta->tag_count; t++) {
            if (meta->tags[t].type != TAGFS_TAG_SYSTEM) {
                continue;
            }
            const char* key = meta->tags[t].key;
            if (strcmp(key, exec_event.filename) == 0) {
                has_name = true;
            }
            if (strcmp(key, "app") == 0 || strcmp(key, "utility") == 0) {
                has_exec_tag = true;
            }
        }

        if (has_name && has_exec_tag) {
            found_id = file_ids[i];

            size_t pos = 0;
            for (uint8_t t = 0; t < meta->tag_count; t++) {
                if (meta->tags[t].type != TAGFS_TAG_SYSTEM) {
                    continue;
                }
                const char* key = meta->tags[t].key;
                size_t klen = strlen(key);
                if (pos + klen + 2 > PROCESS_TAG_SIZE) {
                    break;  // No room
                }
                if (pos > 0) {
                    found_tags[pos++] = ',';
                }
                memcpy(found_tags + pos, key, klen);
                pos += klen;
            }
            found_tags[pos] = '\0';
            break;
        }
    }

    // file_ids no longer needed after the search loop
    kfree(file_ids);
    file_ids = NULL;

    if (found_id == 0) {
        debug_printf("[SYSTEM_DECK] PROC_EXEC: '%s' not found as executable\n",
                exec_event.filename);
        deliver_response(event, SYSTEM_ERR_EXEC_NOT_FOUND, NULL, 0);
        return -1;
    }

    TagFSMetadata* meta = tagfs_get_metadata(found_id);
    uint64_t file_size = meta->size;

    if (file_size == 0 || file_size > PROC_SPAWN_MAX_BINARY_SIZE) {
        debug_printf("[SYSTEM_DECK] PROC_EXEC: invalid file size %lu\n",
                file_size);
        deliver_response(event, SYSTEM_ERR_SIZE_LIMIT, NULL, 0);
        return -1;
    }

    size_t pages_needed = (file_size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    void* phys_buf = pmm_alloc_zero(pages_needed);
    if (!phys_buf) {
        debug_printf("[SYSTEM_DECK] PROC_EXEC: pmm_alloc_zero failed\n");
        deliver_response(event, SYSTEM_ERR_NO_MEMORY, NULL, 0);
        return -1;
    }

    void* virt_buf = vmm_phys_to_virt((uintptr_t)phys_buf);

    TagFSFileHandle* fh = tagfs_open(found_id, TAGFS_HANDLE_READ);
    if (!fh) {
        debug_printf("[SYSTEM_DECK] PROC_EXEC: tagfs_open failed\n");
        pmm_free(phys_buf, pages_needed);
        deliver_response(event, SYSTEM_ERR_LOAD_FAILED, NULL, 0);
        return -1;
    }

    int read_result = tagfs_read(fh, virt_buf, file_size);
    tagfs_close(fh);

    /* tagfs_read returns bytes_read on success, negative on error */
    if (read_result < 0) {
        debug_printf("[SYSTEM_DECK] PROC_EXEC: tagfs_read failed (%d)\n",
                read_result);
        pmm_free(phys_buf, pages_needed);
        deliver_response(event, SYSTEM_ERR_LOAD_FAILED, NULL, 0);
        return -1;
    }

    process_t* proc = process_create(found_tags);
    if (!proc) {
        debug_printf("[SYSTEM_DECK] PROC_EXEC: process_create failed\n");
        pmm_free(phys_buf, pages_needed);
        deliver_response(event, SYSTEM_ERR_CABIN_FAILED, NULL, 0);
        return -1;
    }

    int load_result = process_load_binary(proc, virt_buf, (size_t)file_size);
    pmm_free(phys_buf, pages_needed);

    if (load_result != 0) {
        debug_printf("[SYSTEM_DECK] PROC_EXEC: process_load_binary failed\n");
        process_destroy(proc);
        deliver_response(event, SYSTEM_ERR_LOAD_FAILED, NULL, 0);
        return -1;
    }

    // Set parent PID BEFORE marking process as READY to prevent race condition:
    // scheduler could run the process before spawner_pid is initialized
    proc->spawner_pid = event->pid;
    notify_page_t* exec_np = (notify_page_t*)vmm_phys_to_virt(proc->notify_page_phys);
    if (exec_np) {
        exec_np->magic = NOTIFY_PAGE_MAGIC;
        exec_np->spawner_pid = event->pid;
    }
    __sync_synchronize();

    process_set_state(proc, PROC_WORKING);

    proc_exec_response_t response;
    memset(&response, 0, sizeof(response));
    response.new_pid = proc->pid;

    debug_printf("[SYSTEM_DECK] PROC_EXEC: SUCCESS - '%s' -> PID %u\n",
            exec_event.filename, proc->pid);
    deliver_response(event, SYSTEM_ERR_SUCCESS, &response, sizeof(response));
    return 0;
}

int system_deck_tag_add(Event* event) {
    if (!event) {
        debug_printf("[SYSTEM_DECK] TAG_ADD: NULL event\n");
        return -1;
    }

    tag_modify_event_t tag_event;
    memcpy(&tag_event, event->data, sizeof(tag_modify_event_t));

    // check BEFORE forcing null termination
    if (strnlen(tag_event.tag, sizeof(tag_event.tag)) == sizeof(tag_event.tag)) {
        debug_printf("[SYSTEM_DECK] TAG_ADD: Tag not properly null-terminated\n");
        deliver_response(event, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    tag_event.tag[sizeof(tag_event.tag) - 1] = '\0';

    // check for empty tag (strnlen already prevents embedded nulls)
    if (tag_event.tag[0] == '\0') {
        debug_printf("[SYSTEM_DECK] TAG_ADD: Empty tag not allowed\n");
        deliver_response(event, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    debug_printf("[SYSTEM_DECK] TAG_ADD: target_pid=%u tag='%s'\n",
            tag_event.target_pid, tag_event.tag);

    if (tag_event.target_pid == PROCESS_INVALID_PID) {
        debug_printf("[SYSTEM_DECK] TAG_ADD: Invalid PID\n");
        deliver_response(event, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    process_t* proc = process_find(tag_event.target_pid);
    if (!proc) {
        debug_printf("[SYSTEM_DECK] TAG_ADD: Process %u not found\n", tag_event.target_pid);
        deliver_response(event, SYSTEM_ERR_PROCESS_NOT_FOUND, NULL, 0);
        return -1;
    }

    if (process_has_tag(proc, tag_event.tag)) {
        debug_printf("[SYSTEM_DECK] TAG_ADD: Tag already exists\n");
        tag_modify_response_t response;
        memset(&response, 0, sizeof(response));
        response.error_code = SYSTEM_ERR_TAG_EXISTS;
        strncpy(response.message, "Tag already exists", sizeof(response.message) - 1);
        deliver_response(event, SYSTEM_ERR_TAG_EXISTS, &response, sizeof(response));
        return -1;
    }

    int result = process_add_tag(proc, tag_event.tag);
    if (result != 0) {
        debug_printf("[SYSTEM_DECK] TAG_ADD: Failed to add tag (tags full)\n");
        tag_modify_response_t response;
        memset(&response, 0, sizeof(response));
        response.error_code = SYSTEM_ERR_TAG_FULL;
        strncpy(response.message, "Tag buffer full", sizeof(response.message) - 1);
        deliver_response(event, SYSTEM_ERR_TAG_FULL, &response, sizeof(response));
        return -1;
    }

    // Tag "stopped" freezes the process
    if (strcmp(tag_event.tag, "stopped") == 0) {
        process_state_t state = process_get_state(proc);
        if (state == PROC_WORKING || state == PROC_CREATED) {
            process_set_state(proc, PROC_STOPPED);
        }
    }

    tag_modify_response_t response;
    memset(&response, 0, sizeof(response));
    response.error_code = SYSTEM_ERR_SUCCESS;
    strncpy(response.message, "Tag added successfully", sizeof(response.message) - 1);

    debug_printf("[SYSTEM_DECK] TAG_ADD: SUCCESS - added tag '%s' to PID %u\n",
            tag_event.tag, tag_event.target_pid);
    deliver_response(event, SYSTEM_ERR_SUCCESS, &response, sizeof(response));
    return 0;
}

int system_deck_tag_remove(Event* event) {
    if (!event) {
        debug_printf("[SYSTEM_DECK] TAG_REMOVE: NULL event\n");
        return -1;
    }

    tag_modify_event_t tag_event;
    memcpy(&tag_event, event->data, sizeof(tag_modify_event_t));

    // check BEFORE forcing null termination
    if (strnlen(tag_event.tag, sizeof(tag_event.tag)) == sizeof(tag_event.tag)) {
        debug_printf("[SYSTEM_DECK] TAG_REMOVE: Tag not properly null-terminated\n");
        deliver_response(event, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    tag_event.tag[sizeof(tag_event.tag) - 1] = '\0';

    // check for empty tag (strnlen already prevents embedded nulls)
    if (tag_event.tag[0] == '\0') {
        debug_printf("[SYSTEM_DECK] TAG_REMOVE: Empty tag not allowed\n");
        deliver_response(event, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    debug_printf("[SYSTEM_DECK] TAG_REMOVE: target_pid=%u tag='%s'\n",
            tag_event.target_pid, tag_event.tag);

    if (tag_event.target_pid == PROCESS_INVALID_PID) {
        debug_printf("[SYSTEM_DECK] TAG_REMOVE: Invalid PID\n");
        deliver_response(event, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    process_t* proc = process_find(tag_event.target_pid);
    if (!proc) {
        debug_printf("[SYSTEM_DECK] TAG_REMOVE: Process %u not found\n", tag_event.target_pid);
        deliver_response(event, SYSTEM_ERR_PROCESS_NOT_FOUND, NULL, 0);
        return -1;
    }

    if (!process_has_tag(proc, tag_event.tag)) {
        debug_printf("[SYSTEM_DECK] TAG_REMOVE: Tag not found\n");
        tag_modify_response_t response;
        memset(&response, 0, sizeof(response));
        response.error_code = SYSTEM_ERR_TAG_NOT_FOUND;
        strncpy(response.message, "Tag not found", sizeof(response.message) - 1);
        deliver_response(event, SYSTEM_ERR_TAG_NOT_FOUND, &response, sizeof(response));
        return -1;
    }

    int result = process_remove_tag(proc, tag_event.tag);
    if (result != 0) {
        debug_printf("[SYSTEM_DECK] TAG_REMOVE: Failed to remove tag\n");
        deliver_response(event, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    // Removing "stopped" tag unfreezes the process
    if (strcmp(tag_event.tag, "stopped") == 0) {
        process_state_t state = process_get_state(proc);
        if (state == PROC_STOPPED) {
            process_set_state(proc, PROC_WORKING);
        }
    }

    tag_modify_response_t response;
    memset(&response, 0, sizeof(response));
    response.error_code = SYSTEM_ERR_SUCCESS;
    strncpy(response.message, "Tag removed successfully", sizeof(response.message) - 1);

    debug_printf("[SYSTEM_DECK] TAG_REMOVE: SUCCESS - removed tag '%s' from PID %u\n",
            tag_event.tag, tag_event.target_pid);
    deliver_response(event, SYSTEM_ERR_SUCCESS, &response, sizeof(response));
    return 0;
}

int system_deck_tag_check(Event* event) {
    if (!event) {
        debug_printf("[SYSTEM_DECK] TAG_CHECK: NULL event\n");
        return -1;
    }

    tag_check_event_t tag_event;
    memcpy(&tag_event, event->data, sizeof(tag_check_event_t));

    // check BEFORE forcing null termination
    if (strnlen(tag_event.tag, sizeof(tag_event.tag)) == sizeof(tag_event.tag)) {
        debug_printf("[SYSTEM_DECK] TAG_CHECK: Tag not properly null-terminated\n");
        deliver_response(event, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    tag_event.tag[sizeof(tag_event.tag) - 1] = '\0';

    // check for empty tag (strnlen already prevents embedded nulls)
    if (tag_event.tag[0] == '\0') {
        debug_printf("[SYSTEM_DECK] TAG_CHECK: Empty tag not allowed\n");
        deliver_response(event, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    debug_printf("[SYSTEM_DECK] TAG_CHECK: target_pid=%u tag='%s'\n",
            tag_event.target_pid, tag_event.tag);

    if (tag_event.target_pid == PROCESS_INVALID_PID) {
        debug_printf("[SYSTEM_DECK] TAG_CHECK: Invalid PID\n");
        deliver_response(event, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    process_t* proc = process_find(tag_event.target_pid);
    if (!proc) {
        debug_printf("[SYSTEM_DECK] TAG_CHECK: Process %u not found\n", tag_event.target_pid);
        deliver_response(event, SYSTEM_ERR_PROCESS_NOT_FOUND, NULL, 0);
        return -1;
    }

    tag_check_response_t response;
    memset(&response, 0, sizeof(response));
    response.has_tag = process_has_tag(proc, tag_event.tag);
    response.error_code = SYSTEM_ERR_SUCCESS;

    debug_printf("[SYSTEM_DECK] TAG_CHECK: SUCCESS - PID %u %s tag '%s'\n",
            tag_event.target_pid, response.has_tag ? "has" : "does not have", tag_event.tag);
    deliver_response(event, SYSTEM_ERR_SUCCESS, &response, sizeof(response));
    return 0;
}

static int find_free_buffer_slot(void) {
    for (int i = 0; i < BUF_MAX_COUNT; i++) {
        if (!buffer_table[i].in_use) {
            return i;
        }
    }
    return -1;
}

static int find_buffer_by_handle(uint64_t handle) {
    for (int i = 0; i < BUF_MAX_COUNT; i++) {
        if (buffer_table[i].in_use && buffer_table[i].handle == handle) {
            return i;
        }
    }
    return -1;
}

static uint32_t count_process_buffers(uint32_t pid) {
    uint32_t count = 0;
    for (int i = 0; i < BUF_MAX_COUNT; i++) {
        if (buffer_table[i].in_use && buffer_table[i].owner_pid == pid) {
            count++;
        }
    }
    return count;
}

void system_deck_cleanup_process_buffers(uint32_t pid) {
    process_t* proc = process_find(pid);

    spin_lock(&buffer_table_lock);

    for (int i = 0; i < BUF_MAX_COUNT; i++) {
        if (buffer_table[i].in_use && buffer_table[i].owner_pid == pid) {
            void* phys_addr = (void*)buffer_table[i].phys_addr;
            uint64_t virt_addr = buffer_table[i].virt_addr;
            size_t pages = buffer_table[i].size / PMM_PAGE_SIZE;

            debug_printf("[SYSTEM_DECK] Cleaning up buffer handle=%lu for PID %u\n",
                    buffer_table[i].handle, pid);

            if (virt_addr != 0 && proc && proc->cabin) {
                vmm_unmap_pages(proc->cabin, virt_addr, pages);
            }

            pmm_free(phys_addr, pages);

            __sync_synchronize();

            buffer_table[i].in_use = false;
            buffer_table[i].handle = 0;
            buffer_table[i].phys_addr = 0;
            buffer_table[i].virt_addr = 0;
            buffer_table[i].size = 0;
            buffer_table[i].owner_pid = 0;
        }
    }

    spin_unlock(&buffer_table_lock);
}

static uint64_t generate_random_handle(void) {
    // Use TSC + counter for unpredictable handles
    // TSC provides high-resolution entropy, counter ensures uniqueness
    uint64_t tsc = rdtsc();
    uint64_t counter = atomic_fetch_add_u64(&next_buffer_handle, 1);

    // Mix TSC and counter with XOR and rotation for better distribution
    uint64_t result = (tsc ^ (counter << 32) ^ (counter >> 32));

    // Never return 0 (reserved as invalid)
    return result ? result : 1;
}

int system_deck_buf_alloc(Event* event) {
    if (!event) {
        debug_printf("[SYSTEM_DECK] BUF_ALLOC: NULL event\n");
        return -1;
    }

    buf_alloc_event_t alloc_event;
    memcpy(&alloc_event, event->data, sizeof(buf_alloc_event_t));

    debug_printf("[SYSTEM_DECK] BUF_ALLOC: size=%lu flags=0x%x\n",
            alloc_event.size, alloc_event.flags);

    if (alloc_event.size == 0) {
        debug_printf("[SYSTEM_DECK] BUF_ALLOC: Invalid size (zero)\n");
        deliver_response(event, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    if (alloc_event.size > BUF_MAX_SIZE) {
        debug_printf("[SYSTEM_DECK] BUF_ALLOC: Size exceeds limit (%lu > %u)\n",
                alloc_event.size, BUF_MAX_SIZE);
        deliver_response(event, SYSTEM_ERR_SIZE_LIMIT, NULL, 0);
        return -1;
    }

    if (alloc_event.size > UINT64_MAX - PMM_PAGE_SIZE) {
        debug_printf("[SYSTEM_DECK] BUF_ALLOC: Size would cause overflow in page calculation\n");
        deliver_response(event, SYSTEM_ERR_SIZE_LIMIT, NULL, 0);
        return -1;
    }

    spin_lock(&buffer_table_lock);

    // check buffer limit inside lock to prevent race
    if (count_process_buffers(event->pid) >= BUF_MAX_COUNT / 4) {
        spin_unlock(&buffer_table_lock);
        debug_printf("[SYSTEM_DECK] BUF_ALLOC: Process buffer limit reached\n");
        deliver_response(event, SYSTEM_ERR_BUFFER_LIMIT, NULL, 0);
        return -1;
    }

    int slot = find_free_buffer_slot();
    if (slot < 0) {
        spin_unlock(&buffer_table_lock);
        debug_printf("[SYSTEM_DECK] BUF_ALLOC: Global buffer table full\n");
        deliver_response(event, SYSTEM_ERR_BUFFER_LIMIT, NULL, 0);
        return -1;
    }

    uint64_t handle = generate_random_handle();

    // Check for handle collision (rare with random generation)
    int collision_check_count = 0;
    while (find_buffer_by_handle(handle) >= 0) {
        handle = generate_random_handle();
        collision_check_count++;
        if (collision_check_count > BUF_MAX_COUNT) {
            // This should never happen unless buffer table is completely full
            spin_unlock(&buffer_table_lock);
            debug_printf("[SYSTEM_DECK] BUF_ALLOC: Handle generation failed (all handles in use)\n");
            deliver_response(event, SYSTEM_ERR_BUFFER_LIMIT, NULL, 0);
            return -1;
        }
    }

    // Release lock before potentially slow PMM allocation
    spin_unlock(&buffer_table_lock);

    size_t pages_needed = (alloc_event.size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    void* phys_addr = pmm_alloc_zero(pages_needed);

    __sync_synchronize();

    if (!phys_addr) {
        debug_printf("[SYSTEM_DECK] BUF_ALLOC: PMM allocation failed\n");
        deliver_response(event, SYSTEM_ERR_NO_MEMORY, NULL, 0);
        return -1;
    }

    // re-acquire lock to update buffer table
    spin_lock(&buffer_table_lock);

    // Verify slot is still free (should be, but be defensive)
    if (buffer_table[slot].in_use) {
        spin_unlock(&buffer_table_lock);
        pmm_free(phys_addr, pages_needed);
        debug_printf("[SYSTEM_DECK] BUF_ALLOC: Slot became occupied during allocation\n");
        deliver_response(event, SYSTEM_ERR_BUFFER_LIMIT, NULL, 0);
        return -1;
    }

    buffer_table[slot].handle = handle;
    buffer_table[slot].phys_addr = (uint64_t)phys_addr;
    buffer_table[slot].size = pages_needed * PMM_PAGE_SIZE;
    buffer_table[slot].owner_pid = event->pid;
    buffer_table[slot].virt_addr = 0;
    buffer_table[slot].in_use = true;

    spin_unlock(&buffer_table_lock);

    // Map buffer into process cabin so userspace can access it directly
    uint64_t virt_addr = 0;
    process_t* proc = process_find(event->pid);
    if (proc && proc->cabin) {
        virt_addr = proc->buf_heap_next;
        vmm_map_result_t map_result = vmm_map_pages(proc->cabin, virt_addr,
                (uintptr_t)phys_addr, pages_needed, VMM_FLAGS_USER_RW);
        if (map_result.success) {
            proc->buf_heap_next += pages_needed * PMM_PAGE_SIZE;
            spin_lock(&buffer_table_lock);
            buffer_table[slot].virt_addr = virt_addr;
            spin_unlock(&buffer_table_lock);
        } else {
            debug_printf("[SYSTEM_DECK] BUF_ALLOC: Warning - failed to map into cabin\n");
            virt_addr = 0;
        }
    }

    buf_alloc_response_t response;
    memset(&response, 0, sizeof(response));
    response.buffer_handle = handle;
    response.phys_addr = (uint64_t)phys_addr;
    response.actual_size = pages_needed * PMM_PAGE_SIZE;
    response.virt_addr = virt_addr;
    response.error_code = SYSTEM_ERR_SUCCESS;

    debug_printf("[SYSTEM_DECK] BUF_ALLOC: SUCCESS - handle=%lu phys=0x%lx virt=0x%lx size=%lu\n",
            handle, response.phys_addr, virt_addr, response.actual_size);
    deliver_response(event, SYSTEM_ERR_SUCCESS, &response, sizeof(response));
    return 0;
}

int system_deck_buf_free(Event* event) {
    if (!event) {
        debug_printf("[SYSTEM_DECK] BUF_FREE: NULL event\n");
        return -1;
    }

    buf_free_event_t free_event;
    memcpy(&free_event, event->data, sizeof(buf_free_event_t));

    debug_printf("[SYSTEM_DECK] BUF_FREE: handle=%lu\n", free_event.buffer_handle);

    if (free_event.buffer_handle == 0) {
        debug_printf("[SYSTEM_DECK] BUF_FREE: Invalid handle (zero)\n");
        deliver_response(event, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    spin_lock(&buffer_table_lock);

    int slot = find_buffer_by_handle(free_event.buffer_handle);
    if (slot < 0) {
        spin_unlock(&buffer_table_lock);
        debug_printf("[SYSTEM_DECK] BUF_FREE: Buffer not found\n");
        deliver_response(event, SYSTEM_ERR_BUFFER_NOT_FOUND, NULL, 0);
        return -1;
    }

    if (buffer_table[slot].owner_pid != event->pid) {
        spin_unlock(&buffer_table_lock);
        debug_printf("[SYSTEM_DECK] BUF_FREE: Permission denied (not owner)\n");
        deliver_response(event, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    void* phys_addr = (void*)buffer_table[slot].phys_addr;
    uint64_t virt_addr = buffer_table[slot].virt_addr;
    size_t pages = buffer_table[slot].size / PMM_PAGE_SIZE;
    uint32_t owner_pid = buffer_table[slot].owner_pid;

    // Clear entry before releasing lock
    buffer_table[slot].in_use = false;
    buffer_table[slot].handle = 0;
    buffer_table[slot].phys_addr = 0;
    buffer_table[slot].virt_addr = 0;
    buffer_table[slot].size = 0;
    buffer_table[slot].owner_pid = 0;

    spin_unlock(&buffer_table_lock);

    // Unmap from cabin if it was mapped
    if (virt_addr != 0) {
        process_t* proc = process_find(owner_pid);
        if (proc && proc->cabin) {
            vmm_unmap_pages(proc->cabin, virt_addr, pages);
        }
    }

    // Free physical memory outside the lock
    pmm_free(phys_addr, pages);

    __sync_synchronize();

    buf_free_response_t response;
    memset(&response, 0, sizeof(response));
    response.error_code = SYSTEM_ERR_SUCCESS;

    debug_printf("[SYSTEM_DECK] BUF_FREE: SUCCESS - freed handle=%lu\n", free_event.buffer_handle);
    deliver_response(event, SYSTEM_ERR_SUCCESS, &response, sizeof(response));
    return 0;
}

int system_deck_buf_resize(Event* event) {
    if (!event) {
        debug_printf("[SYSTEM_DECK] BUF_RESIZE: NULL event\n");
        return -1;
    }

    buf_resize_event_t resize_event;
    memcpy(&resize_event, event->data, sizeof(buf_resize_event_t));

    debug_printf("[SYSTEM_DECK] BUF_RESIZE: handle=%lu new_size=%lu\n",
            resize_event.buffer_handle, resize_event.new_size);

    if (resize_event.buffer_handle == 0) {
        debug_printf("[SYSTEM_DECK] BUF_RESIZE: Invalid handle (zero)\n");
        deliver_response(event, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    if (resize_event.new_size == 0) {
        debug_printf("[SYSTEM_DECK] BUF_RESIZE: Invalid new_size (zero)\n");
        deliver_response(event, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    if (resize_event.new_size > BUF_MAX_SIZE) {
        debug_printf("[SYSTEM_DECK] BUF_RESIZE: Size exceeds limit (%lu > %u)\n",
                resize_event.new_size, BUF_MAX_SIZE);
        deliver_response(event, SYSTEM_ERR_SIZE_LIMIT, NULL, 0);
        return -1;
    }

    spin_lock(&buffer_table_lock);

    int slot = find_buffer_by_handle(resize_event.buffer_handle);
    if (slot < 0) {
        spin_unlock(&buffer_table_lock);
        debug_printf("[SYSTEM_DECK] BUF_RESIZE: Buffer not found\n");
        deliver_response(event, SYSTEM_ERR_BUFFER_NOT_FOUND, NULL, 0);
        return -1;
    }

    if (buffer_table[slot].owner_pid != event->pid) {
        spin_unlock(&buffer_table_lock);
        debug_printf("[SYSTEM_DECK] BUF_RESIZE: Permission denied (not owner)\n");
        deliver_response(event, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    uint64_t old_phys = buffer_table[slot].phys_addr;
    uint64_t old_virt_addr = buffer_table[slot].virt_addr;
    uint64_t old_size = buffer_table[slot].size;
    size_t old_pages = old_size / PMM_PAGE_SIZE;

    size_t new_pages = (resize_event.new_size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64_t new_actual_size = new_pages * PMM_PAGE_SIZE;

    // If page count is the same, no reallocation needed
    if (new_pages == old_pages) {
        spin_unlock(&buffer_table_lock);

        buf_resize_response_t response;
        memset(&response, 0, sizeof(response));
        response.buffer_handle = resize_event.buffer_handle;
        response.actual_size = new_actual_size;
        response.error_code = SYSTEM_ERR_SUCCESS;

        deliver_response(event, SYSTEM_ERR_SUCCESS, &response, sizeof(response));
        return 0;
    }

    spin_unlock(&buffer_table_lock);

    // Allocate new buffer
    void* new_phys = pmm_alloc_zero(new_pages);
    if (!new_phys) {
        debug_printf("[SYSTEM_DECK] BUF_RESIZE: PMM allocation failed\n");
        deliver_response(event, SYSTEM_ERR_NO_MEMORY, NULL, 0);
        return -1;
    }

    // Copy data (min of old and new size)
    void* old_virt = vmm_phys_to_virt(old_phys);
    void* new_virt = vmm_phys_to_virt((uintptr_t)new_phys);
    size_t copy_size = old_size < new_actual_size ? old_size : new_actual_size;
    memcpy(new_virt, old_virt, copy_size);

    __sync_synchronize();

    // Remap in cabin: unmap old, map new at same virtual address
    process_t* proc = process_find(event->pid);
    uint64_t new_virt_addr = old_virt_addr;

    if (old_virt_addr != 0 && proc && proc->cabin) {
        vmm_unmap_pages(proc->cabin, old_virt_addr, old_pages);

        if (new_pages <= old_pages) {
            // Shrink: remap at same address
            vmm_map_pages(proc->cabin, old_virt_addr,
                    (uintptr_t)new_phys, new_pages, VMM_FLAGS_USER_RW);
        } else {
            // Grow: allocate new region from buf_heap
            new_virt_addr = proc->buf_heap_next;
            vmm_map_result_t map_result = vmm_map_pages(proc->cabin, new_virt_addr,
                    (uintptr_t)new_phys, new_pages, VMM_FLAGS_USER_RW);
            if (map_result.success) {
                proc->buf_heap_next += new_pages * PMM_PAGE_SIZE;
            } else {
                new_virt_addr = 0;
            }
        }
    }

    // Update table atomically
    spin_lock(&buffer_table_lock);

    // Re-verify slot is still valid
    if (!buffer_table[slot].in_use ||
        buffer_table[slot].handle != resize_event.buffer_handle) {
        spin_unlock(&buffer_table_lock);
        pmm_free(new_phys, new_pages);
        debug_printf("[SYSTEM_DECK] BUF_RESIZE: Buffer state changed during resize\n");
        deliver_response(event, SYSTEM_ERR_BUFFER_NOT_FOUND, NULL, 0);
        return -1;
    }

    buffer_table[slot].phys_addr = (uint64_t)new_phys;
    buffer_table[slot].virt_addr = new_virt_addr;
    buffer_table[slot].size = new_actual_size;

    spin_unlock(&buffer_table_lock);

    // Free old buffer
    pmm_free((void*)old_phys, old_pages);

    buf_resize_response_t response;
    memset(&response, 0, sizeof(response));
    response.buffer_handle = resize_event.buffer_handle;
    response.actual_size = new_actual_size;
    response.error_code = SYSTEM_ERR_SUCCESS;

    debug_printf("[SYSTEM_DECK] BUF_RESIZE: SUCCESS - handle=%lu old=%lu new=%lu\n",
            resize_event.buffer_handle, old_size, new_actual_size);
    deliver_response(event, SYSTEM_ERR_SUCCESS, &response, sizeof(response));
    return 0;
}
