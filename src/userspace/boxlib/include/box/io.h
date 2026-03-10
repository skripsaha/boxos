#ifndef BOX_IO_H
#define BOX_IO_H

#include "box/defs.h"

#define IO_MODE_VGA 0
#define IO_MODE_IPC 1

void io_set_mode(uint8_t mode);
uint8_t io_get_mode(void);

void io_set_display_pid(uint32_t pid);
uint32_t io_get_display_pid(void);

void print(const char* str);
void println(const char* str);
int printf(const char* fmt, ...);
void clear(void);
void color(uint8_t c);
void io_flush(void);

int readline(char* buffer, size_t max_len);
int getchar(void);
int input(const char* prompt, char* buffer, size_t max_len);

void print_int(int num);
void print_hex(uint32_t num);

#define COLOR_BLACK         0x00
#define COLOR_BLUE          0x01
#define COLOR_GREEN         0x02
#define COLOR_CYAN          0x03
#define COLOR_RED           0x04
#define COLOR_MAGENTA       0x05
#define COLOR_BROWN         0x06
#define COLOR_LIGHT_GRAY    0x07
#define COLOR_DARK_GRAY     0x08
#define COLOR_LIGHT_BLUE    0x09
#define COLOR_LIGHT_GREEN   0x0A
#define COLOR_LIGHT_CYAN    0x0B
#define COLOR_LIGHT_RED     0x0C
#define COLOR_LIGHT_MAGENTA 0x0D
#define COLOR_YELLOW        0x0E
#define COLOR_WHITE         0x0F

#define VGA_COLOR(fg, bg) (((bg) << 4) | (fg))

#endif // BOX_IO_H
