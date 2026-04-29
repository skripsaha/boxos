/*
 * keyboard.c — userspace keyboard wrappers (Phase 12: Manifest-only).
 *
 * Each function builds a 1-op Manifest via MfCall1. The new HW_KEYBOARD_*
 * ops put status into a u8 byte at the tail of the out_crate; readline
 * accepts up to (out_capacity - 5) characters in a single syscall.
 */

#include "box/io/keyboard.h"
#include "box/manifest.h"
#include "box/notify.h"
#include "box/result.h"
#include "box/string.h"

#define HW_KB_GETCHAR   0x60
#define HW_KB_READLINE  0x61
#define HW_KB_STATUS    0x62

#define HW_KB_SUCCESS    0
#define HW_KB_NO_DATA    1
#define HW_KB_WOULD_BLOCK 3

static void kb_sleep(uint32_t iterations)
{
    for (uint32_t i = 0; i < iterations; i++) {
        __asm__ volatile("pause");
    }
}

int kb_getchar(void)
{
    uint8_t out[4] = {0};
    int rc = MfCall1(DECK_HARDWARE, HW_KB_GETCHAR,
                     NULL, 0, NULL, 0,
                     out, sizeof(out), NULL,
                     1000, NULL);
    if (rc != 0) return rc < 0 ? rc : -rc;
    if (out[3] != HW_KB_SUCCESS) return -ERR_RESULT_INVALID;
    return (int)out[0];
}

int kb_getchar_timeout(uint32_t timeout_ms)
{
    uint8_t out[4] = {0};
    int rc = MfCall1(DECK_HARDWARE, HW_KB_GETCHAR,
                     NULL, 0, NULL, 0,
                     out, sizeof(out), NULL,
                     timeout_ms, NULL);
    if (rc != 0) return rc < 0 ? rc : -rc;
    if (out[3] != HW_KB_SUCCESS) return -ERR_RESULT_INVALID;
    return (int)out[0];
}

int kb_getchar_ex(kb_char_t *out_char)
{
    if (!out_char) return -ERR_INVALID_ARGS;

    uint8_t out[4] = {0};
    int rc = MfCall1(DECK_HARDWARE, HW_KB_GETCHAR,
                     NULL, 0, NULL, 0,
                     out, sizeof(out), NULL,
                     1000, NULL);
    if (rc != 0) return rc < 0 ? rc : -rc;
    if (out[3] != HW_KB_SUCCESS) return -ERR_RESULT_INVALID;

    out_char->ch       = out[0];
    out_char->scancode = out[1];
    out_char->flags    = out[2];
    out_char->reserved = 0;
    return 0;
}

int kb_readline(char *buffer, size_t size, bool echo)
{
    if (!buffer || size == 0 || size > 1024) return -ERR_INVALID_ARGS;

    /* Out-crate layout: [u32 len][char line[]][u8 status]. We size the buffer
     * for size bytes of line + 4 length + 1 status + 1 NUL slack. */
    uint8_t out[1030];
    uint16_t max_len = (uint16_t)size;
    uint8_t  params[3] = { (uint8_t)(max_len & 0xFF),
                           (uint8_t)(max_len >> 8),
                           (uint8_t)(echo ? 1 : 0) };

    const uint32_t max_retries = 10000;
    for (uint32_t retry = 0; retry < max_retries; retry++) {
        memset(out, 0, sizeof(out));
        uint32_t out_actual = 0;
        Result   r;
        int rc = MfCall1(DECK_HARDWARE, HW_KB_READLINE,
                         params, sizeof(params), NULL, 0,
                         out, (uint32_t)(size + 5), &out_actual,
                         60000, &r);

        if (rc == 0) {
            uint32_t length = (uint32_t)out[0]
                            | ((uint32_t)out[1] << 8)
                            | ((uint32_t)out[2] << 16)
                            | ((uint32_t)out[3] << 24);
            if (length == 0) { buffer[0] = '\0'; return 0; }
            if (length >= size) length = (uint32_t)(size - 1);
            memcpy(buffer, out + 4, length);
            buffer[length] = '\0';
            return (int)length;
        }
        if (rc == ERR_WOULD_BLOCK || rc == ERR_BUSY ||
            r.error_code == ERR_WOULD_BLOCK || r.error_code == ERR_BUSY) {
            kb_sleep(50000);
            continue;
        }
        if (rc == ERR_ACCESS_DENIED) return -ERR_ACCESS_DENIED;
        kb_sleep(50000);
    }
    return -ERR_TIMEOUT;
}

int kb_status(kb_status_t *status)
{
    if (!status) return -ERR_INVALID_ARGS;
    uint8_t out[8] = {0};
    int rc = MfCall1(DECK_HARDWARE, HW_KB_STATUS,
                     NULL, 0, NULL, 0,
                     out, sizeof(out), NULL,
                     1000, NULL);
    if (rc != 0) return rc < 0 ? rc : -rc;
    status->available   = (uint32_t)out[0]
                        | ((uint32_t)out[1] << 8)
                        | ((uint32_t)out[2] << 16)
                        | ((uint32_t)out[3] << 24);
    status->buffer_size = (uint32_t)out[4]
                        | ((uint32_t)out[5] << 8)
                        | ((uint32_t)out[6] << 16)
                        | ((uint32_t)out[7] << 24);
    return 0;
}
