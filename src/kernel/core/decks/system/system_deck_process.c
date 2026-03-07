#include "system_deck_process.h"
#include "klib.h"
#include "process.h"
#include "vmm.h"
#include "pmm.h"
#include "atomics.h"
#include "tagfs.h"

typedef struct {
    uint64_t handle;
    uint64_t phys_addr;
    uint64_t virt_addr;
    uint64_t size;
    uint32_t owner_pid;
    bool in_use;
} buffer_entry_t;

static buffer_entry_t buffer_table[BUF_MAX_COUNT];
static uint64_t next_buffer_handle = 1;
static spinlock_t buffer_table_lock = {0};

static void deliver_response(Pocket* pocket, uint16_t error_code,
                              const void* response_data, size_t response_size) {
    if (!pocket) {
        return;
    }

    pocket->error_code = error_code;

    if (!response_data || response_size == 0) {
        return;
    }

    process_t* proc = process_find(pocket->pid);
    if (!proc) {
        return;
    }

    void* dest = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
    if (!dest) {
        debug_printf("[SYSTEM_DECK] deliver_response: failed to translate data_addr 0x%lx\n",
                     pocket->data_addr);
        return;
    }

    size_t copy_size = response_size;
    if (copy_size > pocket->data_length) {
        copy_size = pocket->data_length;
    }

    memcpy(dest, response_data, copy_size);
    __sync_synchronize();
}

static void* get_request_data(Pocket* pocket) {
    if (!pocket || pocket->data_length == 0) {
        return NULL;
    }

    process_t* proc = process_find(pocket->pid);
    if (!proc) {
        return NULL;
    }

    return vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
}

