#ifndef BOX_KEYBOARD_H
#define BOX_KEYBOARD_H

/*
 * The keyboard is EVENTS: the kernel publishes every key as a Touch on the
 * "keyboard" tag, payload kb_event_t — scancode, ascii (0 for a key that is
 * not a character), modifiers (KB_MOD_*, KB_MOD_EXTENDED for the 0xE0 set:
 * arrows, Home/End, Delete). There is no ring to poll and no call that
 * returns "no data": a program hears its keys through the console's ear
 * (readline / getchar / console_listen in box/print.h) or by claiming the
 * tag itself. This header names the payload.
 */

#include "box/types.h"
#include "kb_event.h"

/*
 * Set-1 scancodes of the 0xE0-prefixed keys — the ones that arrive with
 * KB_MOD_EXTENDED set and ascii 0, because they are keys rather than
 * characters. Named here rather than in each program that answers them: the
 * line editor, a full-screen editor and a menu all decode the same seven
 * bytes, and a private copy of a number is how two of them come to disagree.
 *
 * F1-F12 and the numpad are deliberately absent: the driver's translation
 * table stops at 0x3A and publishes nothing for a key it cannot name, so
 * there is no event to give a name to (src/kernel/drivers/keyboard).
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

#endif /* BOX_KEYBOARD_H */
