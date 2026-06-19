#ifndef BOX_KEYBOARD_H
#define BOX_KEYBOARD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "box/error.h"

/* Modifier bits carried in kb_char_t.flags / kb_event_t.mods. Mirror the
 * keyboard driver's event assembly (kernel/drivers/keyboard/keyboard.c). */
#define KB_MOD_SHIFT  0x01u
#define KB_MOD_CTRL   0x02u
#define KB_MOD_ALT    0x04u

typedef struct {
    char    ch;
    uint8_t scancode;
    uint8_t flags;     /* KB_MOD_* */
    uint8_t reserved;
} kb_char_t;

/* The keyboard Touch payload, published by the kernel under the "keyboard"
 * tag (TOUCH_TAG_KEYBOARD). Field order differs from kb_char_t — scancode
 * first. Consumers of the async (Touch) key stream decode this. */
typedef struct PACKED {
    uint8_t scancode;
    char    ascii;
    uint8_t mods;      /* KB_MOD_* */
} kb_event_t;

typedef struct {
    uint32_t available;
    uint32_t buffer_size;
} kb_status_t;

int kb_getchar(void);
int kb_readline(char* buffer, size_t size, bool echo);
int kb_status(kb_status_t* status);

int kb_getchar_timeout(uint32_t timeout_ms);
int kb_getchar_ex(kb_char_t* out_char);

/* Like kb_getchar_ex but bounded by `timeout_ms` — the underlying op returns
 * scancode + modifiers on every read, so a timed read keeps the full event
 * (kb_getchar_timeout drops them). Returns 0 (filled), -ERR_TIMEOUT if no key
 * arrived within the window, or another -ERR_*. */
int kb_getchar_ex_timeout(kb_char_t* out_char, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // BOX_KEYBOARD_H
