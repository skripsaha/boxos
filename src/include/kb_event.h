#ifndef BOXOS_KB_EVENT_H
#define BOXOS_KB_EVENT_H


#ifndef __packed
#define __packed __attribute__((packed))
#endif

#define KB_MOD_SHIFT  0x01u
#define KB_MOD_CTRL   0x02u
#define KB_MOD_ALT    0x04u
#define KB_MOD_EXTENDED 0x08u

typedef struct __packed {
    uint8_t scancode;
    char    ascii;
    uint8_t mods;
} kb_event_t;

#ifdef __cplusplus
static_assert(sizeof(kb_event_t) == 3,
              "kb_event_t is a kernel/userspace ABI struct — must stay 3 packed bytes");
#else
_Static_assert(sizeof(kb_event_t) == 3,
               "kb_event_t is a kernel/userspace ABI struct — must stay 3 packed bytes");
#endif

#endif