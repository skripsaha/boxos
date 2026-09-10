#ifndef BOX_VGA_H
#define BOX_VGA_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "box/error.h"
#include "box/color.h"
#include "text_cell.h"

typedef struct {
    uint8_t row;
    uint8_t col;
} vga_pos_t;

typedef struct {
    uint8_t rows;
    uint8_t cols;
} vga_dimensions_t;

/*
 * Colours are full #RRGGBB pairs end to end (see box/color.h). Sentinels
 * (COLOR_DEFAULT / COLOR_INHERIT) are resolved to concrete triples here,
 * before the wire — the kernel stores and returns concrete values only.
 */

int vga_putchar(char c);
int vga_puts(const char* str);

/* Paint one cell at an absolute position. Carries its own coordinates and
 * colour pair, touches neither the cursor nor the current colour — the one
 * write that cannot be bent by another writer's cursor movement. */
int vga_putchar_at(uint8_t row, uint8_t col, char ch, Color fg, Color bg);

/* Lay a finished rectangle of cells on the screen: `height` rows of `width`
 * cells, row-major, starting at (row, col). One op, one syscall, one commit
 * — therefore one blit, so a whole frame reaches the glass without a tear
 * down its middle. The cursor does not move and nothing is interpreted: a
 * '\n' in a cell is the glyph, not a line break.
 *
 * This is how a program that owns the screen paints. vga_putchar_at is the
 * one-cell form and costs a syscall's worth of Manifest per cell — and, like
 * every op that puts TEXT on the screen, it also puts that text in the log
 * ring the serial line reads. A painted frame does not go in the ring: a
 * screenful of colour is not something anyone can read back as words, and at
 * thirty frames a second it would bury everything the machine actually said.
 * Say what a painting program has to say with printf.
 *
 * Colours in a painted frame are the one pair boxlib does NOT resolve before
 * the wire — a frame carries thousands of them and walking the caller's buffer
 * to resolve them would mean copying the whole thing. The kernel resolves them
 * as it takes its snapshot (HwVgaPaint), so a sentinel is legal in the buffer
 * handed here and no sentinel ever reaches a cell. Everything else in this
 * header resolves in the caller.
 *
 * A rectangle that does not fit the screen is refused whole
 * (-ERR_OUT_OF_RANGE), never painted in part. Returns 0 or a negative
 * -error_t. Inside a vga_begin/commit batch the pending ops are flushed
 * first, so a paint never overtakes what was said before it. */
int vga_paint(uint8_t row, uint8_t col, uint8_t height, uint8_t width,
              const TextCell *cells);

int vga_clear_rgb(Color fg, Color bg);
int vga_clear_line_rgb(uint8_t row, Color fg, Color bg);
int vga_clear_to_eol(void);          /* uses the kernel's current pair */
int vga_scroll_up(void);
int vga_newline(void);

int vga_getcursor(vga_pos_t* pos);
int vga_setcursor(uint8_t row, uint8_t col);

/* Move the cursor by a signed count of cells, counted linearly across line
 * ends (row * cols + col), and clamped to the screen.
 *
 * This is what a line editor wants and vga_setcursor is not: the editor knows
 * how FAR the caret must move and never where on the screen that is, because
 * the column belongs to the console and everybody writes to it. Read-then-set
 * from out here needs the screen's width — which travels in a byte and is
 * therefore wrong past 255 columns — and leaves a gap in which anything
 * printed by anyone moves the cursor under the arithmetic. The kernel resolves
 * this one against the position itself, in one op, and it batches like every
 * other write. */
int vga_step_cursor(int32_t delta);

int vga_getcolor_rgb(Color* fg, Color* bg);
int vga_setcolor_rgb(Color fg, Color bg);

int vga_getdimensions(vga_dimensions_t* dims);

/* =========================================================================
 * Batch session — multiple VGA operations in one SYSCALL.
 *
 * Between vga_begin() and vga_commit(), every write op (setcolor, puts,
 * putchar, newline, clear, scroll) is appended to a per-process Manifest
 * builder instead of firing immediately. vga_commit() submits the whole
 * batch as a single multi-op Manifest — one kernel re-entry total
 * regardless of how many ops are in the batch.
 *
 * This is the BoxOS Deck-dispatch advantage made concrete: a colored
 * printf that used to cost 10+ syscalls (SET_COLOR / PUTSTRING pairs
 * per colored run) now costs 1.
 *
 * Begin/commit nest correctly: inner vga_begin/vga_commit pairs only
 * flush when the outermost one returns. Getter ops (vga_getcolor_rgb,
 * vga_getcursor, vga_getdimensions) bypass the batch and resolve
 * immediately — they need an answer before the next op decides what to
 * do.
 * ========================================================================= */
void vga_begin(void);
int  vga_commit(void);

#ifdef __cplusplus
}
#endif

#endif // BOX_VGA_H
