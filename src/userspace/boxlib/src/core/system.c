// system — unified userspace system API
// Process management, buffers, tags, power control, filesystem operations

#include "box/system.h"
#include "box/notify.h"
#include "box/chain.h"
#include "box/result.h"
#include "box/string.h"
#include "box/error.h"

// --- Process management ---

int proc_info(uint16_t pid, proc_info_t* info) {
    if (!info) {
        return BOX_ERR_INVALID_ARGS;
    }

    proc_query(pid);
    event_id_t event_id = notify();
    if (event_id == 0) {
        return BOX_ERR_EVENT_FAILED;
    }

    result_entry_t result;
    if (!result_wait(&result, 5000)) {
        return BOX_ERR_TIMEOUT;
    }

    if (result.error_code != BOX_OK) {
        return result.error_code;
    }

    if (result.size < 8) {
        return BOX_ERR_RESULT_INVALID;
    }

    memcpy(&info->pid, result.payload, 2);
    memcpy(&info->state, result.payload + 2, 1);
    memcpy(&info->priority, result.payload + 3, 1);
    memcpy(&info->memory_usage, result.payload + 4, 4);

    return BOX_OK;
}

void exit(uint32_t exit_code) {
    (void)exit_code;

    proc_kill(0);
    notify();

    while (1) {
        __asm__ volatile("pause");
    }
}

int proc_exec(const char* filename) {
    if (!filename || filename[0] == '\0') {
        return -1;
    }

    size_t name_len = strlen(filename);
    if (name_len >= 32) {
        return -1;
    }

    proc_spawn(filename);
    event_id_t event_id = notify();
    if (event_id == 0) {
        return -1;
    }

    result_entry_t result;
    if (!result_wait(&result, 5000)) {
        return -1;
    }

    if (result.error_code != BOX_OK) {
        return -1;
    }

    if (result.size < 4) {
        return -1;
    }

    uint32_t new_pid = 0;
    memcpy(&new_pid, result.payload, 4);
    return (int)new_pid;
}

// --- Buffer management ---

int buffer_alloc(uint8_t size_class, uint16_t* out_buffer_id, uint32_t* out_address) {
    if (!out_buffer_id || !out_address) {
        return BOX_ERR_INVALID_ARGS;
    }

    if (size_class > BOX_BUFFER_SIZE_4K) {
        return BOX_ERR_INVALID_ARGS;
    }

    buf_alloc(size_class);
    event_id_t event_id = notify();
    if (event_id == 0) {
        return BOX_ERR_EVENT_FAILED;
    }

    result_entry_t result;
    if (!result_wait(&result, 5000)) {
        return BOX_ERR_TIMEOUT;
    }

    if (result.error_code != BOX_OK) {
        return result.error_code;
    }

    if (result.size < 6) {
        return BOX_ERR_RESULT_INVALID;
    }

    uint16_t buffer_id;
    uint32_t address;
    memcpy(&buffer_id, result.payload, 2);
    memcpy(&address, result.payload + 2, 4);

    *out_buffer_id = buffer_id;
    *out_address = address;

    return BOX_OK;
}

int buffer_free(uint16_t buffer_id) {
    buf_release(buffer_id);
    event_id_t event_id = notify();
    if (event_id == 0) {
        return BOX_ERR_EVENT_FAILED;
    }

    result_entry_t result;
    if (!result_wait(&result, 5000)) {
        return BOX_ERR_TIMEOUT;
    }

    if (result.error_code != BOX_OK) {
        return result.error_code;
    }

    return BOX_OK;
}

// --- Process tags ---

