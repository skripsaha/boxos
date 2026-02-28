#include "box/chain.h"
#include "box/notify.h"
#include "box/result.h"
#include "box/error.h"
#include "box/string.h"
#include "box/system.h"

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
