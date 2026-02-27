#include "box/ipc.h"
#include "box/notify.h"
#include "box/result.h"
#include "box/string.h"
#include "box/system.h"

int send(uint32_t target_pid, const void* data, uint16_t size) {
    if (target_pid == 0) return -BOX_ERR_INVALID_ARGUMENT;

    notify_page_t* np = notify_page();

    notify_prepare();

    np->route_target = target_pid;
    np->route_tag[0] = '\0';

    notify_add_prefix(BOX_DECK_SYSTEM, BOX_IPC_OP_ROUTE);

    if (data && size > 0) {
        notify_write_data(data, size);
    }

    notify_execute();

    result_entry_t result;
    if (!result_wait(&result, 500000)) {
        return -BOX_ERR_TIMEOUT;
    }

    if (result.error_code != BOX_OK) {
        return -(int)result.error_code;
    }

    return BOX_OK;
}

int broadcast(const char* tag, const void* data, uint16_t size) {
    if (!tag || tag[0] == '\0') return -BOX_ERR_INVALID_ARGUMENT;

    notify_page_t* np = notify_page();

    notify_prepare();

    np->route_target = 0;
    size_t tag_len = strlen(tag);
    if (tag_len > 31) tag_len = 31;
    memcpy(np->route_tag, tag, tag_len);
    np->route_tag[tag_len] = '\0';

    notify_add_prefix(BOX_DECK_SYSTEM, BOX_IPC_OP_ROUTE_TAG);

    if (data && size > 0) {
        notify_write_data(data, size);
    }

    notify_execute();

    result_entry_t result;
    if (!result_wait(&result, 500000)) {
        return -BOX_ERR_TIMEOUT;
    }

    if (result.error_code != BOX_OK) {
        return -(int)result.error_code;
    }

    return BOX_OK;
}

int listen(uint8_t source_type, uint8_t flags) {
    notify_prepare();
    notify_add_prefix(BOX_DECK_SYSTEM, BOX_IPC_OP_LISTEN);

    uint8_t data[4];
    data[0] = source_type;
    data[1] = flags;
    data[2] = 0;
    data[3] = 0;
    notify_write_data(data, 4);

    notify_execute();

    result_entry_t result;
    if (!result_wait(&result, 500000)) {
        return -BOX_ERR_TIMEOUT;
    }

    if (result.error_code != BOX_OK) {
        return -(int)result.error_code;
    }

    return BOX_OK;
}

bool receive(result_entry_t* out_entry) {
    if (!out_entry) return false;
    return result_pop_ipc(out_entry);
}

bool receive_wait(result_entry_t* out_entry, uint32_t timeout_ms) {
    if (!out_entry) return false;

    // Check IPC stash first (populated by VGA result_wait)
    if (result_pop_ipc(out_entry)) return true;

    // Wait for IPC results
    result_page_t* rp = result_page();
    uint32_t iterations = 0;
    uint32_t max_iterations = timeout_ms / 10;

    while (1) {
        __sync_synchronize();

        if (rp->notification_flag != 0 || result_available() || result_ipc_stash_count() > 0) {
            if (result_pop_ipc(out_entry)) {
                rp->notification_flag = 0;
                return true;
            }
            rp->notification_flag = 0;
        }

        if (timeout_ms > 0 && iterations++ >= max_iterations) {
            return false;
        }

        yield();
    }
}
