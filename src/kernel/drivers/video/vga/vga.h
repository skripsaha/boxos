#ifndef VGA_H
#define VGA_H

#include "ktypes.h"

#define VGA_TEXT_BUFFER_ADDR  0xB8000
#define VGA                   VGA_TEXT_BUFFER_ADDR

extern unsigned char *vga;

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define BYTES_FOR_EACH_ELEMENT 2
#define VGA_SIZE (VGA_WIDTH * VGA_HEIGHT * BYTES_FOR_EACH_ELEMENT)

// Color attribute format: (background << 4) | foreground
#define VGA_BLACK         0x0
#define VGA_BLUE          0x1
#define VGA_GREEN         0x2
#define VGA_CYAN          0x3
#define VGA_RED           0x4
#define VGA_MAGENTA       0x5
#define VGA_BROWN         0x6
#define VGA_LIGHT_GRAY    0x7
#define VGA_DARK_GRAY     0x8
#define VGA_LIGHT_BLUE    0x9
#define VGA_LIGHT_GREEN   0xA
#define VGA_LIGHT_CYAN    0xB
#define VGA_LIGHT_RED     0xC
#define VGA_LIGHT_MAGENTA 0xD
#define VGA_YELLOW        0xE
#define VGA_WHITE         0xF

#define VGA_COLOR(fg, bg) (((bg) << 4) | (fg))

#define GRAY_ON_BLACK       VGA_COLOR(VGA_LIGHT_GRAY, VGA_BLACK)
#define WHITE_ON_BLACK      VGA_COLOR(VGA_WHITE, VGA_BLACK)
#define BLACK_ON_WHITE      VGA_COLOR(VGA_BLACK, VGA_WHITE)
#define BLACK_ON_GRAY       VGA_COLOR(VGA_BLACK, VGA_LIGHT_GRAY)

#define RED_ON_BLACK        VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK)
#define GREEN_ON_BLACK      VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK)
#define YELLOW_ON_BLACK     VGA_COLOR(VGA_YELLOW, VGA_BLACK)
#define CYAN_ON_BLACK       VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK)
#define BLUE_ON_BLACK       VGA_COLOR(VGA_LIGHT_BLUE, VGA_BLACK)
#define MAGENTA_ON_BLACK    VGA_COLOR(VGA_LIGHT_MAGENTA, VGA_BLACK)

#define WHITE_ON_BLUE       VGA_COLOR(VGA_WHITE, VGA_BLUE)
#define WHITE_ON_RED        VGA_COLOR(VGA_WHITE, VGA_RED)
#define WHITE_ON_GREEN      VGA_COLOR(VGA_WHITE, VGA_GREEN)
#define BLACK_ON_CYAN       VGA_COLOR(VGA_BLACK, VGA_CYAN)
#define BLACK_ON_YELLOW     VGA_COLOR(VGA_BLACK, VGA_YELLOW)

#define PROMPT_COLOR        CYAN_ON_BLACK
#define CONTEXT_COLOR       YELLOW_ON_BLACK
#define INPUT_COLOR         WHITE_ON_BLACK
#define OUTPUT_COLOR        GRAY_ON_BLACK
#define ERROR_COLOR         RED_ON_BLACK
#define SUCCESS_COLOR       GREEN_ON_BLACK
#define WARNING_COLOR       YELLOW_ON_BLACK
#define HINT_COLOR          CYAN_ON_BLACK
#define FILENAME_COLOR      WHITE_ON_BLACK
#define TAG_COLOR           MAGENTA_ON_BLACK

#define TEXT_ATTR_DEFAULT   GRAY_ON_BLACK
#define TEXT_ATTR_CURSOR    BLUE_ON_BLACK
#define TEXT_ATTR_ERROR     RED_ON_BLACK
#define TEXT_ATTR_WARNING   YELLOW_ON_BLACK
#define TEXT_ATTR_HINT      CYAN_ON_BLACK
#define TEXT_ATTR_SUCCESS   GREEN_ON_BLACK

void vga_init(void);

void vga_print(const char *str);
void vga_print_char(char ch, const unsigned char attr);
void vga_print_newline(void);
void vga_clear_screen(void);
void vga_clear_line(int line);
void vga_clear_to_eol();
void vga_print_error(const char *str);
void vga_print_success(const char *str);
void vga_print_hint(const char *str);
void vga_scroll_up(void);
void vga_change_background(unsigned char attr);

void vga_update_cursor(void);
void vga_set_cursor_position(int x, int y);
int vga_get_cursor_position_x();
int vga_get_cursor_position_y();

extern uint8_t vga_current_color;
void vga_set_color(uint8_t color);
uint8_t vga_get_color(void);
void vga_reset_color(void);

#endif // VGA_H
