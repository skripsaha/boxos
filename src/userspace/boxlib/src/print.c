#include "box/print.h"
#include "box/color.h"
#include "box/vga.h"
#include "box/keyboard.h"
#include "box/string.h"
#include "box/ipc.h"
#include "box/convert.h"
#include "box/core/notify.h"
#include "box/display.h"

/* ===========================================================================
 * Cabin I/O state
 *
 * Three orthogonal pieces:
 *   1. g_io_mode      — VGA direct vs. display-daemon IPC routing.
 *   2. g_display_pid  — discovered display daemon (on first IPC operation).
 *   3. g_color_fg/bg  — current text colours (24-bit RGB; quantised to VGA
 *                       4-bit on the way to the kernel).
 * =========================================================================== */
static uint8_t  g_io_mode      = IO_MODE_IPC;
static uint32_t g_display_pid  = 0;
static Color    g_color_fg     = COLOR_DEFAULT;
static Color    g_color_bg     = COLOR_BLACK;

void     io_set_mode(uint8_t mode)        { g_io_mode = mode; }
uint8_t  io_get_mode(void)                { return g_io_mode; }
void     io_set_display_pid(uint32_t pid) { g_display_pid = pid; }
uint32_t io_get_display_pid(void)         { return g_display_pid; }

/* ===========================================================================
 * IPC output buffer — coalesces bytes destined for the display daemon so
 * each tiny print() doesn't grab its own ResultRing slot.
 * =========================================================================== */
#define IO_BUF_SIZE 256
static char io_buf[IO_BUF_SIZE];
static int  io_buf_pos = 0;

void io_flush(void)
{
    if (g_io_mode == IO_MODE_IPC && io_buf_pos > 0) {
        broadcast("display", io_buf, (uint16_t)io_buf_pos);
        io_buf_pos = 0;
    }
}

static void io_buf_append(const char* data, size_t len)
{
    while (len > 0) {
        size_t space = (size_t)(IO_BUF_SIZE - io_buf_pos);
        size_t chunk = len < space ? len : space;
        memcpy(io_buf + io_buf_pos, data, chunk);
        io_buf_pos += (int)chunk;
        data += chunk;
        len  -= chunk;
        if (io_buf_pos >= IO_BUF_SIZE) io_flush();
    }
}

static void io_buf_putc(char c)
{
    io_buf[io_buf_pos++] = c;
    if (io_buf_pos >= IO_BUF_SIZE) io_flush();
}

/* ===========================================================================
 * Color state.
 *
 * push_vga_attr() converts the current 24-bit (fg, bg) into the kernel's
 * 8-bit VGA attribute and routes it to whichever backend is active. The
 * full RGB is still useful for printf %color which embeds a per-run colour
 * regardless of the global state.
 * =========================================================================== */
static void push_vga_attr(void)
{
    uint8_t attr = color_to_vga_attr(g_color_fg, g_color_bg);
    if (g_io_mode == IO_MODE_IPC) {
        char cmd[2] = { (char)DISP_CMD_COLOR, (char)attr };
        io_buf_append(cmd, 2);
        return;
    }
    vga_setcolor(attr);
}

void  set_color(Color fg)    { g_color_fg = fg; push_vga_attr(); }
Color get_color(void)        { return g_color_fg; }
void  set_color_bg(Color bg) { g_color_bg = bg; push_vga_attr(); }
Color get_color_bg(void)     { return g_color_bg; }

/* ===========================================================================
 * Low-level emit — push a chunk of ASCII bytes with a given Color. Honours
 * g_io_mode: VGA direct path uses vga_puts; IPC path embeds a DISP_CMD_COLOR
 * marker plus raw text into the io_buf.
 * =========================================================================== */
