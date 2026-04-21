#ifndef BOX_IO_VGA_H
#define BOX_IO_VGA_H

#include "../types.h"
#include "../error.h"
#include "video_colors.h"

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

/* Backward-compatible aliases for userspace code using VGA_* colour names */
#define VGA_BLACK           VIDEO_BLACK
#define VGA_BLUE            VIDEO_BLUE
#define VGA_GREEN           VIDEO_GREEN
#define VGA_CYAN            VIDEO_CYAN
#define VGA_RED             VIDEO_RED
#define VGA_MAGENTA         VIDEO_MAGENTA
#define VGA_BROWN           VIDEO_BROWN
#define VGA_LIGHT_GRAY      VIDEO_LIGHT_GRAY
#define VGA_DARK_GRAY       VIDEO_DARK_GRAY
#define VGA_LIGHT_BLUE      VIDEO_LIGHT_BLUE
#define VGA_LIGHT_GREEN     VIDEO_LIGHT_GREEN
#define VGA_LIGHT_CYAN      VIDEO_LIGHT_CYAN
#define VGA_LIGHT_RED       VIDEO_LIGHT_RED
#define VGA_LIGHT_MAGENTA   VIDEO_LIGHT_MAGENTA
#define VGA_YELLOW          VIDEO_YELLOW
#define VGA_WHITE           VIDEO_WHITE

#define VGA_COLOR(fg, bg)   VIDEO_COLOR(fg, bg)
#define VGA_FG(color)       VIDEO_FG(color)
#define VGA_BG(color)       VIDEO_BG(color)

#define VGA_SCHEME_DEFAULT        VIDEO_ATTR_DEFAULT
#define VGA_SCHEME_GRAY_ON_BLACK  VIDEO_GRAY_ON_BLACK
#define VGA_SCHEME_WHITE_ON_BLUE  VIDEO_WHITE_ON_BLUE
#define VGA_SCHEME_BLACK_ON_GRAY  VIDEO_BLACK_ON_GRAY
#define VGA_SCHEME_GREEN_ON_BLACK VIDEO_GREEN_ON_BLACK

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

/* =========================================================================
 * Batch session — multiple VGA operations in one SYSCALL.
 *
 *   vga_begin();
 *   vga_setcolor(GREEN);       // queued, no SYSCALL
 *   vga_puts("OK: ");          // queued, no SYSCALL
 *   vga_setcolor(WHITE);       // queued, no SYSCALL
 *   vga_puts(filename);        // queued, no SYSCALL
 *   vga_newline();             // queued, no SYSCALL
 *   vga_commit();              // ONE SYSCALL for all 5 ops
 *
 * Query functions (getcolor, getcursor, getdimensions) always return
 * cached values — they work identically inside and outside batch mode.
 * Auto-commits if batch buffer overflows.
 * ========================================================================= */

void vga_begin(void);
int  vga_commit(void);

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
