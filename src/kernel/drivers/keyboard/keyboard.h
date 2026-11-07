#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "ktypes.h"
#include "boxos_limits.h"
#include "kernel_config.h"   /* CONFIG_KB_REPEAT_{DELAY,RATE}_MS */

#define KEYBOARD_DATA_PORT   0x60
#define KEYBOARD_STATUS_PORT 0x64

/* Software typematic timing — values live in kernel_config.h so they
 * can be overridden at build (-DCONFIG_KB_REPEAT_DELAY_MS=N). Kept
 * here as the legacy alias names that the driver internals use. */
#define KB_REPEAT_DELAY_MS      CONFIG_KB_REPEAT_DELAY_MS
#define KB_REPEAT_RATE_MS       CONFIG_KB_REPEAT_RATE_MS

/* Runtime values (calculated from ms based on timer frequency) */
extern uint32_t g_kb_repeat_delay_ticks;
extern uint32_t g_kb_repeat_rate_ticks;

/* Modifier/lock fields are touched from more than one core:
 *   - PS/2 IRQ1 — sets shift/ctrl/alt/caps/num/scroll
 *   - the xHCI keyboard path — also sets shift/ctrl/alt
 *   - a user strand on any App-Core — reads via keyboard_get_state /
 *     line_process_char / hw_kb.modifiers
 * A bitfield byte cannot be safely RMW'd from multiple cores, so each field
 * gets its own _Atomic uint8_t storage. Access via __atomic_*.
 *
 * ‼ WHY THE xHCI PATH IS NOT THE BSP'S, WHICH IS NOT WHAT IT LOOKS LIKE.
 * This used to say "xHCI HID IRQ (any LAPIC)", and read as a statement about
 * the INTERRUPT it is false: xhci.c aims MSI-X, MSI and the INTx fallback
 * alike at g_amp.bsp_lapic_id, like every other device in this kernel. The
 * reason it is true anyway is the event ring, which is ALSO drained
 * synchronously by whichever core is waiting on it — eight polling sites do
 * that — and on a machine whose volume is a USB stick, every block read
 * drains it, keyboard reports included. So a keyboard report really can be
 * processed on any core; it just does not arrive there as an interrupt. */
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

/* Feed characters from an external input source (the COM1 serial console)
 * into the keyboard input ring, delivered to the shell as if typed. */
void keyboard_inject(const char *chars, uint32_t count);


keyboard_state_t* keyboard_get_state(void);

/* Called from PIT IRQ0 handler every tick to drive software key repeat */
void keyboard_timer_tick(void);

/* Push a raw byte sequence into the keyboard buffer (used by USB HID path) */

/* Update PS/2 keyboard LEDs (Caps/Num/Scroll Lock) */
void keyboard_set_leds(uint8_t caps, uint8_t num, uint8_t scroll);

#endif /* KEYBOARD_H */
