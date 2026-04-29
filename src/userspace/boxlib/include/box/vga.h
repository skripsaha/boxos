#ifndef BOX_VGA_H
#define BOX_VGA_H

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
 * Currently no-op; Stage 3 will wire to a Manifest-batch builder so a
 * single printf with %color produces one syscall for all fragments.
 * ========================================================================= */
void vga_begin(void);
int  vga_commit(void);

#endif // BOX_VGA_H
