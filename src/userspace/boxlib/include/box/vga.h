#ifndef BOX_VGA_H
#define BOX_VGA_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "box/error.h"
#include "video_colors.h"

typedef struct {
    uint8_t row;
    uint8_t col;
} vga_pos_t;

typedef struct {
    uint8_t rows;
    uint8_t cols;
} vga_dimensions_t;

int vga_putchar(char c);
int vga_puts(const char* str);

int vga_clear(uint8_t color);
int vga_clear_line(uint8_t row, uint8_t color);
int vga_clear_to_eol(uint8_t color);
int vga_scroll_up(uint8_t lines, uint8_t fill_color);
int vga_newline(void);

int vga_getcursor(vga_pos_t* pos);
int vga_setcursor(uint8_t row, uint8_t col);

int vga_getcolor(void);
int vga_setcolor(uint8_t color);

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
 * flush when the outermost one returns. Getter ops (vga_getcolor,
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
