#ifndef BOXOS_KB_EVENT_H
#define BOXOS_KB_EVENT_H

/*
 * Shared kernel/userspace ABI header — the keyboard event payload.
 *
 * The keyboard driver publishes this struct under the "keyboard" Touch tag
 * (TOUCH_TAG_KEYBOARD); userspace decodes it (box/keyboard.h, box::key). This
 * is the SINGLE source of truth for the layout and the modifier bits: the
 * kernel producer and every consumer include this header, and the static
 * assert below guards the on-the-wire size so a stray field or padding cannot
 * silently desynchronise the two sides.
 *
 * Includer must provide uint*_t BEFORE including this header.
 *   Kernel:    #include "ktypes.h"
 *   Userspace: #include "box/types.h"
 */

#ifndef __packed
#define __packed __attribute__((packed))
#endif

/* Modifier bits carried in kb_event_t.mods. */
#define KB_MOD_SHIFT  0x01u
#define KB_MOD_CTRL   0x02u
#define KB_MOD_ALT    0x04u
/* The key is one of the 0xE0-prefixed set (arrows, Home/End, Delete, …):
 * `scancode` names it and `ascii` is 0 — it is a key, not a character. */
#define KB_MOD_EXTENDED 0x08u

typedef struct __packed {
    uint8_t scancode;
    char    ascii;
    uint8_t mods;   /* KB_MOD_* */
} kb_event_t;

#ifdef __cplusplus
static_assert(sizeof(kb_event_t) == 3,
              "kb_event_t is a kernel/userspace ABI struct — must stay 3 packed bytes");
#else
_Static_assert(sizeof(kb_event_t) == 3,
               "kb_event_t is a kernel/userspace ABI struct — must stay 3 packed bytes");
#endif

/*
 * Set-1 scancodes of the 0xE0-prefixed keys — the ones that arrive with
 * KB_MOD_EXTENDED set and ascii 0, because they are keys rather than
 * characters. Named here rather than in each program that answers them: the
 * line editor, a full-screen editor and a menu all decode the same seven
 * bytes, and a private copy of a number is how two of them come to disagree.
 *
 * F1-F12 and the numpad are deliberately absent: the driver's translation
 * table stops at 0x3A and publishes nothing for a key it cannot name, so
 * there is no event to give a name to (src/kernel/drivers/keyboard). The
 * kernel names them too: a terminal on the serial line spells them as
 * escape sequences, and keyboard_inject turns those into these keys.
 */
#define KEY_HOME    0x47
#define KEY_UP      0x48
#define KEY_PAGEUP  0x49
#define KEY_LEFT    0x4B
#define KEY_RIGHT   0x4D
#define KEY_END     0x4F
#define KEY_DOWN    0x50
#define KEY_PAGEDN  0x51
#define KEY_INSERT  0x52
#define KEY_DELETE  0x53

/* The characters that are keys as much as they are bytes. ESC arrives as a
 * character (ascii 27) with no EXTENDED bit, and Ctrl is a MODIFIER on the
 * letter — Ctrl+S is {ascii 's', mods KB_MOD_CTRL}, never byte 0x13. */
#define KEY_ESCAPE     27
#define KEY_BACKSPACE  '\b'
#define KEY_ENTER      '\n'
#define KEY_TAB        '\t'

#endif /* BOXOS_KB_EVENT_H */
