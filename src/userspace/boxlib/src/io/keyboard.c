#include "box/io/keyboard.h"
#include "box/chain.h"
#include "box/notify.h"
#include "box/result.h"
#include "box/string.h"

// Scratch buffer for keyboard data (must be in mapped memory)
static uint8_t g_kb_buf[256] __attribute__((aligned(16)));

static void kb_sleep(uint32_t iterations) {
    for (uint32_t i = 0; i < iterations; i++) {
        __asm__ volatile("pause");
    }
}

static uint32_t kb_decode_u32(const uint8_t* data, size_t offset) {
    return ((uint32_t)data[offset] << 24) |
           ((uint32_t)data[offset + 1] << 16) |
           ((uint32_t)data[offset + 2] << 8) |
           ((uint32_t)data[offset + 3]);
}

int kb_getchar(void) {
    hw_kb_getchar();

    Result result;
    if (!result_wait(&result, 1000)) {
        return -ERR_TIMEOUT;
    }

    if (result.error_code != OK) {
        return -(int)result.error_code;
    }

    if (result.data_addr == 0 || result.data_length == 0) {
        return -ERR_RESULT_INVALID;
    }

    return (int)(uint8_t)(*(uint8_t*)(uintptr_t)result.data_addr);
}

int kb_getchar_timeout(uint32_t timeout_ms) {
    hw_kb_getchar();

    Result result;
    if (!result_wait(&result, timeout_ms)) {
        return -ERR_TIMEOUT;
    }

    if (result.error_code != OK) {
        return -(int)result.error_code;
    }

    if (result.data_addr == 0 || result.data_length == 0) {
        return -ERR_RESULT_INVALID;
    }

    return (int)(uint8_t)(*(uint8_t*)(uintptr_t)result.data_addr);
}

int kb_getchar_ex(kb_char_t* out_char) {
    if (!out_char) {
        return -ERR_INVALID_ARGS;
    }

    hw_kb_getchar();

    Result result;
    if (!result_wait(&result, 1000)) {
        return -ERR_TIMEOUT;
    }

    if (result.error_code != OK) {
        return -(int)result.error_code;
    }

    if (result.data_addr == 0 || result.data_length < 3) {
        return -ERR_RESULT_INVALID;
    }

    uint8_t* data = (uint8_t*)(uintptr_t)result.data_addr;
    out_char->ch = data[0];
    out_char->scancode = data[1];
    out_char->flags = data[2];
    out_char->reserved = 0;

    return 0;
}

int kb_readline(char* buffer, size_t size, bool echo) {
    if (!buffer || size == 0 || size > 256) {
        return -ERR_INVALID_ARGS;
    }

    const uint32_t max_retries = 10000;
    uint32_t retry_count = 0;

    while (retry_count < max_retries) {
        hw_kb_readline((uint8_t)size, echo);

        Result result;
        if (!result_wait(&result, 60000)) {
            return -ERR_TIMEOUT;
        }

        if (result.error_code == OK) {
            if (result.data_addr == 0 || result.data_length < 4) {
                buffer[0] = '\0';
                return 0;
            }

            uint8_t* data = (uint8_t*)(uintptr_t)result.data_addr;
            uint32_t length = kb_decode_u32(data, 0);

            if (length == 0) {
                buffer[0] = '\0';
                return 0;
            }

            if (length >= size) {
                length = size - 1;
            }

            memcpy(buffer, data + 4, length);
            buffer[length] = '\0';

            return (int)length;
        } else if (result.error_code == ERR_WOULD_BLOCK ||
                   result.error_code == ERR_BUSY) {
            retry_count++;
            kb_sleep(50000);
            continue;
        } else if (result.error_code == ERR_ACCESS_DENIED) {
            return -ERR_ACCESS_DENIED;
        } else {
            retry_count++;
            kb_sleep(50000);
            continue;
        }
    }

    return -ERR_TIMEOUT;
}

int kb_readline_async(char* buffer, size_t size, bool echo) {
    if (!buffer || size == 0 || size > 256) {
        return -ERR_INVALID_ARGS;
    }

    memset(g_kb_buf, 0, 4);
    g_kb_buf[0] = (uint8_t)(size >> 8);
    g_kb_buf[1] = (uint8_t)(size & 0xFF);
    g_kb_buf[2] = echo ? 1 : 0;
    g_kb_buf[3] = 0;

    Pocket p;
    pocket_prepare(&p);
    pocket_set_data(&p, g_kb_buf, 4);
    pocket_add_prefix(&p, DECK_HARDWARE, KB_OP_READLINE);
    pocket_submit(&p);

    return 0;
}

int kb_status(kb_status_t* status) {
    if (!status) {
        return -ERR_INVALID_ARGS;
    }

    hw_kb_status();

    Result result;
    if (!result_wait(&result, 1000)) {
        return -ERR_TIMEOUT;
    }

    if (result.error_code != OK) {
        return -(int)result.error_code;
    }

    if (result.data_addr == 0 || result.data_length < 8) {
        return -ERR_RESULT_INVALID;
    }

    uint8_t* data = (uint8_t*)(uintptr_t)result.data_addr;
    status->available = kb_decode_u32(data, 0);
    status->buffer_size = kb_decode_u32(data, 4);

    return 0;
}
