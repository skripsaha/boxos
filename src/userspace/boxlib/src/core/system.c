#include "box/system.h"
#include "box/notify.h"
#include "box/ipc.h"
#include "box/io.h"
#include "box/result.h"
#include "box/string.h"
#include "box/error.h"

int proc_info(uint16_t pid, proc_info_t* info) {
    if (!info) {
        return ERR_INVALID_ARGS;
    }

    uint8_t args[8];
    memset(args, 0, sizeof(args));
    memcpy(args, &pid, sizeof(pid));
    pocket_send(DECK_SYSTEM, 0x03, args, sizeof(args));

    Result result;
    if (!result_wait(&result, 5000)) {
        return ERR_TIMEOUT;
    }

    if (result.error_code != OK) {
        return result.error_code;
    }

    if (result.data_length < 8 || result.data_addr == 0) {
        return ERR_RESULT_INVALID;
    }

    uint8_t* data = (uint8_t*)(uintptr_t)result.data_addr;
    memcpy(&info->pid, data, 2);
    memcpy(&info->state, data + 2, 1);
    memcpy(&info->priority, data + 3, 1);
    memcpy(&info->memory_usage, data + 4, 4);

    return OK;
}

void exit(uint32_t exit_code) {
    io_flush();

    CabinInfo* ci = cabin_info();
    if (ci->spawner_pid != 0) {
        uint8_t msg[2] = {0xFE, (uint8_t)(exit_code & 0xFF)};
        send(ci->spawner_pid, msg, 2);
    }

    uint32_t zero_pid = 0;
    uint8_t kill_args[8];
    memset(kill_args, 0, sizeof(kill_args));
    memcpy(kill_args, &zero_pid, sizeof(zero_pid));
    pocket_send(DECK_SYSTEM, 0x02, kill_args, sizeof(kill_args));

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

    uint8_t spawn_args[192];
    memset(spawn_args, 0, sizeof(spawn_args));
    memcpy(spawn_args, filename, name_len);
    pocket_send(DECK_SYSTEM, 0x06, spawn_args, sizeof(spawn_args));

    Result result;
    if (!result_wait(&result, 5000)) {
        return -1;
    }

    if (result.error_code != OK) {
        return -1;
    }

    if (result.data_length < 4 || result.data_addr == 0) {
        return -1;
    }

    uint32_t new_pid = 0;
    memcpy(&new_pid, (void*)(uintptr_t)result.data_addr, 4);
    return (int)new_pid;
}

int buffer_alloc(uint8_t size_class, uint16_t* out_buffer_id, uint32_t* out_address) {
    if (!out_buffer_id || !out_address) {
        return ERR_INVALID_ARGS;
    }

    if (size_class > BUFFER_SIZE_4K) {
        return ERR_INVALID_ARGS;
    }

    pocket_send(DECK_SYSTEM, 0x10, &size_class, sizeof(size_class));

    Result result;
    if (!result_wait(&result, 5000)) {
        return ERR_TIMEOUT;
    }

    if (result.error_code != OK) {
        return result.error_code;
    }

    if (result.data_length < 6 || result.data_addr == 0) {
        return ERR_RESULT_INVALID;
    }

    uint8_t* data = (uint8_t*)(uintptr_t)result.data_addr;
    uint16_t buffer_id;
    uint32_t address;
    memcpy(&buffer_id, data, 2);
    memcpy(&address, data + 2, 4);

    *out_buffer_id = buffer_id;
    *out_address = address;

    return OK;
}

int buffer_free(uint16_t buffer_id) {
    pocket_send(DECK_SYSTEM, 0x11, &buffer_id, sizeof(buffer_id));

    Result result;
    if (!result_wait(&result, 5000)) {
        return ERR_TIMEOUT;
    }

    if (result.error_code != OK) {
        return result.error_code;
    }

    return OK;
}

static void ptag_send(uint16_t pid, const char* tag, uint8_t opcode) {
    struct PACKED {
        uint16_t pid;
        char tag[32];
    } args;
    args.pid = pid;
    memset(args.tag, 0, sizeof(args.tag));
    size_t len = strlen(tag);
    if (len > 31) len = 31;
    memcpy(args.tag, tag, len);
    pocket_send(DECK_SYSTEM, opcode, &args, sizeof(args));
}

int proc_tag_add(const char* tag) {
    if (!tag) {
        return ERR_INVALID_ARGS;
    }

    size_t tag_len = strlen(tag);
    if (tag_len == 0 || tag_len >= 32) {
        return ERR_INVALID_ARGS;
    }

    uint32_t my_pid = cabin_info()->pid;
    ptag_send((uint16_t)my_pid, tag, 0x20);

    Result res;
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

    uint32_t my_pid = cabin_info()->pid;
    ptag_send((uint16_t)my_pid, tag, 0x21);

    Result res;
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

    uint32_t my_pid = cabin_info()->pid;
    ptag_send((uint16_t)my_pid, tag, 0x22);

    Result res;
    if (!result_wait(&res, 5000)) {
        return ERR_TIMEOUT;
    }

    if (res.error_code != OK) {
        return res.error_code;
    }

    if (res.data_length < 1 || res.data_addr == 0) {
        return ERR_RESULT_INVALID;
    }

    uint8_t tag_present;
    memcpy(&tag_present, (void*)(uintptr_t)res.data_addr, 1);

    *has_tag = (tag_present != 0);

    return OK;
}

int reboot(void) {
    uint8_t args[192];
    memset(args, 0, sizeof(args));
    pocket_send(DECK_HARDWARE, 0x80, args, sizeof(args));

    Result result;
    result_wait(&result, 5000);
    return -1;
}

int shutdown(void) {
    uint8_t args[192];
    memset(args, 0, sizeof(args));
    pocket_send(DECK_HARDWARE, 0x81, args, sizeof(args));

    Result result;
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
    uint8_t defrag_args[192];
    memset(defrag_args, 0, sizeof(defrag_args));
    memcpy(defrag_args,     &file_id,      4);
    memcpy(defrag_args + 4, &target_block, 4);
    pocket_send(DECK_SYSTEM, 0x18, defrag_args, sizeof(defrag_args));

    Result result;
    if (!result_wait(&result, 5000)) return -1;
    if (result.error_code != OK) return -1;
    if (result.data_length < 8 || result.data_addr == 0) return -1;

    uint8_t* data = (uint8_t*)(uintptr_t)result.data_addr;
    uint32_t error_code, frag_score;
    memcpy(&error_code, data, 4);
    memcpy(&frag_score, data + 4, 4);

    return (error_code != 0) ? -1 : (int)frag_score;
}

int fragmentation(void) {
    uint8_t frag_args[192];
    memset(frag_args, 0, sizeof(frag_args));
    pocket_send(DECK_SYSTEM, 0x19, frag_args, sizeof(frag_args));

    Result result;
    if (!result_wait(&result, 5000)) return -1;
    if (result.error_code != OK) return -1;
    if (result.data_length < 4 || result.data_addr == 0) return -1;

    uint32_t score;
    memcpy(&score, (void*)(uintptr_t)result.data_addr, 4);
    return (int)score;
}

int perf_dump(void) {
    pocket_send(DECK_SYSTEM, SYSTEM_PERF_DUMP, NULL, 0);
    Result result;
    if (!result_wait(&result, 5000)) return -1;
    return result.error_code;
}
