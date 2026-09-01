#ifndef BOX_VGA_H
#define BOX_VGA_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "box/error.h"
#include "box/color.h"

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

int vga_clear_rgb(Color fg, Color bg);
int vga_clear_line_rgb(uint8_t row, Color fg, Color bg);
int vga_clear_to_eol(void);          /* uses the kernel's current pair */
int vga_scroll_up(void);
int vga_newline(void);

int vga_getcursor(vga_pos_t* pos);
int vga_setcursor(uint8_t row, uint8_t col);

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