static void emit_run(const char *bytes, int len, Color fg, Color bg)
{
    if (len <= 0) return;

    uint8_t attr = color_to_vga_attr(fg, bg);

    if (g_io_mode == IO_MODE_IPC) {
        char cmd[2] = { (char)DISP_CMD_COLOR, (char)attr };
        io_buf_append(cmd, 2);
        /* Split on '\n' so the daemon's renderer keeps newline semantics. */
        int seg_start = 0;
        for (int i = 0; i <= len; i++) {
            int at_end = (i == len);
            int is_nl  = !at_end && bytes[i] == '\n';
            if (at_end || is_nl) {
                if (i > seg_start) io_buf_append(bytes + seg_start, (size_t)(i - seg_start));
                if (is_nl) io_buf_putc('\n');
                seg_start = i + 1;
            }
        }
        return;
    }

    /* VGA direct: set colour once, emit text honouring newlines.
     *
     * vga_puts() takes a NUL-terminated string of arbitrary length, but the
     * underlying syscall packs into a kernel buffer; for large segments we
     * stream in 192-byte chunks. The loop has no upper bound on `len` — it
     * iterates until the entire run is delivered. */
    vga_setcolor(attr);
    char tmp[192];
    int  seg_start = 0;
    for (int i = 0; i <= len; i++) {
        int at_end = (i == len);
        int is_nl  = !at_end && bytes[i] == '\n';
        if (at_end || is_nl) {
            int seg_len = i - seg_start;
            int off     = 0;
            while (off < seg_len) {
                int copy = seg_len - off;
                if (copy > (int)sizeof(tmp) - 1) copy = (int)sizeof(tmp) - 1;
                memcpy(tmp, bytes + seg_start + off, (size_t)copy);
                tmp[copy] = '\0';
                vga_puts(tmp);
                off += copy;
            }
            if (is_nl) vga_newline();
            seg_start = i + 1;
        }
    }
}

/* ===========================================================================
 * Public output API
 *
 * print() streams the input in fixed-size chunks: each chunk is UTF-8
 * filtered into a stack scratch buffer, then handed to emit_run. There is no
 * upper bound on input length — the loop keeps consuming until the source
 * NUL. The chunk size is purely an internal staging optimisation.
 * =========================================================================== */
void print(const char* str)
{
    if (!str) return;

    char chunk[256];
    while (*str) {
        /* Find how many input bytes we can safely consume so the filtered
         * output stays within `chunk`. Worst case: every byte is multi-byte
         * UTF-8 and emits one '?'. So consume up to sizeof(chunk) source
         * bytes — guarantees output ≤ sizeof(chunk). */
        int src_consumed = 0;
        int chunk_pos    = 0;
        while (chunk_pos < (int)sizeof(chunk) &&
               src_consumed < (int)sizeof(chunk) && str[src_consumed] != '\0') {
            unsigned char b = (unsigned char)str[src_consumed];
            if (b < 0x80) {
                chunk[chunk_pos++] = (char)b;
                src_consumed++;
            } else {
                chunk[chunk_pos++] = '?';
                int seq;
                if (b < 0xC2)      seq = 1;
                else if (b < 0xE0) seq = 2;
                else if (b < 0xF0) seq = 3;
                else if (b < 0xF5) seq = 4;
                else               seq = 1;
                src_consumed++;
                for (int k = 1; k < seq; k++) {
                    if (str[src_consumed] == '\0') break;
                    if (((unsigned char)str[src_consumed] & 0xC0) != 0x80) break;
                    src_consumed++;
                }
            }
        }
        if (chunk_pos == 0) break;
        emit_run(chunk, chunk_pos, g_color_fg, g_color_bg);
        str += src_consumed;
    }
}

void println(const char* str)
{
    if (str) print(str);
    if (g_io_mode == IO_MODE_IPC) {
        io_buf_putc('\n');
        return;
    }
    vga_newline();
}

void clear(void)
{
    if (g_io_mode == IO_MODE_IPC) {
        io_flush();
        uint8_t cmd = DISP_CMD_CLEAR;
        broadcast("display", &cmd, 1);
        return;
    }
    vga_clear(VIDEO_COLOR(VIDEO_LIGHT_GRAY, VIDEO_BLACK));
}

/* ===========================================================================
 * printf — BoxOS extension: %color consumes one Color (uint32_t RGB) and
 * switches the active foreground for following text runs.
 *
 * Implementation walks the format string, accumulates text into a working
 * buffer, and emits a colored run whenever:
 *   - %color is encountered (flush + change colour);
 *   - the buffer is about to overflow;
 *   - format string ends.
 *
 * UTF-8: ASCII pass-through; multi-byte runs collapse to '?' (kernel font
 * extension is a future milestone).
 *
 * One printf currently produces N×emit_run calls (N = colour changes). For
 * VGA direct mode each emit is 1 setcolor + 1 puts syscall; for IPC mode
 * everything is coalesced into io_buf and flushed lazily.
 * =========================================================================== */

