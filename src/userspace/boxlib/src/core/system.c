#include "box/system.h"
#include "box/notify.h"
#include "box/ipc.h"
#include "box/io.h"
#include "box/chain.h"
#include "box/result.h"
#include "box/string.h"
#include "box/error.h"

int proc_info(uint16_t pid, proc_info_t* info) {
    if (!info) {
        return ERR_INVALID_ARGS;
    }

    proc_query(pid);
    event_id_t event_id = notify();
    if (event_id == 0) {
        return ERR_EVENT_FAILED;
    }

    result_entry_t result;
    if (!result_wait(&result, 5000)) {
        return ERR_TIMEOUT;
    }

    if (result.error_code != OK) {
        return result.error_code;
    }

    if (result.size < 8) {
        return ERR_RESULT_INVALID;
    }

    memcpy(&info->pid, result.payload, 2);
    memcpy(&info->state, result.payload + 2, 1);
    memcpy(&info->priority, result.payload + 3, 1);
    memcpy(&info->memory_usage, result.payload + 4, 4);

    return OK;
}

void exit(uint32_t exit_code) {
    io_flush();

    notify_page_t* np = notify_page();
    if (np->parent_pid != 0) {
        uint8_t msg[2] = {0xFE, (uint8_t)(exit_code & 0xFF)};
        send(np->parent_pid, msg, 2);
    }

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

    if (result.error_code != OK) {
        return -1;
    }

    if (result.size < 4) {
        return -1;
    }

    uint32_t new_pid = 0;
    memcpy(&new_pid, result.payload, 4);
    return (int)new_pid;
}

int buffer_alloc(uint8_t size_class, uint16_t* out_buffer_id, uint32_t* out_address) {
    if (!out_buffer_id || !out_address) {
        return ERR_INVALID_ARGS;
    }

    if (size_class > BUFFER_SIZE_4K) {
        return ERR_INVALID_ARGS;
    }

    buf_alloc(size_class);
    event_id_t event_id = notify();
    if (event_id == 0) {
        return ERR_EVENT_FAILED;
    }

    result_entry_t result;
    if (!result_wait(&result, 5000)) {
        return ERR_TIMEOUT;
    }

    if (result.error_code != OK) {
        return result.error_code;
    }

    if (result.size < 6) {
        return ERR_RESULT_INVALID;
    }

    uint16_t buffer_id;
    uint32_t address;
    memcpy(&buffer_id, result.payload, 2);
    memcpy(&address, result.payload + 2, 4);

    *out_buffer_id = buffer_id;
    *out_address = address;

    return OK;
}

int buffer_free(uint16_t buffer_id) {
    buf_release(buffer_id);
    event_id_t event_id = notify();
    if (event_id == 0) {
        return ERR_EVENT_FAILED;
    }

    result_entry_t result;
    if (!result_wait(&result, 5000)) {
        return ERR_TIMEOUT;
    }

    if (result.error_code != OK) {
        return result.error_code;
    }

    return OK;
}

int proc_tag_add(const char* tag) {
    if (!tag) {
        return ERR_INVALID_ARGS;
    }

    size_t tag_len = strlen(tag);
    if (tag_len == 0 || tag_len >= 32) {
        return ERR_INVALID_ARGS;
    }

    proc_info_t info;
    int rc = proc_info(0, &info);
    if (rc != OK) {
        return rc;
    }

    ptag_add(info.pid, tag);
    event_id_t event_id = notify();
    if (event_id == 0) {
        return ERR_EVENT_FAILED;
    }

    result_entry_t res;
    if (!result_wait(&res, 5000)) {
        return ERR_TIMEOUT;
    }

    if (res.error_code != OK) {
        return res.error_code;
    }

    return OK;
}

int proc_tag_remove(const char* tag) {
    if (!tag) {
        return ERR_INVALID_ARGS;
    }

    size_t tag_len = strlen(tag);
    if (tag_len == 0 || tag_len >= 32) {
        return ERR_INVALID_ARGS;
    }

    proc_info_t info;
    int rc = proc_info(0, &info);
    if (rc != OK) {
        return rc;
    }

    ptag_remove(info.pid, tag);
    event_id_t event_id = notify();
    if (event_id == 0) {
        return ERR_EVENT_FAILED;
    }

    result_entry_t res;
    if (!result_wait(&res, 5000)) {
        return ERR_TIMEOUT;
    }

    if (res.error_code != OK) {
        return res.error_code;
    }

    return OK;
}

int proc_tag_check(const char* tag, bool* has_tag) {
    if (!tag || !has_tag) {
        return ERR_INVALID_ARGS;
    }

    size_t tag_len = strlen(tag);
    if (tag_len == 0 || tag_len >= 32) {
        return ERR_INVALID_ARGS;
    }

    proc_info_t info;
    int rc = proc_info(0, &info);
    if (rc != OK) {
        return rc;
    }

    ptag_check(info.pid, tag);
    event_id_t event_id = notify();
    if (event_id == 0) {
        return ERR_EVENT_FAILED;
    }

    result_entry_t res;
    if (!result_wait(&res, 5000)) {
        return ERR_TIMEOUT;
    }

    if (res.error_code != OK) {
        return res.error_code;
    }

    if (res.size < 1) {
        return ERR_RESULT_INVALID;
    }

    uint8_t tag_present;
    memcpy(&tag_present, res.payload, 1);

    *has_tag = (tag_present != 0);

    return OK;
}

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

int defrag(uint32_t file_id, uint32_t target_block) {
    fs_defrag(file_id, target_block);
    notify();

    result_entry_t result;
    if (!result_wait(&result, 5000)) return -1;
    if (result.error_code != OK) return -1;
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
    if (result.error_code != OK) return -1;
    if (result.size < 4) return -1;

    uint32_t score;
    memcpy(&score, result.payload, 4);
    return (int)score;
}
