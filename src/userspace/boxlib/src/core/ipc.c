#include "box/ipc.h"
#include "box/notify.h"
#include "box/result.h"
#include "box/string.h"

int send(uint32_t target_pid, const void* data, uint16_t size) {
    if (target_pid == 0) return -BOX_ERR_INVALID_ARGUMENT;

    box_notify_page_t* np = box_notify_page();

    box_notify_prepare();

    np->route_target = target_pid;
    np->route_tag[0] = '\0';

    box_notify_add_prefix(BOX_DECK_SYSTEM, BOX_IPC_OP_ROUTE);

    if (data && size > 0) {
        box_notify_write_data(data, size);
    }

    box_notify_execute();

    box_result_entry_t result;
    if (!box_result_wait(&result, 500000)) {
        return -BOX_ERR_TIMEOUT;
    }

    if (result.error_code != BOX_OK) {
        return -(int)result.error_code;
    }

    return BOX_OK;
}

int broadcast(const char* tag, const void* data, uint16_t size) {
    if (!tag || tag[0] == '\0') return -BOX_ERR_INVALID_ARGUMENT;

    box_notify_page_t* np = box_notify_page();

    box_notify_prepare();

    np->route_target = 0;
    size_t tag_len = strlen(tag);
    if (tag_len > 31) tag_len = 31;
    memcpy(np->route_tag, tag, tag_len);
    np->route_tag[tag_len] = '\0';

    box_notify_add_prefix(BOX_DECK_SYSTEM, BOX_IPC_OP_ROUTE_TAG);

    if (data && size > 0) {
        box_notify_write_data(data, size);
    }

    box_notify_execute();

    box_result_entry_t result;
    if (!box_result_wait(&result, 500000)) {
        return -BOX_ERR_TIMEOUT;
    }

    if (result.error_code != BOX_OK) {
        return -(int)result.error_code;
    }

    return BOX_OK;
}

int listen(uint8_t source_type, uint8_t flags) {
    box_notify_prepare();
    box_notify_add_prefix(BOX_DECK_SYSTEM, BOX_IPC_OP_LISTEN);

    uint8_t data[4];
    data[0] = source_type;
    data[1] = flags;
    data[2] = 0;
    data[3] = 0;
    box_notify_write_data(data, 4);

    box_notify_execute();

    box_result_entry_t result;
    if (!box_result_wait(&result, 500000)) {
        return -BOX_ERR_TIMEOUT;
    }

    if (result.error_code != BOX_OK) {
        return -(int)result.error_code;
    }

    return BOX_OK;
}

bool receive(box_result_entry_t* out_entry) {
    if (!out_entry) return false;
    return box_result_pop(out_entry);
}

bool receive_wait(box_result_entry_t* out_entry, uint32_t timeout_ms) {
    if (!out_entry) return false;
    return box_result_wait_smart(out_entry, timeout_ms);
}
