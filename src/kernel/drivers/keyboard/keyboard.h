#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "ktypes.h"
#include "vga.h"
#include "boxos_limits.h"

#define KEYBOARD_DATA_PORT 0x60
#define KEYBOARD_STATUS_PORT 0x64

// Line buffer configuration
#define KEYBOARD_LINE_BUFFER_SIZE BOXOS_KEYBOARD_LINE_BUFFER_SIZE

// Keyboard state structure
typedef struct {
    uint8_t shift_pressed : 1;
    uint8_t ctrl_pressed : 1;
    uint8_t alt_pressed : 1;
    uint8_t caps_lock : 1;
    uint8_t num_lock : 1;
    uint8_t scroll_lock : 1;
    uint8_t last_keycode;
} keyboard_state_t;

// Line editing state (for shell)
typedef struct {
    char buffer[KEYBOARD_LINE_BUFFER_SIZE];
    uint16_t length;          // Current line length
    uint16_t cursor;          // Cursor position in line
    uint8_t line_ready;       // Line complete (Enter pressed)
    uint8_t echo_enabled;     // Echo chars to screen
    uint8_t ctrl_c_pressed;   // Ctrl+C interrupt signal
} keyboard_line_state_t;

// Initialization
void keyboard_init(void);

// IRQ handler (called from interrupt context)
void keyboard_handle_scancode(uint8_t scancode);

// Basic character API
int keyboard_has_input(void);          // Returns 1 if input available
char keyboard_getchar(void);           // Non-blocking read (returns 0 if no input)
char keyboard_getchar_blocking(void);  // Blocking read (waits for input)
void keyboard_flush(void);             // Clear input buffer
uint32_t keyboard_available(void);     // Returns number of characters in buffer

// Line editing API (for shell)
void keyboard_line_init(void);                    // Initialize line buffer
void keyboard_set_echo(bool enabled);             // Enable/disable echo
char* keyboard_readline(void);                    // Read full line (blocking, returns on Enter)
int keyboard_readline_async(char* buf, int max);  // Non-blocking: returns 1 if line ready, 0 otherwise
int keyboard_check_ctrl_c(void);                  // Check and clear Ctrl+C flag
void keyboard_line_clear(void);                   // Clear current line buffer

// Get keyboard state
keyboard_state_t* keyboard_get_state(void);

#endif // KEYBOARD_H