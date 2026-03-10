#include "box/io/keyboard.h"
#include "box/notify.h"
#include "box/result.h"
#include "box/string.h"

static void kb_sleep(uint32_t iterations) {
    for (uint32_t i = 0; i < iterations; i++) {
        __asm__ volatile("pause");
    }
}

static uint32_t kb_decode_u32(const void* ptr) {
    uint32_t v;
    memcpy(&v, ptr, sizeof(v));
    return v;
}

int kb_getchar(void) {
    uint8_t buf[4] = {0};
    pocket_send(DECK_HARDWARE, KB_OP_GETCHAR, buf, 4);

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
    uint8_t buf[4] = {0};
    pocket_send(DECK_HARDWARE, KB_OP_GETCHAR, buf, 4);

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

    uint8_t getchar_buf[4] = {0};
    pocket_send(DECK_HARDWARE, KB_OP_GETCHAR, getchar_buf, 4);

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
        uint8_t readline_buf[192];
        memset(readline_buf, 0, sizeof(readline_buf));
        readline_buf[0] = (uint8_t)size;
        readline_buf[1] = echo ? 1 : 0;
        pocket_send(DECK_HARDWARE, KB_OP_READLINE, readline_buf, sizeof(readline_buf));

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
            uint32_t length = kb_decode_u32(data);

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

    uint8_t async_buf[192];
    memset(async_buf, 0, sizeof(async_buf));
    async_buf[0] = (uint8_t)size;
    async_buf[1] = echo ? 1 : 0;
    pocket_send(DECK_HARDWARE, KB_OP_READLINE, async_buf, sizeof(async_buf));

    return 0;
}

int kb_status(kb_status_t* status) {
    if (!status) {
        return -ERR_INVALID_ARGS;
    }

    uint8_t status_buf[16] = {0};
    pocket_send(DECK_HARDWARE, KB_OP_STATUS, status_buf, sizeof(status_buf));

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
    status->available = kb_decode_u32(data);
    status->buffer_size = kb_decode_u32(data + 4);

    return 0;
}
