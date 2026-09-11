#ifndef BOX_PRINT_H
#define BOX_PRINT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"
#include "box/color.h"
#include "box/touch.h"

#define IO_MODE_VGA 0
#define IO_MODE_IPC 1

void io_set_mode(uint8_t mode);
uint8_t io_get_mode(void);

void io_set_display_pid(uint32_t pid);
uint32_t io_get_display_pid(void);

void print(const char* str);
void println(const char* str);

void print_bytes(const char* data, size_t len);

void clear(void);
void io_flush(void);

int printf(const char* fmt, ...);

int readline(char* buffer, size_t max_len);
int getchar(void);
int input(const char* prompt, char* buffer, size_t max_len);

TouchTag console_listen(void);

void console_unlisten(void);

void console_step(int32_t delta);

void print_int(int num);
void print_hex(uint32_t num);

#ifdef __cplusplus
}
#endif

#endif