#define PRINTF_BUFLEN 512

static void fmt_htoa64(unsigned long v, char *buf)
{
    static const char hex[] = "0123456789abcdef";
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[17];
    int  i = 0;
    while (v > 0) { tmp[i++] = hex[v & 0xF]; v >>= 4; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

static int append_str(char *buf, int pos, int max, const char *s)
{
    while (s && *s && pos < max) buf[pos++] = *s++;
    return pos;
}

int printf(const char *fmt, ...)
{
    if (!fmt) return -1;

    va_list args;
    va_start(args, fmt);

    char buf[PRINTF_BUFLEN];
    int  pos       = 0;
    int  total_out = 0;
    Color cur_fg   = g_color_fg;
    Color cur_bg   = g_color_bg;
    char  numbuf[24];

    /* Local helpers — re-emit and reset the working buffer. */
    #define FLUSH() do {                                    \
        if (pos > 0) {                                      \
            emit_run(buf, pos, cur_fg, cur_bg);             \
            total_out += pos;                               \
            pos = 0;                                        \
        }                                                   \
    } while (0)

    #define ENSURE(n) do {                                  \
        if (pos + (n) > PRINTF_BUFLEN - 4) FLUSH();         \
    } while (0)

    while (*fmt) {
        if (*fmt != '%') {
            ENSURE(4);
            unsigned char b = (unsigned char)*fmt;
            if (b < 0x80) {
                buf[pos++] = (char)b;
                fmt++;
            } else {
                /* UTF-8 multi-byte → single '?'. */
                buf[pos++] = '?';
                int seq;
                if (b < 0xC2)      seq = 1;
                else if (b < 0xE0) seq = 2;
                else if (b < 0xF0) seq = 3;
                else if (b < 0xF5) seq = 4;
                else               seq = 1;
                fmt++;
                for (int k = 1; k < seq; k++) {
                    if (*fmt == '\0') break;
                    if (((unsigned char)*fmt & 0xC0) != 0x80) break;
                    fmt++;
                }
            }
            continue;
        }

        /* %... */
        const char *spec_start = fmt;
        fmt++;
        if (*fmt == '\0') { /* trailing % — emit literal */
            ENSURE(2);
            buf[pos++] = '%';
            break;
        }

        /* %color — extension: switch foreground, no text emitted. */
        if (*fmt == 'c' && fmt[1] == 'o' && fmt[2] == 'l' &&
            fmt[3] == 'o' && fmt[4] == 'r') {
            FLUSH();
            cur_fg = (Color)va_arg(args, uint32_t);
            fmt += 5;
            continue;
        }

        switch (*fmt) {
            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                /* Stream UTF-8 byte-at-a-time, flushing on overflow. */
                while (*s) {
                    ENSURE(4);
                    unsigned char b = (unsigned char)*s;
                    if (b < 0x80) {
                        buf[pos++] = (char)b;
                        s++;
                    } else {
                        buf[pos++] = '?';
                        int seq;
                        if (b < 0xC2)      seq = 1;
                        else if (b < 0xE0) seq = 2;
                        else if (b < 0xF0) seq = 3;
                        else if (b < 0xF5) seq = 4;
                        else               seq = 1;
                        s++;
                        for (int k = 1; k < seq; k++) {
                            if (*s == '\0') break;
                            if (((unsigned char)*s & 0xC0) != 0x80) break;
                            s++;
                        }
                    }
                }
                break;
            }
            case 'd': {
                int v = va_arg(args, int);
                to_str(v, numbuf, sizeof(numbuf));
                ENSURE(16);
                pos = append_str(buf, pos, PRINTF_BUFLEN - 4, numbuf);
                break;
            }
            case 'u': {
                unsigned int v = va_arg(args, unsigned int);
                uint_to_str(v, numbuf, sizeof(numbuf));
                ENSURE(16);
                pos = append_str(buf, pos, PRINTF_BUFLEN - 4, numbuf);
                break;
            }
            case 'x': case 'X': {
                unsigned int v = va_arg(args, unsigned int);
                to_hex(v, numbuf, sizeof(numbuf));
                ENSURE(16);
                pos = append_str(buf, pos, PRINTF_BUFLEN - 4, numbuf);
                break;
            }
            case 'c': {
                char c = (char)va_arg(args, int);
                ENSURE(2);
                buf[pos++] = c;
                break;
            }
            case 'p': {
                unsigned long v = va_arg(args, unsigned long);
                ENSURE(20);
                buf[pos++] = '0';
                buf[pos++] = 'x';
                fmt_htoa64(v, numbuf);
                pos = append_str(buf, pos, PRINTF_BUFLEN - 4, numbuf);
                break;
            }
            case '%': {
                ENSURE(2);
                buf[pos++] = '%';
                break;
            }
            default: {
                /* Unknown spec — copy verbatim from spec_start through *fmt. */
                ENSURE(4);
                buf[pos++] = '%';
                buf[pos++] = *fmt;
                (void)spec_start;
                break;
            }
        }
        fmt++;
    }

    FLUSH();

    #undef FLUSH
    #undef ENSURE

    va_end(args);
    return total_out;
}