int system_deck_proc_spawn(Pocket* pocket) {
    if (!pocket) {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: NULL pocket\n");
        return -1;
    }

    proc_spawn_event_t spawn_req;
    memset(&spawn_req, 0, sizeof(spawn_req));

    void* data = get_request_data(pocket);
    if (!data) {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: failed to read request data\n");
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    memcpy(&spawn_req, data, sizeof(proc_spawn_event_t));

    if (strnlen(spawn_req.tags, sizeof(spawn_req.tags)) == sizeof(spawn_req.tags)) {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: Tags not properly null-terminated\n");
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    spawn_req.tags[sizeof(spawn_req.tags) - 1] = '\0';

    if (spawn_req.tags[0] == '\0') {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: Empty tags not allowed\n");
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    debug_printf("[SYSTEM_DECK] PROC_SPAWN: binary_phys=0x%lx size=%lu tags='%s'\n",
            spawn_req.binary_phys_addr, spawn_req.binary_size, spawn_req.tags);

    if (spawn_req.binary_phys_addr == 0 || spawn_req.binary_size == 0) {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: Invalid binary address or size\n");
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    if (spawn_req.binary_phys_addr & 0xFFF) {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: Binary address not page-aligned (0x%lx)\n",
                spawn_req.binary_phys_addr);
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    if (spawn_req.binary_phys_addr + spawn_req.binary_size < spawn_req.binary_phys_addr) {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: Binary address range overflows\n");
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    if (spawn_req.binary_phys_addr > PROC_SPAWN_MAX_PHYS_ADDR) {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: Binary address too high (0x%lx)\n",
                spawn_req.binary_phys_addr);
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    #define MAX_SAFE_BINARY_SIZE (SIZE_MAX - 4096)
    if (spawn_req.binary_size > MAX_SAFE_BINARY_SIZE) {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: Binary size would cause overflow\n");
        deliver_response(pocket, SYSTEM_ERR_SIZE_LIMIT, NULL, 0);
        return -1;
    }

    if (spawn_req.binary_size > PROC_SPAWN_MAX_BINARY_SIZE) {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: Binary too large (%lu > %u bytes)\n",
                spawn_req.binary_size, PROC_SPAWN_MAX_BINARY_SIZE);
        deliver_response(pocket, SYSTEM_ERR_SIZE_LIMIT, NULL, 0);
        return -1;
    }

    if (process_get_count() >= PROCESS_MAX_COUNT) {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: Process limit reached\n");
        deliver_response(pocket, SYSTEM_ERR_PROCESS_LIMIT, NULL, 0);
        return -1;
    }

    process_t* proc = process_create(spawn_req.tags);
    if (!proc) {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: Failed to create process\n");
        deliver_response(pocket, SYSTEM_ERR_CABIN_FAILED, NULL, 0);
        return -1;
    }

    proc->spawner_pid = pocket->pid;

    void* binary_virt = vmm_phys_to_virt(spawn_req.binary_phys_addr);

    const uint8_t* elf_hdr = (const uint8_t*)binary_virt;
    if (spawn_req.binary_size < 16 ||
        elf_hdr[0] != 0x7F || elf_hdr[1] != 'E' ||
        elf_hdr[2] != 'L' || elf_hdr[3] != 'F') {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: Invalid ELF magic\n");
        process_destroy(proc);
        deliver_response(pocket, SYSTEM_ERR_LOAD_FAILED, NULL, 0);
        return -1;
    }

    int load_result = process_load_binary(proc, binary_virt, spawn_req.binary_size);
    if (load_result != 0) {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: Failed to load binary\n");
        process_destroy(proc);
        deliver_response(pocket, SYSTEM_ERR_LOAD_FAILED, NULL, 0);
        return -1;
    }

    proc_spawn_response_t response;
    memset(&response, 0, sizeof(response));
    response.new_pid = proc->pid;

    debug_printf("[SYSTEM_DECK] PROC_SPAWN: SUCCESS - created PID %u\n", proc->pid);
    deliver_response(pocket, SYSTEM_ERR_SUCCESS, &response, sizeof(response));
    return 0;
}

int system_deck_proc_kill(Pocket* pocket) {
    if (!pocket) {
        debug_printf("[SYSTEM_DECK] PROC_KILL: NULL pocket\n");
        return -1;
    }

    proc_kill_event_t kill_req;
    memset(&kill_req, 0, sizeof(kill_req));

    void* data = get_request_data(pocket);
    if (!data) {
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }
    memcpy(&kill_req, data, sizeof(proc_kill_event_t));

    uint32_t target_pid = kill_req.target_pid;
    bool is_self_exit = (target_pid == 0);

    if (is_self_exit) {
        target_pid = pocket->pid;
        uint32_t exit_code = kill_req.target_pid;
        debug_printf("[SYSTEM_DECK] PROC_KILL: Process %u exiting with code %u\n",
                     target_pid, exit_code);
    } else {
        debug_printf("[SYSTEM_DECK] PROC_KILL: Killing target_pid=%u\n", target_pid);
    }

    if (target_pid == PROCESS_INVALID_PID) {
        debug_printf("[SYSTEM_DECK] PROC_KILL: Invalid PID\n");
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    process_t* proc = process_find(target_pid);
    if (!proc) {
        debug_printf("[SYSTEM_DECK] PROC_KILL: Process %u not found\n", target_pid);
        deliver_response(pocket, SYSTEM_ERR_PROCESS_NOT_FOUND, NULL, 0);
        return -1;
    }

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
    deliver_response(pocket, SYSTEM_ERR_SUCCESS, &response, sizeof(response));
    return 0;
}

int system_deck_proc_info(Pocket* pocket) {
    if (!pocket) {
        debug_printf("[SYSTEM_DECK] PROC_INFO: NULL pocket\n");
        return -1;
    }

    proc_info_event_t info_req;
    memset(&info_req, 0, sizeof(info_req));

    void* data = get_request_data(pocket);
    if (!data) {
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }
    memcpy(&info_req, data, sizeof(proc_info_event_t));

    uint32_t target_pid = info_req.target_pid;

    if (target_pid == 0) {
        target_pid = pocket->pid;
        debug_printf("[SYSTEM_DECK] PROC_INFO: Query self (PID %u)\n", target_pid);
    } else {
        debug_printf("[SYSTEM_DECK] PROC_INFO: Query PID %u\n", target_pid);
    }

    process_t* proc = process_find(target_pid);
    if (!proc) {
        debug_printf("[SYSTEM_DECK] PROC_INFO: Process %u not found\n", target_pid);
        deliver_response(pocket, SYSTEM_ERR_PROCESS_NOT_FOUND, NULL, 0);
        return -1;
    }

    proc_info_response_t response;
    memset(&response, 0, sizeof(response));

    response.pid        = proc->pid;
    response.state      = (uint32_t)proc->state;
    response.score      = proc->score;
    response.code_start = proc->code_start;
    response.code_size  = proc->code_size;

    strncpy(response.tags, proc->tags, PROC_INFO_TAGS_SIZE);
    response.tags[PROC_INFO_TAGS_SIZE - 1] = '\0';

    debug_printf("[SYSTEM_DECK] PROC_INFO: SUCCESS - returned info for PID %u\n", proc->pid);
    deliver_response(pocket, SYSTEM_ERR_SUCCESS, &response, sizeof(response));
    return 0;
}

int system_deck_proc_exec(Pocket* pocket) {
    if (!pocket) {
        debug_printf("[SYSTEM_DECK] PROC_EXEC: NULL pocket\n");
        return -1;
    }

    proc_exec_event_t exec_req;
    memset(&exec_req, 0, sizeof(exec_req));

    void* data = get_request_data(pocket);
    if (!data) {
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }
    memcpy(&exec_req, data, sizeof(proc_exec_event_t));

    if (strnlen(exec_req.filename, sizeof(exec_req.filename))
            == sizeof(exec_req.filename)) {
        debug_printf("[SYSTEM_DECK] PROC_EXEC: filename not null-terminated\n");
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    exec_req.filename[sizeof(exec_req.filename) - 1] = '\0';

    if (exec_req.filename[0] == '\0') {
        debug_printf("[SYSTEM_DECK] PROC_EXEC: empty filename\n");
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    debug_printf("[SYSTEM_DECK] PROC_EXEC: looking for '%s'\n", exec_req.filename);

    if (process_get_count() >= PROCESS_MAX_COUNT) {
        debug_printf("[SYSTEM_DECK] PROC_EXEC: process limit reached\n");
        deliver_response(pocket, SYSTEM_ERR_PROCESS_LIMIT, NULL, 0);
        return -1;
    }

    #define PROC_EXEC_MAX_SCAN 256
    uint32_t* file_ids = kmalloc(PROC_EXEC_MAX_SCAN * sizeof(uint32_t));
    if (!file_ids) {
        debug_printf("[SYSTEM_DECK] PROC_EXEC: kmalloc for file scan failed\n");
        deliver_response(pocket, SYSTEM_ERR_NO_MEMORY, NULL, 0);
        return -1;
    }
    int file_count = tagfs_list_all_files(file_ids, PROC_EXEC_MAX_SCAN);

    uint32_t found_id = 0;
    char found_tags[PROCESS_TAG_SIZE];
    found_tags[0] = '\0';

    for (int i = 0; i < file_count; i++) {
        TagFSMetadata* meta = tagfs_get_metadata(file_ids[i]);
        if (!meta || !(meta->flags & TAGFS_FILE_ACTIVE)) {
            continue;
        }

        bool has_name     = false;
        bool has_exec_tag = false;

        for (uint8_t t = 0; t < meta->tag_count; t++) {
            if (meta->tags[t].type != TAGFS_TAG_SYSTEM) {
                continue;
            }
            const char* key = meta->tags[t].key;
            if (strcmp(key, exec_req.filename) == 0) {
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
                    break;
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

    kfree(file_ids);
    file_ids = NULL;

    if (found_id == 0) {
        debug_printf("[SYSTEM_DECK] PROC_EXEC: '%s' not found as executable\n",
                exec_req.filename);
        deliver_response(pocket, SYSTEM_ERR_EXEC_NOT_FOUND, NULL, 0);
        return -1;
    }

    TagFSMetadata* meta = tagfs_get_metadata(found_id);
    uint64_t file_size = meta->size;

    if (file_size == 0 || file_size > PROC_SPAWN_MAX_BINARY_SIZE) {
        debug_printf("[SYSTEM_DECK] PROC_EXEC: invalid file size %lu\n", file_size);
        deliver_response(pocket, SYSTEM_ERR_SIZE_LIMIT, NULL, 0);
        return -1;
    }

    size_t pages_needed = (file_size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    void* phys_buf = pmm_alloc_zero(pages_needed);
    if (!phys_buf) {
        debug_printf("[SYSTEM_DECK] PROC_EXEC: pmm_alloc_zero failed\n");
        deliver_response(pocket, SYSTEM_ERR_NO_MEMORY, NULL, 0);
        return -1;
    }

    void* virt_buf = vmm_phys_to_virt((uintptr_t)phys_buf);

    TagFSFileHandle* fh = tagfs_open(found_id, TAGFS_HANDLE_READ);
    if (!fh) {
        debug_printf("[SYSTEM_DECK] PROC_EXEC: tagfs_open failed\n");
        pmm_free(phys_buf, pages_needed);
        deliver_response(pocket, SYSTEM_ERR_LOAD_FAILED, NULL, 0);
        return -1;
    }

    int read_result = tagfs_read(fh, virt_buf, file_size);
    tagfs_close(fh);

    if (read_result < 0) {
        debug_printf("[SYSTEM_DECK] PROC_EXEC: tagfs_read failed (%d)\n", read_result);
        pmm_free(phys_buf, pages_needed);
        deliver_response(pocket, SYSTEM_ERR_LOAD_FAILED, NULL, 0);
        return -1;
    }

    process_t* proc = process_create(found_tags);
    if (!proc) {
        debug_printf("[SYSTEM_DECK] PROC_EXEC: process_create failed\n");
        pmm_free(phys_buf, pages_needed);
        deliver_response(pocket, SYSTEM_ERR_CABIN_FAILED, NULL, 0);
        return -1;
    }

    // Set spawner_pid BEFORE load_binary — load_binary writes CabinInfo page
    // which userspace reads to discover its spawner. If set after, spawner_pid=0.
    proc->spawner_pid = pocket->pid;

    int load_result = process_load_binary(proc, virt_buf, (size_t)file_size);
    pmm_free(phys_buf, pages_needed);

    if (load_result != 0) {
        debug_printf("[SYSTEM_DECK] PROC_EXEC: process_load_binary failed\n");
        process_destroy(proc);
        deliver_response(pocket, SYSTEM_ERR_LOAD_FAILED, NULL, 0);
        return -1;
    }

    __sync_synchronize();

    process_set_state(proc, PROC_WORKING);

    proc_exec_response_t response;
    memset(&response, 0, sizeof(response));
    response.new_pid = proc->pid;

    debug_printf("[SYSTEM_DECK] PROC_EXEC: SUCCESS - '%s' -> PID %u\n",
            exec_req.filename, proc->pid);
    deliver_response(pocket, SYSTEM_ERR_SUCCESS, &response, sizeof(response));
    return 0;
}

int system_deck_tag_add(Pocket* pocket) {
    if (!pocket) {
        debug_printf("[SYSTEM_DECK] TAG_ADD: NULL pocket\n");
        return -1;
    }

    tag_modify_event_t tag_req;
    memset(&tag_req, 0, sizeof(tag_req));

    void* data = get_request_data(pocket);
    if (!data) {
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }
    memcpy(&tag_req, data, sizeof(tag_modify_event_t));

    if (strnlen(tag_req.tag, sizeof(tag_req.tag)) == sizeof(tag_req.tag)) {
        debug_printf("[SYSTEM_DECK] TAG_ADD: Tag not properly null-terminated\n");
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    tag_req.tag[sizeof(tag_req.tag) - 1] = '\0';

    if (tag_req.tag[0] == '\0') {
        debug_printf("[SYSTEM_DECK] TAG_ADD: Empty tag not allowed\n");
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    debug_printf("[SYSTEM_DECK] TAG_ADD: target_pid=%u tag='%s'\n",
            tag_req.target_pid, tag_req.tag);

    if (tag_req.target_pid == PROCESS_INVALID_PID) {
        debug_printf("[SYSTEM_DECK] TAG_ADD: Invalid PID\n");
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    process_t* proc = process_find(tag_req.target_pid);
    if (!proc) {
        debug_printf("[SYSTEM_DECK] TAG_ADD: Process %u not found\n", tag_req.target_pid);
        deliver_response(pocket, SYSTEM_ERR_PROCESS_NOT_FOUND, NULL, 0);
        return -1;
    }

    if (process_has_tag(proc, tag_req.tag)) {
        debug_printf("[SYSTEM_DECK] TAG_ADD: Tag already exists\n");
        tag_modify_response_t response;
        memset(&response, 0, sizeof(response));
        response.error_code = SYSTEM_ERR_TAG_EXISTS;
        strncpy(response.message, "Tag already exists", sizeof(response.message) - 1);
        deliver_response(pocket, SYSTEM_ERR_TAG_EXISTS, &response, sizeof(response));
        return -1;
    }

    int result = process_add_tag(proc, tag_req.tag);
    if (result != 0) {
        debug_printf("[SYSTEM_DECK] TAG_ADD: Failed to add tag (tags full)\n");
        tag_modify_response_t response;
        memset(&response, 0, sizeof(response));
        response.error_code = SYSTEM_ERR_TAG_FULL;
        strncpy(response.message, "Tag buffer full", sizeof(response.message) - 1);
        deliver_response(pocket, SYSTEM_ERR_TAG_FULL, &response, sizeof(response));
        return -1;
    }

    if (strcmp(tag_req.tag, "stopped") == 0) {
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
            tag_req.tag, tag_req.target_pid);
    deliver_response(pocket, SYSTEM_ERR_SUCCESS, &response, sizeof(response));
    return 0;
}

int system_deck_tag_remove(Pocket* pocket) {
    if (!pocket) {
        debug_printf("[SYSTEM_DECK] TAG_REMOVE: NULL pocket\n");
        return -1;
    }

    tag_modify_event_t tag_req;
    memset(&tag_req, 0, sizeof(tag_req));

    void* data = get_request_data(pocket);
    if (!data) {
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }
    memcpy(&tag_req, data, sizeof(tag_modify_event_t));

    if (strnlen(tag_req.tag, sizeof(tag_req.tag)) == sizeof(tag_req.tag)) {
        debug_printf("[SYSTEM_DECK] TAG_REMOVE: Tag not properly null-terminated\n");
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    tag_req.tag[sizeof(tag_req.tag) - 1] = '\0';

    if (tag_req.tag[0] == '\0') {
        debug_printf("[SYSTEM_DECK] TAG_REMOVE: Empty tag not allowed\n");
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    debug_printf("[SYSTEM_DECK] TAG_REMOVE: target_pid=%u tag='%s'\n",
            tag_req.target_pid, tag_req.tag);

    if (tag_req.target_pid == PROCESS_INVALID_PID) {
        debug_printf("[SYSTEM_DECK] TAG_REMOVE: Invalid PID\n");
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    process_t* proc = process_find(tag_req.target_pid);
    if (!proc) {
        debug_printf("[SYSTEM_DECK] TAG_REMOVE: Process %u not found\n", tag_req.target_pid);
        deliver_response(pocket, SYSTEM_ERR_PROCESS_NOT_FOUND, NULL, 0);
        return -1;
    }

    if (!process_has_tag(proc, tag_req.tag)) {
        debug_printf("[SYSTEM_DECK] TAG_REMOVE: Tag not found\n");
        tag_modify_response_t response;
        memset(&response, 0, sizeof(response));
        response.error_code = SYSTEM_ERR_TAG_NOT_FOUND;
        strncpy(response.message, "Tag not found", sizeof(response.message) - 1);
        deliver_response(pocket, SYSTEM_ERR_TAG_NOT_FOUND, &response, sizeof(response));
        return -1;
    }

    int result = process_remove_tag(proc, tag_req.tag);
    if (result != 0) {
        debug_printf("[SYSTEM_DECK] TAG_REMOVE: Failed to remove tag\n");
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    if (strcmp(tag_req.tag, "stopped") == 0) {
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
            tag_req.tag, tag_req.target_pid);
    deliver_response(pocket, SYSTEM_ERR_SUCCESS, &response, sizeof(response));
    return 0;
}

int system_deck_tag_check(Pocket* pocket) {
    if (!pocket) {
        debug_printf("[SYSTEM_DECK] TAG_CHECK: NULL pocket\n");
        return -1;
    }

    tag_check_event_t tag_req;
    memset(&tag_req, 0, sizeof(tag_req));

    void* data = get_request_data(pocket);
    if (!data) {
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }
    memcpy(&tag_req, data, sizeof(tag_check_event_t));

    if (strnlen(tag_req.tag, sizeof(tag_req.tag)) == sizeof(tag_req.tag)) {
        debug_printf("[SYSTEM_DECK] TAG_CHECK: Tag not properly null-terminated\n");
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    tag_req.tag[sizeof(tag_req.tag) - 1] = '\0';

    if (tag_req.tag[0] == '\0') {
        debug_printf("[SYSTEM_DECK] TAG_CHECK: Empty tag not allowed\n");
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    debug_printf("[SYSTEM_DECK] TAG_CHECK: target_pid=%u tag='%s'\n",
            tag_req.target_pid, tag_req.tag);

    if (tag_req.target_pid == PROCESS_INVALID_PID) {
        debug_printf("[SYSTEM_DECK] TAG_CHECK: Invalid PID\n");
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    process_t* proc = process_find(tag_req.target_pid);
    if (!proc) {
        debug_printf("[SYSTEM_DECK] TAG_CHECK: Process %u not found\n", tag_req.target_pid);
        deliver_response(pocket, SYSTEM_ERR_PROCESS_NOT_FOUND, NULL, 0);
        return -1;
    }

    tag_check_response_t response;
    memset(&response, 0, sizeof(response));
    response.has_tag    = process_has_tag(proc, tag_req.tag);
    response.error_code = SYSTEM_ERR_SUCCESS;

    debug_printf("[SYSTEM_DECK] TAG_CHECK: SUCCESS - PID %u %s tag '%s'\n",
            tag_req.target_pid, response.has_tag ? "has" : "does not have", tag_req.tag);
    deliver_response(pocket, SYSTEM_ERR_SUCCESS, &response, sizeof(response));
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
            void* phys_addr  = (void*)buffer_table[i].phys_addr;
            uint64_t virt_addr = buffer_table[i].virt_addr;
            size_t pages     = buffer_table[i].size / PMM_PAGE_SIZE;

            debug_printf("[SYSTEM_DECK] Cleaning up buffer handle=%lu for PID %u\n",
                    buffer_table[i].handle, pid);

            if (virt_addr != 0 && proc && proc->cabin) {
                vmm_unmap_pages(proc->cabin, virt_addr, pages);
            }

            pmm_free(phys_addr, pages);

            __sync_synchronize();

            buffer_table[i].in_use    = false;
            buffer_table[i].handle    = 0;
            buffer_table[i].phys_addr = 0;
            buffer_table[i].virt_addr = 0;
            buffer_table[i].size      = 0;
            buffer_table[i].owner_pid = 0;
        }
    }

    spin_unlock(&buffer_table_lock);
}

static uint64_t generate_random_handle(void) {
    uint64_t tsc     = rdtsc();
    uint64_t counter = atomic_fetch_add_u64(&next_buffer_handle, 1);
    uint64_t result  = (tsc ^ (counter << 32) ^ (counter >> 32));
    return result ? result : 1;
}

int system_deck_buf_alloc(Pocket* pocket) {
    if (!pocket) {
        debug_printf("[SYSTEM_DECK] BUF_ALLOC: NULL pocket\n");
        return -1;
    }

    buf_alloc_event_t alloc_req;
    memset(&alloc_req, 0, sizeof(alloc_req));

    void* data = get_request_data(pocket);
    if (!data) {
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }
    memcpy(&alloc_req, data, sizeof(buf_alloc_event_t));

    debug_printf("[SYSTEM_DECK] BUF_ALLOC: size=%lu flags=0x%x\n",
            alloc_req.size, alloc_req.flags);

    if (alloc_req.size == 0) {
        debug_printf("[SYSTEM_DECK] BUF_ALLOC: Invalid size (zero)\n");
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    if (alloc_req.size > BUF_MAX_SIZE) {
        debug_printf("[SYSTEM_DECK] BUF_ALLOC: Size exceeds limit (%lu > %u)\n",
                alloc_req.size, BUF_MAX_SIZE);
        deliver_response(pocket, SYSTEM_ERR_SIZE_LIMIT, NULL, 0);
        return -1;
    }

    if (alloc_req.size > UINT64_MAX - PMM_PAGE_SIZE) {
        debug_printf("[SYSTEM_DECK] BUF_ALLOC: Size would cause overflow in page calculation\n");
        deliver_response(pocket, SYSTEM_ERR_SIZE_LIMIT, NULL, 0);
        return -1;
    }

    spin_lock(&buffer_table_lock);

    if (count_process_buffers(pocket->pid) >= BUF_MAX_COUNT / 4) {
        spin_unlock(&buffer_table_lock);
        debug_printf("[SYSTEM_DECK] BUF_ALLOC: Process buffer limit reached\n");
        deliver_response(pocket, SYSTEM_ERR_BUFFER_LIMIT, NULL, 0);
        return -1;
    }

    int slot = find_free_buffer_slot();
    if (slot < 0) {
        spin_unlock(&buffer_table_lock);
        debug_printf("[SYSTEM_DECK] BUF_ALLOC: Global buffer table full\n");
        deliver_response(pocket, SYSTEM_ERR_BUFFER_LIMIT, NULL, 0);
        return -1;
    }

    uint64_t handle = generate_random_handle();

    int collision_count = 0;
    while (find_buffer_by_handle(handle) >= 0) {
        handle = generate_random_handle();
        collision_count++;
        if (collision_count > BUF_MAX_COUNT) {
            spin_unlock(&buffer_table_lock);
            debug_printf("[SYSTEM_DECK] BUF_ALLOC: Handle generation failed\n");
            deliver_response(pocket, SYSTEM_ERR_BUFFER_LIMIT, NULL, 0);
            return -1;
        }
    }

    spin_unlock(&buffer_table_lock);

    size_t pages_needed = (alloc_req.size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    void* phys_addr = pmm_alloc_zero(pages_needed);

    __sync_synchronize();

    if (!phys_addr) {
        debug_printf("[SYSTEM_DECK] BUF_ALLOC: PMM allocation failed\n");
        deliver_response(pocket, SYSTEM_ERR_NO_MEMORY, NULL, 0);
        return -1;
    }

    spin_lock(&buffer_table_lock);

    if (buffer_table[slot].in_use) {
        spin_unlock(&buffer_table_lock);
        pmm_free(phys_addr, pages_needed);
        debug_printf("[SYSTEM_DECK] BUF_ALLOC: Slot became occupied during allocation\n");
        deliver_response(pocket, SYSTEM_ERR_BUFFER_LIMIT, NULL, 0);
        return -1;
    }

    buffer_table[slot].handle    = handle;
    buffer_table[slot].phys_addr = (uint64_t)phys_addr;
    buffer_table[slot].size      = pages_needed * PMM_PAGE_SIZE;
    buffer_table[slot].owner_pid = pocket->pid;
    buffer_table[slot].virt_addr = 0;
    buffer_table[slot].in_use    = true;

    spin_unlock(&buffer_table_lock);

    uint64_t virt_addr = 0;
    process_t* proc = process_find(pocket->pid);
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
    response.phys_addr     = (uint64_t)phys_addr;
    response.actual_size   = pages_needed * PMM_PAGE_SIZE;
    response.virt_addr     = virt_addr;
    response.error_code    = SYSTEM_ERR_SUCCESS;

    debug_printf("[SYSTEM_DECK] BUF_ALLOC: SUCCESS - handle=%lu phys=0x%lx virt=0x%lx size=%lu\n",
            handle, response.phys_addr, virt_addr, response.actual_size);
    deliver_response(pocket, SYSTEM_ERR_SUCCESS, &response, sizeof(response));
    return 0;
}

int system_deck_buf_free(Pocket* pocket) {
    if (!pocket) {
        debug_printf("[SYSTEM_DECK] BUF_FREE: NULL pocket\n");
        return -1;
    }

    buf_free_event_t free_req;
    memset(&free_req, 0, sizeof(free_req));

    void* data = get_request_data(pocket);
    if (!data) {
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }
    memcpy(&free_req, data, sizeof(buf_free_event_t));

    debug_printf("[SYSTEM_DECK] BUF_FREE: handle=%lu\n", free_req.buffer_handle);

    if (free_req.buffer_handle == 0) {
        debug_printf("[SYSTEM_DECK] BUF_FREE: Invalid handle (zero)\n");
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    spin_lock(&buffer_table_lock);

    int slot = find_buffer_by_handle(free_req.buffer_handle);
    if (slot < 0) {
        spin_unlock(&buffer_table_lock);
        debug_printf("[SYSTEM_DECK] BUF_FREE: Buffer not found\n");
        deliver_response(pocket, SYSTEM_ERR_BUFFER_NOT_FOUND, NULL, 0);
        return -1;
    }

    if (buffer_table[slot].owner_pid != pocket->pid) {
        spin_unlock(&buffer_table_lock);
        debug_printf("[SYSTEM_DECK] BUF_FREE: Permission denied (not owner)\n");
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    void* phys_addr    = (void*)buffer_table[slot].phys_addr;
    uint64_t virt_addr = buffer_table[slot].virt_addr;
    size_t pages       = buffer_table[slot].size / PMM_PAGE_SIZE;
    uint32_t owner_pid = buffer_table[slot].owner_pid;

    buffer_table[slot].in_use    = false;
    buffer_table[slot].handle    = 0;
    buffer_table[slot].phys_addr = 0;
    buffer_table[slot].virt_addr = 0;
    buffer_table[slot].size      = 0;
    buffer_table[slot].owner_pid = 0;

    spin_unlock(&buffer_table_lock);

    if (virt_addr != 0) {
        process_t* proc = process_find(owner_pid);
        if (proc && proc->cabin) {
            vmm_unmap_pages(proc->cabin, virt_addr, pages);
        }
    }

    pmm_free(phys_addr, pages);

    __sync_synchronize();

    buf_free_response_t response;
    memset(&response, 0, sizeof(response));
    response.error_code = SYSTEM_ERR_SUCCESS;

    debug_printf("[SYSTEM_DECK] BUF_FREE: SUCCESS - freed handle=%lu\n", free_req.buffer_handle);
    deliver_response(pocket, SYSTEM_ERR_SUCCESS, &response, sizeof(response));
    return 0;
}

int system_deck_buf_resize(Pocket* pocket) {
    if (!pocket) {
        debug_printf("[SYSTEM_DECK] BUF_RESIZE: NULL pocket\n");
        return -1;
    }

    buf_resize_event_t resize_req;
    memset(&resize_req, 0, sizeof(resize_req));

    void* data = get_request_data(pocket);
    if (!data) {
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }
    memcpy(&resize_req, data, sizeof(buf_resize_event_t));

    debug_printf("[SYSTEM_DECK] BUF_RESIZE: handle=%lu new_size=%lu\n",
            resize_req.buffer_handle, resize_req.new_size);

    if (resize_req.buffer_handle == 0) {
        debug_printf("[SYSTEM_DECK] BUF_RESIZE: Invalid handle (zero)\n");
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    if (resize_req.new_size == 0) {
        debug_printf("[SYSTEM_DECK] BUF_RESIZE: Invalid new_size (zero)\n");
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    if (resize_req.new_size > BUF_MAX_SIZE) {
        debug_printf("[SYSTEM_DECK] BUF_RESIZE: Size exceeds limit (%lu > %u)\n",
                resize_req.new_size, BUF_MAX_SIZE);
        deliver_response(pocket, SYSTEM_ERR_SIZE_LIMIT, NULL, 0);
        return -1;
    }

    spin_lock(&buffer_table_lock);

    int slot = find_buffer_by_handle(resize_req.buffer_handle);
    if (slot < 0) {
        spin_unlock(&buffer_table_lock);
        debug_printf("[SYSTEM_DECK] BUF_RESIZE: Buffer not found\n");
        deliver_response(pocket, SYSTEM_ERR_BUFFER_NOT_FOUND, NULL, 0);
        return -1;
    }

    if (buffer_table[slot].owner_pid != pocket->pid) {
        spin_unlock(&buffer_table_lock);
        debug_printf("[SYSTEM_DECK] BUF_RESIZE: Permission denied (not owner)\n");
        deliver_response(pocket, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
        return -1;
    }

    uint64_t old_phys      = buffer_table[slot].phys_addr;
    uint64_t old_virt_addr = buffer_table[slot].virt_addr;
    uint64_t old_size      = buffer_table[slot].size;
    size_t old_pages       = old_size / PMM_PAGE_SIZE;

    size_t new_pages      = (resize_req.new_size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64_t new_actual_size = new_pages * PMM_PAGE_SIZE;

    if (new_pages == old_pages) {
        spin_unlock(&buffer_table_lock);

        buf_resize_response_t response;
        memset(&response, 0, sizeof(response));
        response.buffer_handle = resize_req.buffer_handle;
        response.actual_size   = new_actual_size;
        response.error_code    = SYSTEM_ERR_SUCCESS;

        deliver_response(pocket, SYSTEM_ERR_SUCCESS, &response, sizeof(response));
        return 0;
    }

    spin_unlock(&buffer_table_lock);

    void* new_phys = pmm_alloc_zero(new_pages);
    if (!new_phys) {
        debug_printf("[SYSTEM_DECK] BUF_RESIZE: PMM allocation failed\n");
        deliver_response(pocket, SYSTEM_ERR_NO_MEMORY, NULL, 0);
        return -1;
    }

    void* old_virt   = vmm_phys_to_virt(old_phys);
    void* new_virt   = vmm_phys_to_virt((uintptr_t)new_phys);
    size_t copy_size = old_size < new_actual_size ? old_size : new_actual_size;
    memcpy(new_virt, old_virt, copy_size);

    __sync_synchronize();

    process_t* proc = process_find(pocket->pid);
    uint64_t new_virt_addr = old_virt_addr;

    if (old_virt_addr != 0 && proc && proc->cabin) {
        vmm_unmap_pages(proc->cabin, old_virt_addr, old_pages);

        if (new_pages <= old_pages) {
            vmm_map_pages(proc->cabin, old_virt_addr,
                    (uintptr_t)new_phys, new_pages, VMM_FLAGS_USER_RW);
        } else {
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

    spin_lock(&buffer_table_lock);

    if (!buffer_table[slot].in_use ||
        buffer_table[slot].handle != resize_req.buffer_handle) {
        spin_unlock(&buffer_table_lock);
        pmm_free(new_phys, new_pages);
        debug_printf("[SYSTEM_DECK] BUF_RESIZE: Buffer state changed during resize\n");
        deliver_response(pocket, SYSTEM_ERR_BUFFER_NOT_FOUND, NULL, 0);
        return -1;
    }

    buffer_table[slot].phys_addr = (uint64_t)new_phys;
    buffer_table[slot].virt_addr = new_virt_addr;
    buffer_table[slot].size      = new_actual_size;

    spin_unlock(&buffer_table_lock);

    pmm_free((void*)old_phys, old_pages);

    buf_resize_response_t response;
    memset(&response, 0, sizeof(response));
    response.buffer_handle = resize_req.buffer_handle;
    response.actual_size   = new_actual_size;
    response.error_code    = SYSTEM_ERR_SUCCESS;

    debug_printf("[SYSTEM_DECK] BUF_RESIZE: SUCCESS - handle=%lu old=%lu new=%lu\n",
            resize_req.buffer_handle, old_size, new_actual_size);
    deliver_response(pocket, SYSTEM_ERR_SUCCESS, &response, sizeof(response));
    return 0;
}
