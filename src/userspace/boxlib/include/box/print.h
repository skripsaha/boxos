#ifndef BOX_PRINT_H
#define BOX_PRINT_H

#include "box/defs.h"
#include "box/color.h"

#define IO_MODE_VGA 0
#define IO_MODE_IPC 1

void io_set_mode(uint8_t mode);
uint8_t io_get_mode(void);

void io_set_display_pid(uint32_t pid);
uint32_t io_get_display_pid(void);

void print(const char* str);
void println(const char* str);
void clear(void);
void io_flush(void);

/* printf with BoxOS-native colored runs.
 *
 * Standard format specifiers:  %s %d %u %x %X %c %p %%
 *
 * Extension:  %color   — consumes one Color (uint32_t RGB) argument and
 *                        switches the foreground colour for following text
 *                        runs in the same printf call.
 *
 * UTF-8 input: ASCII passes through; multi-byte sequences are replaced with
 * '?' until the kernel-side font extension lands. This keeps printf safe for
 * arbitrary user strings without breaking the cell grid.
 *
 * The whole printf produces ONE syscall — output runs are batched as ops
 * inside a single Manifest submit.
 */
int printf(const char* fmt, ...);

int readline(char* buffer, size_t max_len);
int getchar(void);
int input(const char* prompt, char* buffer, size_t max_len);

void print_int(int num);
void print_hex(uint32_t num);

#endif // BOX_PRINT_H
