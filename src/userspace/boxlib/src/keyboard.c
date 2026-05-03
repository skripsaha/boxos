/*
 * keyboard.c — userspace keyboard wrappers (Phase 12: Manifest-only).
 *
 * Each function builds a 1-op Manifest via MfCall1. The new HW_KEYBOARD_*
 * ops put status into a u8 byte at the tail of the out_crate; readline
 * accepts up to (out_capacity - 5) characters in a single syscall.
 */

#include "box/keyboard.h"
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
     * for size bytes of line + 4 length + 1 status + 1 NUL slack.
     *
     * Kept in .bss (not on the stack): the typical caller chain
     * handle_readline → kb_readline → MfCall1 → ManifestSubmitFull
     * accumulates ~3 KB of frames; an extra 1 KB on the stack here was
     * enough — combined with the IRQ save area — to overflow into the
     * user-stack guard page during long readlines. Each Cabin runs a
     * single thread, so .bss is reentrancy-safe. */
    static uint8_t out[1030];
    uint16_t max_len = (uint16_t)size;
    uint8_t  params[3] = { (uint8_t)(max_len & 0xFF),
                           (uint8_t)(max_len >> 8),
                           (uint8_t)(echo ? 1 : 0) };

    /* Poll forever — readline is a blocking primitive. The kernel's
     * HW_KEYBOARD_READLINE op is async (returns ERR_WOULD_BLOCK when no
     * complete line is buffered yet), so we pace the retry loop with
     * kb_sleep(50 ms) and only exit on hard errors (ACCESS_DENIED) or a
     * successful line. Previously the loop was capped at 10000 iterations
     * (~8 min), which surfaced as phantom shell prompts every few minutes
     * when the user took longer than that to type. */
    for (;;) {
        memset(out, 0, sizeof(out));
        uint32_t out_actual = 0;
        Result   r;
        int rc = MfCall1(DECK_HARDWARE, HW_KB_READLINE,
                         params, sizeof(params), NULL, 0,
                         out, (uint32_t)(size + 5), &out_actual,
                         60000, &r);

        if (rc == 0) {
            /* rc=OK means the kernel produced a COMPLETE line — including
             * an empty Enter (length==0). Pass it through so the shell
             * loop can treat it as LINE_EMPTY and re-prompt. The previous
             * iteration of this fix accidentally swallowed empty Enters
             * by polling on length==0 — that path only applies when the
             * kernel returned WOULD_BLOCK below. */
            uint32_t length = (uint32_t)out[0]
                            | ((uint32_t)out[1] << 8)
                            | ((uint32_t)out[2] << 16)
                            | ((uint32_t)out[3] << 24);
            if (length >= size) length = (uint32_t)(size - 1);
            if (length > 0) memcpy(buffer, out + 4, length);
            buffer[length] = '\0';
            return (int)length;
        }
        if (rc == ERR_WOULD_BLOCK || rc == ERR_BUSY ||
            r.error_code == ERR_WOULD_BLOCK || r.error_code == ERR_BUSY) {
            /* Kernel buffer doesn't have a complete line yet — pace the
             * userspace poll. Note: with HW_KEYBOARD_READLINE async, this
             * is the ONLY path that means "no line yet"; rc=OK with
             * length=0 means "empty Enter, line is complete". */
            kb_sleep(50000);
            continue;
        }
        if (rc == ERR_ACCESS_DENIED) return -ERR_ACCESS_DENIED;
        kb_sleep(50000);
    }
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
