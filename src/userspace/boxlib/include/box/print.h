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

/* Write exactly `len` bytes to the console (not NUL-terminated). Same
 * VGA/IPC routing, colour state and UTF-8->'?' filtering as print(); the
 * byte-count form std::print and other length-carrying writers need. */
void print_bytes(const char* data, size_t len);

void clear(void);
void io_flush(void);

/* printf with BoxOS-native colored runs.
 *
 * Standard format specifiers:  %s %d %u %x %X %c %p %%
 *
 * Extensions: %color   — consumes one Color (#RRGGBB in a uint32_t) and
 *                        switches the foreground for following text runs
 *                        in the same printf call.
 *             %bgcolor — the same for the background. Colour is a full
 *                        24-bit (fg, bg) pair the whole way to the screen;
 *                        the VGA text backend alone projects it onto its
 *                        16 colours, by dominant hue, at draw time.
 *
 * UTF-8 input: ASCII passes through; multi-byte sequences are replaced with
 * '?' until the kernel-side font extension lands. This keeps printf safe for
 * arbitrary user strings without breaking the cell grid.
 *
 * The whole printf produces ONE syscall — output runs are batched as ops
 * inside a single Manifest submit.
 */
int printf(const char* fmt, ...);

/* Input — the line editor (readline.c), event-driven over the console's ear. */
int readline(char* buffer, size_t max_len);
int getchar(void);
int input(const char* prompt, char* buffer, size_t max_len);

/* The tag this strand's keys arrive on. With a display daemon it is the
 * strand's own lane tag and the daemon is told to listen (DISP_CMD_LISTEN);
 * without one it is "keyboard" itself. TOUCH_TAG_INVALID when there is no
 * way to hear at all. */
TouchTag console_listen(void);

/* The reading is over: the ear goes back to the daemon. Whoever reads next
 * hears; a key typed while nobody reads waits at the daemon for that reader. */
void console_unlisten(void);

/* Move the cursor `delta` cells along the console (negative = back), across
 * line ends, erasing nothing — the editor's step inside a line. */
void console_step(int32_t delta);

void print_int(int num);
void print_hex(uint32_t num);

#ifdef __cplusplus
}
#endif

#endif // BOX_PRINT_H
