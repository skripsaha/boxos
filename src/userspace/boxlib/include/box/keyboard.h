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

#endif /* BOX_KEYBOARD_H */
