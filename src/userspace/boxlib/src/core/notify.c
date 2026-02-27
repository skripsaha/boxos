#include "box/notify.h"
#include "box/string.h"

void box_notify_prepare(void) {
    box_notify_page_t* np = box_notify_page();
    memset(np, 0, sizeof(box_notify_page_t));
    np->magic = BOX_NOTIFY_MAGIC;
}

bool box_notify_add_prefix(uint8_t deck_id, uint8_t opcode) {
    box_notify_page_t* np = box_notify_page();
    if (np->prefix_count >= BOX_MAX_PREFIXES) {
        return false;
    }
    np->prefixes[np->prefix_count++] = BOX_PREFIX(deck_id, opcode);
    return true;
}

size_t box_notify_write_data(const void* data, size_t size) {
    box_notify_page_t* np = box_notify_page();

    // Kernel copies only BOX_INLINE_DATA_SIZE (256) bytes
    size_t to_write = (size > BOX_INLINE_DATA_SIZE)
                      ? BOX_INLINE_DATA_SIZE
                      : size;

    memcpy(np->data, data, to_write);
    return to_write;
}

box_event_id_t box_notify_execute(void) {
    uint64_t result;
    __asm__ volatile(
        "int $0x80"
        : "=a"(result)
        :
        : "memory"
    );
    return (box_event_id_t)result;
}

box_event_id_t box_notify(uint8_t deck_id, uint8_t opcode,
                          const void* data, size_t data_size) {
    box_notify_prepare();
    box_notify_add_prefix(deck_id, opcode);
    box_notify_write_data(data, data_size);
    return box_notify_execute();
}

int box_notify_checked(uint8_t deck_id, uint8_t opcode,
                       const void* data, size_t data_size,
                       box_event_id_t* out_event_id) {
    box_notify_page_t* np = box_notify_page();

    box_notify_prepare();
    box_notify_add_prefix(deck_id, opcode);
    box_notify_write_data(data, data_size);

    box_event_id_t event_id = box_notify_execute();

    // Check if kernel set error flag
    if (np->flags & BOX_NOTIFY_FLAG_CHECK_STATUS) {
        if (out_event_id) {
            *out_event_id = 0;
        }
        return np->status;
    }

    if (out_event_id) {
        *out_event_id = event_id;
    }
    return BOX_NOTIFY_STATUS_OK;
}

box_event_id_t box_notify_with_retry(uint8_t deck_id, uint8_t opcode,
                                     const void* data, size_t data_size,
                                     uint32_t max_retries) {
    box_event_id_t event_id;
    uint32_t backoff = 1;

    for (uint32_t attempt = 0; attempt <= max_retries; attempt++) {
        int status = box_notify_checked(deck_id, opcode, data, data_size, &event_id);

        if (status == BOX_NOTIFY_STATUS_OK) {
            return event_id;
        }

        if (status != BOX_NOTIFY_STATUS_RING_FULL) {
            return (box_event_id_t)-1;
        }

        // Exponential backoff: wait 2^attempt iterations
        for (uint32_t i = 0; i < backoff; i++) {
            __asm__ volatile("pause");
        }
        backoff *= 2;
        if (backoff > 1024) {
            backoff = 1024;
        }
    }

    return (box_event_id_t)-1;
}
