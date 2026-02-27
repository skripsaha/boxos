#include "box/notify.h"
#include "box/result.h"
#include "box/error.h"
#include "box/string.h"
#include "box/types.h"
#include "box/system.h"

#define HARDWARE_DECK_ID     0x03
#define HW_SYSTEM_REBOOT     0x80
#define HW_SYSTEM_SHUTDOWN   0x81
#define SYSTEM_DECK_ID       0xFF

int reboot(void) {
    box_notify_prepare();
    box_notify_add_prefix(HARDWARE_DECK_ID, HW_SYSTEM_REBOOT);

    uint8_t data[192];
    memset(data, 0, 192);

    box_notify_write_data(data, 192);
    box_event_id_t event_id = box_notify_execute();
    if (event_id == 0) return -1;

    box_result_entry_t result;
    box_result_wait(&result, 5000);
    return -1;
}

int shutdown(void) {
    box_notify_prepare();
    box_notify_add_prefix(HARDWARE_DECK_ID, HW_SYSTEM_SHUTDOWN);

    uint8_t data[192];
    memset(data, 0, 192);

    box_notify_write_data(data, 192);
    box_event_id_t event_id = box_notify_execute();
    if (event_id == 0) return -1;

    box_result_entry_t result;
    box_result_wait(&result, 5000);
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
    box_notify_prepare();
    box_notify_add_prefix(SYSTEM_DECK_ID, 0x18);

    uint8_t data[192];
    memset(data, 0, 192);
    memcpy(data, &file_id, 4);
    memcpy(data + 4, &target_block, 4);

    box_notify_write_data(data, 192);
    box_event_id_t event_id = box_notify_execute();
    if (event_id == 0) return -1;

    box_result_entry_t result;
    if (!box_result_wait(&result, 5000)) return -1;
    if (result.error_code != BOX_OK) return -1;
    if (result.size < 8) return -1;

    uint32_t error_code, frag_score;
    memcpy(&error_code, result.payload, 4);
    memcpy(&frag_score, result.payload + 4, 4);

    return (error_code != 0) ? -1 : (int)frag_score;
}

int fragmentation(void) {
    box_notify_prepare();
    box_notify_add_prefix(SYSTEM_DECK_ID, 0x19);

    uint8_t data[192];
    memset(data, 0, 192);

    box_notify_write_data(data, 192);
    box_event_id_t event_id = box_notify_execute();
    if (event_id == 0) return -1;

    box_result_entry_t result;
    if (!box_result_wait(&result, 5000)) return -1;
    if (result.error_code != BOX_OK) return -1;
    if (result.size < 4) return -1;

    uint32_t score;
    memcpy(&score, result.payload, 4);
    return (int)score;
}
