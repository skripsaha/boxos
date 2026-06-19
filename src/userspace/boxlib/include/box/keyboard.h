#ifndef BOX_KEYBOARD_H
#define BOX_KEYBOARD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "box/error.h"
#include "kb_event.h"  /* KB_MOD_SHIFT/CTRL/ALT + kb_event_t — the shared
                        * kernel/userspace ABI for a published keyboard event */

/* A key read from the synchronous HW ring (HW_KB_GETCHAR). Userspace-only
 * convenience layout (the op returns ch / scancode / flags as out[0..2]) —
 * note the field order differs from kb_event_t (the Touch payload). */
typedef struct {
    char    ch;
    uint8_t scancode;
    uint8_t flags;     /* KB_MOD_* */
    uint8_t reserved;
} kb_char_t;

typedef struct {
    uint32_t available;
    uint32_t buffer_size;
} kb_status_t;

int kb_getchar(void);
int kb_readline(char* buffer, size_t size, bool echo);
int kb_status(kb_status_t* status);

int kb_getchar_timeout(uint32_t timeout_ms);

/* Non-blocking structured read: fills out_char from the ring if a key is
 * buffered, else returns -ERR_RESULT_INVALID immediately. The HW_KB_GETCHAR op
 * does not block, so for a blocking / timed read use the "keyboard" Touch
 * channel (touch_await), which waits in the kernel — see box::keyboard_events. */
int kb_getchar_ex(kb_char_t* out_char);

#ifdef __cplusplus
}
#endif

#endif // BOX_KEYBOARD_H
