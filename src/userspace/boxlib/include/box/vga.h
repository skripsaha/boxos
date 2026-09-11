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


int vga_putchar(char c);
int vga_puts(const char* str);

int vga_putchar_at(uint8_t row, uint8_t col, char ch, Color fg, Color bg);

int vga_paint(uint8_t row, uint8_t col, uint8_t height, uint8_t width,
              const TextCell *cells);

int vga_clear_rgb(Color fg, Color bg);
int vga_clear_line_rgb(uint8_t row, Color fg, Color bg);
int vga_clear_to_eol(void);
int vga_scroll_up(void);
int vga_newline(void);

int vga_getcursor(vga_pos_t* pos);
int vga_setcursor(uint8_t row, uint8_t col);

int vga_step_cursor(int32_t delta);

int vga_getcolor_rgb(Color* fg, Color* bg);
int vga_setcolor_rgb(Color fg, Color bg);

int vga_getdimensions(vga_dimensions_t* dims);

void vga_begin(void);
int  vga_commit(void);

#ifdef __cplusplus
}
#endif

#endif