int proc_tag_add(const char* tag) {
    if (!tag) {
        return BOX_ERR_INVALID_ARGS;
    }

    size_t tag_len = strlen(tag);
    if (tag_len == 0 || tag_len >= 32) {
        return BOX_ERR_INVALID_ARGS;
    }

    proc_info_t info;
    int rc = proc_info(0, &info);
    if (rc != BOX_OK) {
        return rc;
    }

    ptag_add(info.pid, tag);
    event_id_t event_id = notify();
    if (event_id == 0) {
        return BOX_ERR_EVENT_FAILED;
    }

    result_entry_t res;
    if (!result_wait(&res, 5000)) {
        return BOX_ERR_TIMEOUT;
    }

    if (res.error_code != BOX_OK) {
        return res.error_code;
    }

    return BOX_OK;
}

int proc_tag_remove(const char* tag) {
    if (!tag) {
        return BOX_ERR_INVALID_ARGS;
    }

    size_t tag_len = strlen(tag);
    if (tag_len == 0 || tag_len >= 32) {
        return BOX_ERR_INVALID_ARGS;
    }

    proc_info_t info;
    int rc = proc_info(0, &info);
    if (rc != BOX_OK) {
        return rc;
    }

    ptag_remove(info.pid, tag);
    event_id_t event_id = notify();
    if (event_id == 0) {
        return BOX_ERR_EVENT_FAILED;
    }

    result_entry_t res;
    if (!result_wait(&res, 5000)) {
        return BOX_ERR_TIMEOUT;
    }

    if (res.error_code != BOX_OK) {
        return res.error_code;
    }

    return BOX_OK;
}

int proc_tag_check(const char* tag, bool* has_tag) {
    if (!tag || !has_tag) {
        return BOX_ERR_INVALID_ARGS;
    }

    size_t tag_len = strlen(tag);
    if (tag_len == 0 || tag_len >= 32) {
        return BOX_ERR_INVALID_ARGS;
    }

    proc_info_t info;
    int rc = proc_info(0, &info);
    if (rc != BOX_OK) {
        return rc;
    }

    ptag_check(info.pid, tag);
    event_id_t event_id = notify();
    if (event_id == 0) {
        return BOX_ERR_EVENT_FAILED;
    }

    result_entry_t res;
    if (!result_wait(&res, 5000)) {
        return BOX_ERR_TIMEOUT;
    }

    if (res.error_code != BOX_OK) {
        return res.error_code;
    }

    if (res.size < 1) {
        return BOX_ERR_RESULT_INVALID;
    }

    uint8_t tag_present;
    memcpy(&tag_present, res.payload, 1);

    *has_tag = (tag_present != 0);

    return BOX_OK;
}

// --- Power and system control ---

int reboot(void) {
    hw_reboot();
    notify();

    result_entry_t result;
    result_wait(&result, 5000);
    return -1;
}

int shutdown(void) {
    hw_shutdown();
    notify();

    result_entry_t result;
    result_wait(&result, 5000);
    return -1;
}

int sysinfo(system_info_t* info) {
    if (!info) return -1;

    memcpy(info->version, "BoxOS v0.1.0", 13);
    info->version[13] = '\0';
    info->uptime_seconds = 0;
    info->total_memory = 16 * 1024 * 1024;
    info->used_memory = 8 * 1024 * 1024;

    return 0;
}

// --- Filesystem operations ---

int defrag(uint32_t file_id, uint32_t target_block) {
    fs_defrag(file_id, target_block);
    notify();

    result_entry_t result;
    if (!result_wait(&result, 5000)) return -1;
    if (result.error_code != BOX_OK) return -1;
    if (result.size < 8) return -1;

    uint32_t error_code, frag_score;
    memcpy(&error_code, result.payload, 4);
    memcpy(&frag_score, result.payload + 4, 4);

    return (error_code != 0) ? -1 : (int)frag_score;
}

int fragmentation(void) {
    fs_fraginfo();
    notify();

    result_entry_t result;
    if (!result_wait(&result, 5000)) return -1;
    if (result.error_code != BOX_OK) return -1;
    if (result.size < 4) return -1;

    uint32_t score;
    memcpy(&score, result.payload, 4);
    return (int)score;
}
