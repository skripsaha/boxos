#ifndef EMERGENCY_SCREEN_H
#define EMERGENCY_SCREEN_H

#include "ktypes.h"

/*
 * Emergency Screen — the only drawing the kernel does.
 *
 * The screen belongs to the program that claims it (screen.h). The kernel
 * draws only when nobody does: from the first line of the boot until the
 * display program claims the screen, after the holder gives it back or dies,
 * while Ctrl+Alt+Esc has it, and in a panic. What it draws is the tail of the
 * log ring — it is a second reader of the ring, beside the serial Wire, with a
 * position of its own, and it puts the ring's bytes straight into video memory
 * through the kernel's own mapping of the glass. No shadow, no batches, and
 * never a read of video memory: a scroll is a fresh draw of the ring's tail.
 *
 * Every call but PanicTake and Redraw is made holding the console lock
 * (console_lock_acquire), which is also what kprintf holds when it calls
 * Follow; PanicTake and Redraw are the panic owner's, which takes every lock
 * as it stands (klib_lock.c).
 */

/* The glass to draw on: the legacy 80x25 text page, or a linear framebuffer
 * through `pixels`, the kernel's write-combining mapping of it. */
void EmergencyScreenUseText(uint32_t cols, uint32_t rows);
void EmergencyScreenUsePixels(volatile uint8_t *pixels, uint32_t width,
                              uint32_t height, uint32_t stride, uint32_t format);

/* The smallest cell a framebuffer is drawn in — what one must hold to be
 * drawn on at all — and the cells the one in use is drawn in: their size and
 * how many go across and down. */
void EmergencyScreenLeastCell(uint32_t *width, uint32_t *height);
void EmergencyScreenCells(uint32_t *cols, uint32_t *rows, uint32_t *cell_width, uint32_t *cell_height);

/* A quiet boot: nothing is drawn — the glass keeps what the firmware left on
 * it, its logo on a desk machine — until the display program claims it, and
 * the tail is shown at once instead when the ring says anything in the error
 * tone (Follow looks), on a panic, or when the screen is shown outright
 * (screen.c, ScreenQuietEnds). Never a timer: only something said. */
void EmergencyScreenAwait(void);

/* Take the glass: clear it and draw the ring's tail; from now on Follow draws. */
void EmergencyScreenShow(void);

/* Give the glass up: from now on Follow draws nothing. */
void EmergencyScreenHide(void);

/* Draw what the ring has said since the last draw, if the glass is ours. */
void EmergencyScreenFollow(void);

/* A panic's: the glass taken whoever holds the screen, the tail drawn, and
 * every bit of state a core stopped mid-draw may have left reset. */
void EmergencyScreenPanicTake(void);

/* A panic's, at the end of its account: the whole tail drawn once more, over
 * anything a stopped core's write-combining buffers let go of late. */
void EmergencyScreenRedraw(void);

#endif /* EMERGENCY_SCREEN_H */
