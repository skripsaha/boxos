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

/* Modifier bits carried in kb_event_t.mods (and kb_char_t.flags, the ring
 * variant of a key read via HW_KB_GETCHAR). */
#define KB_MOD_SHIFT  0x01u
#define KB_MOD_CTRL   0x02u
#define KB_MOD_ALT    0x04u

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

#endif /* BOXOS_KB_EVENT_H */