/* ===========================================================================
 * Input
 * =========================================================================== */
int readline(char* buffer, size_t max_len)
{
    io_flush();
    if (!buffer || max_len < 2) return -1;

    if (g_io_mode == IO_MODE_IPC && g_display_pid == 0) {
        uint8_t ping = DISP_CMD_PING;
        broadcast("display", &ping, 1);
        Result ping_result;
        if (receive_wait(&ping_result, 2000) && ping_result.sender_pid != 0)
            g_display_pid = ping_result.sender_pid;
    }

    if (g_io_mode == IO_MODE_IPC && g_display_pid != 0) {
        uint16_t capped = (uint16_t)(max_len > 1024 ? 1024 : max_len);
        uint8_t  req[4] = { DISP_CMD_READLINE,
                            (uint8_t)(capped & 0xFF),
                            (uint8_t)(capped >> 8),
                            1 };
        send(g_display_pid, req, 4);

        Result result;
        if (!receive_wait(&result, 60000))                        return -1;
        if (result.error_code != OK)                              return -1;
        if (result.data_addr == 0 || result.data_length < 4)      return -1;

        const uint8_t* resp = (const uint8_t*)(uintptr_t)result.data_addr;
        uint32_t len;
        memcpy(&len, resp, 4);
        if (len > max_len - 1) len = (uint32_t)(max_len - 1);
        memcpy(buffer, resp + 4, len);
        buffer[len] = '\0';
        return (int)len;
    }

    int len = kb_readline(buffer, max_len, true);
    return len < 0 ? -1 : len;
}

int getchar(void)
{
    io_flush();
    if (g_io_mode == IO_MODE_IPC && g_display_pid == 0) {
        uint8_t ping = DISP_CMD_PING;
        broadcast("display", &ping, 1);
        Result ping_result;
        if (receive_wait(&ping_result, 2000) && ping_result.sender_pid != 0)
            g_display_pid = ping_result.sender_pid;
    }

    if (g_io_mode == IO_MODE_IPC && g_display_pid != 0) {
        uint8_t req = DISP_CMD_GETCHAR;
        send(g_display_pid, &req, 1);

        Result result;
        if (!receive_wait(&result, 60000))                        return -1;
        if (result.error_code != OK)                              return -1;
        if (result.data_addr == 0 || result.data_length < 1)      return -1;
        return *(const uint8_t*)(uintptr_t)result.data_addr;
    }

    return kb_getchar();
}

int input(const char* prompt, char* buffer, size_t max_len)
{
    if (prompt) print(prompt);
    return readline(buffer, max_len);
}

/* ===========================================================================
 * Convenience integer printers (kept for source compat with apps).
 * =========================================================================== */
void print_int(int num)
{
    char b[12];
    to_str(num, b, sizeof(b));
    print(b);
}

void print_hex(uint32_t num)
{
    static const char hex[] = "0123456789abcdef";
    char b[11] = "0x00000000";
    for (int i = 9; i >= 2; i--) { b[i] = hex[num & 0xF]; num >>= 4; }
    print(b);
}
