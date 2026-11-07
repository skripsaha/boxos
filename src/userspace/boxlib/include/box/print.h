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

/* With `cmd -> file` (box/form.h) everything a program prints — print, println,
 * print_bytes, printf, and std::cout above them — is added to the end of that
 * file, colours dropped, and none of it reaches the screen. It is gathered and
 * added a gathering at a time; io_flush() adds what is gathered now, and exit()
 * does before the program ends. */
void print(const char* str);
void println(const char* str);

/* Write exactly `len` bytes to the console (not NUL-terminated). Same
 * VGA/IPC routing, colour state and UTF-8->'?' filtering as print(); the
 * byte-count form std::print and other length-carrying writers need. */
void print_bytes(const char* data, size_t len);

/* The same two, to the person's screen whatever `->` says. What a program
 * prints goes where the arrow points; its prompt, the editor's echo and the
 * words it says to the person do not. Without an arrow they are print and
 * print_bytes. */
void print_to_screen(const char* str);
void print_bytes_to_screen(const char* data, size_t len);

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
 * UTF-8 input: passes through untouched, every byte of it. A printer does
 * not decide what a character looks like — the code that owns the font does,
 * at the moment it draws (boxos_glyph_fold.h, in the display program and the
 * kernel's Emergency Screen), where a
 * codepoint with no glyph becomes a visible box and nothing is lost on the
 * way to the log. Replacing sequences here used to cost the character in the
 * log file as well, because it happened in this cabin, before the syscall.
 *
 * The whole printf produces ONE syscall — output runs are batched as ops
 * inside a single Manifest submit.
 */
int printf(const char* fmt, ...);

/* C's snprintf and vsnprintf: printf's conversions written into str instead
 * of to the screen — at most n - 1 bytes of the text and a NUL after them,
 * nothing at all when n is 0 (and then str may be NULL) — returning the
 * length of the whole text, or -1. A buffer has no colour: %color is C's %c
 * there. A C++ program's <cstdio> names these same two functions. */
int snprintf(char* str, size_t n, const char* fmt, ...);
int vsnprintf(char* str, size_t n, const char* fmt, va_list args);

/* Input — the line editor (readline.c), event-driven over the console's ear. */
int readline(char* buffer, size_t max_len);

/* The same editor over a line of its own that grows as it is typed, with no
 * ceiling but memory: its length, and *line — NUL-terminated, the caller's to
 * free — or -1 with *line NULL when the ear is gone or no memory was had. */
int readline_grow(char** line);

/* A Whisper: what a line could go on to be, laid dim past the caret while it
 * is typed. The one who reads the line knows its lines; the editor shows what
 * it is told and takes it when it is asked to:
 *
 *   Tab          the next word of what is whispered, as if it were typed
 *   Shift+Tab    all of it
 *   Tab, with nothing whispered — what the line still needs, laid dim and
 *                never typed; the next key takes it away
 *
 * Something is whispered only while the caret is at the end of a line that has
 * something on it, and asked again after every key. Every answer is a heap
 * string the editor frees, NULL for nothing, and only what the editor could
 * have typed itself is shown. The whisperer is handed back to itself, so a
 * caller keeps what it needs in a structure that begins with one of these, as
 * BoxFormLookup is kept. */
typedef struct LineWhisper LineWhisper;
struct LineWhisper {
    /* What may follow `line` — the characters that would come next. */
    char *(*Continue)(LineWhisper* whisper, const char* line, uint32_t length);
    /* What `line` still needs, said in words: laid when Tab has nothing to take. */
    char *(*Needs)(LineWhisper* whisper, const char* line, uint32_t length);
    /* The whisperer's line `back` lines before its newest (0: the newest);
     * NULL past the oldest. Up and Down walk these. */
    char *(*Recall)(LineWhisper* whisper, uint32_t back);
};

/* readline_grow with a Whisper. Up and Down walk the whisperer's lines rather
 * than the cabin's history, and the line read is not kept in that history:
 * the whisperer keeps what it wants. */
int readline_whispered(char** line, LineWhisper* whisper);

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
