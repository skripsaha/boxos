/*
 * keyboard.c — userspace keyboard wrappers (Phase 12: Manifest-only).
 */

#include "box/keyboard.h"
#include "box/touch.h"
#include "box/vga.h"
#include "box/core/manifest.h"
#include "box/core/notify.h"
#include "box/core/result.h"
#include "box/string.h"

#define HW_KB_GETCHAR   0x60
#define HW_KB_READLINE  0x61
#define HW_KB_STATUS    0x62

#define HW_KB_SUCCESS    0
#define HW_KB_NO_DATA    1
#define HW_KB_WOULD_BLOCK 3

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

    /* Claim TOUCH_TAG_KEYBOARD once per process. The kernel handles a
     * repeat-claim as a no-op but each call is still a manifest
     * round-trip — 1 syscall + 1 reply per kb_readline before the
     * process even got a chance to read its first character. Skipping
     * after the first success drops that overhead. */
    static bool s_claimed = false;
    TouchTag kb_tag = TOUCH_TAG_ID(TOUCH_TAG_KEYBOARD);
    if (!s_claimed) {
        if (touch_claim(kb_tag, TOUCH_REST, 0, 0) == 0) {
            s_claimed = true;
        }
    }

    size_t pos   = 0;
    bool   done  = false;

    while (!done) {
        Touch t;
        int rc = touch_await(kb_tag, &t, 30000);
        if (rc != 0) continue;

        /* Payload is inline inside `t` (TouchRing copies it on pop) —
         * no separate cabin allocation, lifetime is the local `Touch t`. */
        if (t.payload_len < sizeof(kb_event_t)) continue;

        const kb_event_t *kp = (const kb_event_t *)t.payload;
        char ch = kp->ascii;
        if (ch == 0) continue;

        if (ch == '\b' || ch == 0x7F) {
            if (pos > 0) {
                pos--;
                if (echo) vga_puts("\b \b");
            }
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            buffer[pos] = '\0';
            if (echo) vga_puts("\n");
            done = true;
            continue;
        }

        if (pos < size - 1) {
            buffer[pos++] = ch;
            if (echo) vga_putchar(ch);
        }
    }

    buffer[pos] = '\0';
    return (int)pos;
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
