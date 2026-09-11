#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "ktypes.h"
#include "video.h"
#include "boxos_limits.h"
#include "kernel_config.h"

#define KEYBOARD_DATA_PORT   0x60
#define KEYBOARD_STATUS_PORT 0x64

#define KB_REPEAT_DELAY_MS      CONFIG_KB_REPEAT_DELAY_MS
#define KB_REPEAT_RATE_MS       CONFIG_KB_REPEAT_RATE_MS

extern uint32_t g_kb_repeat_delay_ticks;
extern uint32_t g_kb_repeat_rate_ticks;

typedef struct {
    _Atomic uint8_t shift_pressed;
    _Atomic uint8_t ctrl_pressed;
    _Atomic uint8_t alt_pressed;
    _Atomic uint8_t caps_lock;
    _Atomic uint8_t num_lock;
    _Atomic uint8_t scroll_lock;
    _Atomic uint8_t last_keycode;
} keyboard_state_t;


void keyboard_init(void);
void keyboard_handle_scancode(uint8_t scancode);

void keyboard_inject(const char *chars, uint32_t count);


keyboard_state_t* keyboard_get_state(void);

void keyboard_timer_tick(void);


void keyboard_set_leds(uint8_t caps, uint8_t num, uint8_t scroll);

#endif