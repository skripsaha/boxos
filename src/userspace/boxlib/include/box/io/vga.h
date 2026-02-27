#ifndef BOX_IO_VGA_H
#define BOX_IO_VGA_H

#include "../types.h"
#include "../error.h"

// Hardware Deck VGA Opcodes (0x70-0x7B)
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

// VGA Color Definitions
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

// Color Scheme Helpers
#define VGA_COLOR(fg, bg)   (((bg) << 4) | (fg))
#define VGA_FG(color)       ((color) & 0x0F)
#define VGA_BG(color)       (((color) >> 4) & 0x0F)

// Predefined Color Schemes
#define VGA_SCHEME_DEFAULT       VGA_COLOR(VGA_LIGHT_GRAY, VGA_BLACK)
#define VGA_SCHEME_GRAY_ON_BLACK VGA_COLOR(VGA_LIGHT_GRAY, VGA_BLACK)
#define VGA_SCHEME_WHITE_ON_BLUE VGA_COLOR(VGA_WHITE, VGA_BLUE)
#define VGA_SCHEME_BLACK_ON_GRAY VGA_COLOR(VGA_BLACK, VGA_LIGHT_GRAY)
#define VGA_SCHEME_GREEN_ON_BLACK VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK)

// Error codes
#define VGA_ERR_OUT_OF_BOUNDS    BOX_ERR_VGA_ERROR
#define VGA_ERR_INVALID_CURSOR   BOX_ERR_VGA_ERROR
#define VGA_ERR_ACCESS_DENIED    BOX_ERR_ACCESS_DENIED
#define VGA_ERR_STRING_TOO_LONG  BOX_ERR_BUFFER_TOO_SMALL

// Position structure
typedef struct {
    uint8_t row;
    uint8_t col;
} box_vga_pos_t;

// Dimensions structure
typedef struct {
    uint8_t rows;
    uint8_t cols;
} box_vga_dimensions_t;

// Core Character Output
int vga_putchar(char c);
int vga_puts(const char* str);

// Screen Management
int vga_clear(uint8_t color);
int vga_clear_line(uint8_t row, uint8_t color);
int vga_clear_to_eol(uint8_t color);
int vga_scroll_up(uint8_t lines, uint8_t fill_color);
int vga_newline(void);

// Cursor Management
int vga_getcursor(box_vga_pos_t* pos);
int vga_setcursor(uint8_t row, uint8_t col);

// Color Management
int vga_getcolor(void);
int vga_setcolor(uint8_t color);

// Screen Info
int vga_getdimensions(box_vga_dimensions_t* dims);

// Helper: Map kernel error codes to BoxLib error codes
BOX_INLINE int vga_map_error(int32_t kernel_error) {
    switch (kernel_error) {
        case 0:  return 0;
        case -1: return -BOX_ERR_INVALID_ARGUMENT;
        case -2: return -BOX_ERR_VGA_ERROR;
        case -3: return -BOX_ERR_VGA_ERROR;
        case -4: return -BOX_ERR_ACCESS_DENIED;
        case -5: return -BOX_ERR_BUFFER_TOO_SMALL;
        default: return -BOX_ERR_INTERNAL;
    }
}

#endif // BOX_IO_VGA_H
