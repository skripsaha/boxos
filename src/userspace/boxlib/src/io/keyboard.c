#include "box/io/keyboard.h"
#include "box/chain.h"
#include "box/notify.h"
#include "box/result.h"
#include "box/string.h"

static void kb_sleep(uint32_t iterations) {
    for (uint32_t i = 0; i < iterations; i++) {
        __asm__ volatile("pause");
    }
}

static uint32_t kb_decode_u32(const uint8_t* payload, size_t offset) {
    return ((uint32_t)payload[offset] << 24) |
           ((uint32_t)payload[offset + 1] << 16) |
           ((uint32_t)payload[offset + 2] << 8) |
           ((uint32_t)payload[offset + 3]);
}

int kb_getchar(void) {
    hw_kb_getchar();
    notify();

    result_entry_t result;
    if (!result_wait(&result, 1000)) {
        return -BOX_ERR_TIMEOUT;
    }

    if (result.error_code != BOX_OK) {
        return -(int)result.error_code;
    }

    return (int)(uint8_t)result.payload[0];
}

int kb_getchar_timeout(uint32_t timeout_ms) {
    hw_kb_getchar();
    notify();

    result_entry_t result;
    if (!result_wait(&result, timeout_ms)) {
        return -BOX_ERR_TIMEOUT;
    }

    if (result.error_code != BOX_OK) {
        return -(int)result.error_code;
    }

    return (int)(uint8_t)result.payload[0];
}

int kb_getchar_ex(kb_char_t* out_char) {
    if (!out_char) {
        return -BOX_ERR_INVALID_ARGS;
    }

    hw_kb_getchar();
    notify();

    result_entry_t result;
    if (!result_wait(&result, 1000)) {
        return -BOX_ERR_TIMEOUT;
    }

    if (result.error_code != BOX_OK) {
        return -(int)result.error_code;
    }

    out_char->ch = result.payload[0];
    out_char->scancode = result.payload[1];
    out_char->flags = result.payload[2];
    out_char->reserved = 0;

    return 0;
}

int kb_readline(char* buffer, size_t size, bool echo) {
    if (!buffer || size == 0 || size > 256) {
        return -BOX_ERR_INVALID_ARGS;
    }

    const uint32_t max_retries = 10000;
    uint32_t retry_count = 0;

    while (retry_count < max_retries) {
        hw_kb_readline((uint8_t)size, echo);
        notify();

        result_entry_t result;
        if (!result_wait(&result, 60000)) {
            return -BOX_ERR_TIMEOUT;
        }

        if (result.error_code == BOX_OK) {
            uint32_t length = kb_decode_u32(result.payload, 0);

            if (length == 0) {
                retry_count++;
                kb_sleep(5000);
                continue;
            }

            if (length >= size) {
                length = size - 1;
            }

            memcpy(buffer, result.payload + 4, length);
            buffer[length] = '\0';

            return (int)length;
        } else if (result.error_code == BOX_ERR_WOULD_BLOCK ||
                   result.error_code == BOX_ERR_BUSY) {
            retry_count++;
            kb_sleep(50000);
            continue;
        } else if (result.error_code == BOX_ERR_ACCESS_DENIED) {
            return -BOX_ERR_ACCESS_DENIED;
        } else {
            retry_count++;
            kb_sleep(50000);
            continue;
        }
    }

    return -BOX_ERR_TIMEOUT;
}

int kb_readline_async(char* buffer, size_t size, bool echo) {
    if (!buffer || size == 0 || size > 256) {
        return -BOX_ERR_INVALID_ARGS;
    }

    // Non-standard data encoding (16-bit size split), use raw notify helpers
    notify_page_t* np = notify_page();
    if (np->magic != BOX_NOTIFY_MAGIC) {
        notify_prepare();
    }

    uint8_t data[4];
    data[0] = (uint8_t)(size >> 8);
    data[1] = (uint8_t)(size & 0xFF);
    data[2] = echo ? 1 : 0;
    data[3] = 0;

    notify_write_data(data, 4);
    notify_add_prefix(BOX_DECK_HARDWARE, KB_OP_READLINE);
    event_id_t event_id = notify();

    return (int)event_id;
}

int kb_status(kb_status_t* status) {
    if (!status) {
        return -BOX_ERR_INVALID_ARGS;
    }

    hw_kb_status();
    notify();

    result_entry_t result;
    if (!result_wait(&result, 1000)) {
        return -BOX_ERR_TIMEOUT;
    }

    if (result.error_code != BOX_OK) {
        return -(int)result.error_code;
    }

    status->available = kb_decode_u32(result.payload, 0);
    status->buffer_size = kb_decode_u32(result.payload, 4);

    return 0;
}
