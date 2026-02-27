#include "box/system.h"
#include "box/notify.h"
#include "box/result.h"
#include "box/string.h"

//==============================================================================
// Process Operations
//==============================================================================

int proc_info(uint16_t pid, proc_info_t* info) {
    if (!info) {
        return BOX_ERR_INVALID_ARGS;
    }

    box_notify_prepare();
    box_notify_add_prefix(BOX_SYSTEM_DECK_ID, BOX_SYSTEM_PROC_INFO);

    box_notify_write_data(&pid, sizeof(pid));
    box_event_id_t event_id = box_notify_execute();
    if (event_id == 0) {
        return BOX_ERR_EVENT_FAILED;
    }

    box_result_entry_t result;
    if (!box_result_wait(&result, 5000)) {
        return BOX_ERR_TIMEOUT;
    }

    // Check error_code field (new ABI)
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

    box_notify_prepare();
    box_notify_add_prefix(BOX_SYSTEM_DECK_ID, 0x02);

    uint16_t self_pid = 0;
    box_notify_write_data(&self_pid, sizeof(self_pid));
    box_notify_execute();

    // Should not return, but if it does, loop forever
    while (1) {
        __asm__ volatile("pause");
    }
}

//==============================================================================
// Buffer Management
//==============================================================================

int buffer_alloc(uint8_t size_class, uint16_t* out_buffer_id, uint32_t* out_address) {
    if (!out_buffer_id || !out_address) {
        return BOX_ERR_INVALID_ARGS;
    }

    if (size_class > BOX_BUFFER_SIZE_4K) {
        return BOX_ERR_INVALID_ARGS;
    }

    box_notify_prepare();
    box_notify_add_prefix(BOX_SYSTEM_DECK_ID, BOX_SYSTEM_BUFFER_ALLOC);

    box_notify_write_data(&size_class, sizeof(size_class));
    box_event_id_t event_id = box_notify_execute();
    if (event_id == 0) {
        return BOX_ERR_EVENT_FAILED;
    }

    box_result_entry_t result;
    if (!box_result_wait(&result, 5000)) {
        return BOX_ERR_TIMEOUT;
    }

    // Check error_code field (new ABI)
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
    box_notify_prepare();
    box_notify_add_prefix(BOX_SYSTEM_DECK_ID, BOX_SYSTEM_BUFFER_FREE);

    box_notify_write_data(&buffer_id, sizeof(buffer_id));
    box_event_id_t event_id = box_notify_execute();
    if (event_id == 0) {
        return BOX_ERR_EVENT_FAILED;
    }

    box_result_entry_t result;
    if (!box_result_wait(&result, 5000)) {
        return BOX_ERR_TIMEOUT;
    }

    // Check error_code field (new ABI)
    if (result.error_code != BOX_OK) {
        return result.error_code;
    }

    return BOX_OK;
}

//==============================================================================
// Process Tag Operations
//==============================================================================

int proc_tag_add(const char* tag) {
    if (!tag) {
        return BOX_ERR_INVALID_ARGS;
    }

    size_t tag_len = strlen(tag);
    if (tag_len == 0 || tag_len >= 32) {
        return BOX_ERR_INVALID_ARGS;
    }

    proc_info_t info;
    int result = proc_info(0, &info);
    if (result != BOX_OK) {
        return result;
    }

    box_notify_prepare();
    box_notify_add_prefix(BOX_SYSTEM_DECK_ID, BOX_SYSTEM_TAG_ADD);

    struct __attribute__((packed)) {
        uint16_t pid;
        char tag[32];
    } request;

    request.pid = info.pid;
    memset(request.tag, 0, sizeof(request.tag));
    strcpy(request.tag, tag);

    box_notify_write_data(&request, sizeof(request));
    box_event_id_t event_id = box_notify_execute();
    if (event_id == 0) {
        return BOX_ERR_EVENT_FAILED;
    }

    box_result_entry_t res;
    if (!box_result_wait(&res, 5000)) {
        return BOX_ERR_TIMEOUT;
    }

    // Check error_code field (new ABI)
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
    int result = proc_info(0, &info);
    if (result != BOX_OK) {
        return result;
    }

    box_notify_prepare();
    box_notify_add_prefix(BOX_SYSTEM_DECK_ID, BOX_SYSTEM_TAG_REMOVE);

    struct __attribute__((packed)) {
        uint16_t pid;
        char tag[32];
    } request;

    request.pid = info.pid;
    memset(request.tag, 0, sizeof(request.tag));
    strcpy(request.tag, tag);

    box_notify_write_data(&request, sizeof(request));
    box_event_id_t event_id = box_notify_execute();
    if (event_id == 0) {
        return BOX_ERR_EVENT_FAILED;
    }

    box_result_entry_t res;
    if (!box_result_wait(&res, 5000)) {
        return BOX_ERR_TIMEOUT;
    }

    // Check error_code field (new ABI)
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
    int result = proc_info(0, &info);
    if (result != BOX_OK) {
        return result;
    }

    box_notify_prepare();
    box_notify_add_prefix(BOX_SYSTEM_DECK_ID, BOX_SYSTEM_TAG_CHECK);

    struct __attribute__((packed)) {
        uint16_t pid;
        char tag[32];
    } request;

    request.pid = info.pid;
    memset(request.tag, 0, sizeof(request.tag));
    strcpy(request.tag, tag);

    box_notify_write_data(&request, sizeof(request));
    box_event_id_t event_id = box_notify_execute();
    if (event_id == 0) {
        return BOX_ERR_EVENT_FAILED;
    }

    box_result_entry_t res;
    if (!box_result_wait(&res, 5000)) {
        return BOX_ERR_TIMEOUT;
    }

    // Check error_code field (new ABI)
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
