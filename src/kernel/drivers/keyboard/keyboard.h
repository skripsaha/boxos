#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "ktypes.h"
#include "vga.h"
#include "boxos_limits.h"

#define KEYBOARD_DATA_PORT   0x60
#define KEYBOARD_STATUS_PORT 0x64

/* Software typematic timing (PIT = 100 Hz, 1 tick = 10 ms) */
#define KB_REPEAT_DELAY_TICKS   50  /* 500 ms initial delay  */
#define KB_REPEAT_RATE_TICKS     3  /* ~33 ms between repeats = ~30 chars/sec */

typedef struct {
    uint8_t shift_pressed  : 1;
    uint8_t ctrl_pressed   : 1;
    uint8_t alt_pressed    : 1;
    uint8_t caps_lock      : 1;
    uint8_t num_lock       : 1;
    uint8_t scroll_lock    : 1;
    uint8_t last_keycode;
} keyboard_state_t;

typedef struct {
    char     buffer[KEYBOARD_LINE_BUFFER_SIZE];
    uint16_t length;
    uint16_t cursor;
    uint8_t  line_ready;
    uint8_t  echo_enabled;
    uint8_t  ctrl_c_pressed;
} keyboard_line_state_t;

void keyboard_init(void);
void keyboard_handle_scancode(uint8_t scancode);

int      keyboard_has_input(void);
char     keyboard_getchar(void);
char     keyboard_getchar_blocking(void);
void     keyboard_flush(void);
uint32_t keyboard_available(void);

void  keyboard_line_init(void);
void  keyboard_set_echo(bool enabled);
char* keyboard_readline(void);
int   keyboard_readline_async(char* buf, int max);
int   keyboard_check_ctrl_c(void);
void  keyboard_line_clear(void);

keyboard_state_t* keyboard_get_state(void);

/* Called from PIT IRQ0 handler every tick to drive software key repeat */
void keyboard_timer_tick(void);

/* Push a raw byte sequence into the keyboard buffer (used by USB HID path) */
void keyboard_push_sequence(const char* seq, uint8_t len);

/* Update PS/2 keyboard LEDs (Caps/Num/Scroll Lock) */
void keyboard_set_leds(uint8_t caps, uint8_t num, uint8_t scroll);

#endif /* KEYBOARD_H */
