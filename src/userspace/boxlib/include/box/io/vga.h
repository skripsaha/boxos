#ifndef BOX_IO_VGA_H
#define BOX_IO_VGA_H

#include "../types.h"
#include "../error.h"

#define VGA_OP_PUTCHAR         0x70
#define VGA_OP_PUTSTRING       0x71
#define VGA_OP_CLEAR_SCREEN    0x72
#define VGA_OP_CLEAR_LINE      0x73
#define VGA_OP_CLEAR_TO_EOL    0x74
#define VGA_OP_GET_CURSOR      0x75
#define VGA_OP_SET_CURSOR      0x76
#define VGA_OP_SET_COLOR       0x77
#define VGA_OP_GET_COLOR       0x78
#define VGA_OP_SCROLL_UP       0x79
#define VGA_OP_NEWLINE         0x7A
#define VGA_OP_GET_DIMENSIONS  0x7B

#define VGA_BLACK           0x00
#define VGA_BLUE            0x01
#define VGA_GREEN           0x02
#define VGA_CYAN            0x03
#define VGA_RED             0x04
#define VGA_MAGENTA         0x05
#define VGA_BROWN           0x06
#define VGA_LIGHT_GRAY      0x07
#define VGA_DARK_GRAY       0x08
#define VGA_LIGHT_BLUE      0x09
#define VGA_LIGHT_GREEN     0x0A
#define VGA_LIGHT_CYAN      0x0B
#define VGA_LIGHT_RED       0x0C
#define VGA_LIGHT_MAGENTA   0x0D
#define VGA_YELLOW          0x0E
#define VGA_WHITE           0x0F

#define VGA_COLOR(fg, bg)   (((bg) << 4) | (fg))
#define VGA_FG(color)       ((color) & 0x0F)
#define VGA_BG(color)       (((color) >> 4) & 0x0F)

#define VGA_SCHEME_DEFAULT       VGA_COLOR(VGA_LIGHT_GRAY, VGA_BLACK)
#define VGA_SCHEME_GRAY_ON_BLACK VGA_COLOR(VGA_LIGHT_GRAY, VGA_BLACK)
#define VGA_SCHEME_WHITE_ON_BLUE VGA_COLOR(VGA_WHITE, VGA_BLUE)
#define VGA_SCHEME_BLACK_ON_GRAY VGA_COLOR(VGA_BLACK, VGA_LIGHT_GRAY)
#define VGA_SCHEME_GREEN_ON_BLACK VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK)

#define VGA_ERR_OUT_OF_BOUNDS    ERR_VGA_ERROR
#define VGA_ERR_INVALID_CURSOR   ERR_VGA_ERROR
#define VGA_ERR_ACCESS_DENIED    ERR_ACCESS_DENIED
#define VGA_ERR_STRING_TOO_LONG  ERR_BUFFER_TOO_SMALL

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

INLINE int vga_map_error(int32_t kernel_error) {
    switch (kernel_error) {
        case 0:  return 0;
        case -1: return -ERR_INVALID_ARGUMENT;
        case -2: return -ERR_VGA_ERROR;
        case -3: return -ERR_VGA_ERROR;
        case -4: return -ERR_ACCESS_DENIED;
        case -5: return -ERR_BUFFER_TOO_SMALL;
        default: return -ERR_INTERNAL;
    }
}

#endif // BOX_IO_VGA_H
