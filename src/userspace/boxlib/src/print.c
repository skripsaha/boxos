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

/* Last 8-bit VGA attribute we sent to the kernel/daemon. Both the explicit
 * set_color() path and the printf %color path consult this cache to skip
 * redundant DISP_CMD_COLOR / vga_setcolor traffic. */
static uint8_t  s_last_attr      = 0;
static bool     s_last_attr_set  = false;

void     io_set_mode(uint8_t mode)
{
    /* Switching between VGA and IPC paths invalidates the kernel/daemon
     * colour state we cached locally; force the next emit to re-send. */
    if (mode != g_io_mode) s_last_attr_set = false;
    g_io_mode = mode;
}
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
        /* Unicast to the resolved display daemon when we know it. Earlier
         * code unconditionally broadcast()'d to the "display" tag; if a
         * caller had — for whatever reason — spawned a redundant display
         * (shell does this when the autostart daemon hasn't reached its
         * receive loop within 500 ms, see 2026-05-14 audit), every render
         * frame hit *both* daemons and the user saw a duplicated screen
         * plus character-by-character interleaving in the serial mirror.
         *
         * Broadcast is now strictly the discovery fallback (no display
         * pid known yet). Once io_set_display_pid() / readline() /
         * getchar() has resolved a daemon, all subsequent traffic flows
         * to that single PID. */
        if (g_display_pid != 0) {
            send(g_display_pid, io_buf, (uint16_t)io_buf_pos);
        } else {
            broadcast("display", io_buf, (uint16_t)io_buf_pos);
        }
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
    if (s_last_attr_set && s_last_attr == attr) return;   /* no-op */

    if (g_io_mode == IO_MODE_IPC) {
        char cmd[2] = { (char)DISP_CMD_COLOR, (char)attr };
        io_buf_append(cmd, 2);
    } else {
        vga_setcolor(attr);
    }
    s_last_attr     = attr;
    s_last_attr_set = true;
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
        if (!s_last_attr_set || s_last_attr != attr) {
            char cmd[2] = { (char)DISP_CMD_COLOR, (char)attr };
            io_buf_append(cmd, 2);
            s_last_attr     = attr;
            s_last_attr_set = true;
        }
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

    /* VGA direct: set colour only when it actually changed; emit text
     * honouring newlines. vga_puts() takes a NUL-terminated string of
     * arbitrary length, but the underlying syscall packs into a kernel
     * buffer; for large segments we stream in 192-byte chunks. */
    if (!s_last_attr_set || s_last_attr != attr) {
        vga_setcolor(attr);
        s_last_attr     = attr;
        s_last_attr_set = true;
    }
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

    /* IO_MODE_IPC already batches into io_buf and flushes via one
     * send/broadcast; IO_MODE_VGA wins a syscall reduction by feeding
     * every internal vga_setcolor/vga_puts/vga_newline into a single
     * Manifest. The nested vga_begin/vga_commit composes with an outer
     * caller that may itself be batching (printf, shell renderer). */
    bool we_began = false;
    if (g_io_mode == IO_MODE_VGA) { vga_begin(); we_began = true; }

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

    if (we_began) vga_commit();
}

void println(const char* str)
{
    if (g_io_mode == IO_MODE_IPC) {
        if (str) print(str);
        io_buf_putc('\n');
        return;
    }
    /* VGA mode: wrap the print + newline in one batch so both fire as a
     * single multi-op syscall. */
    vga_begin();
    if (str) print(str);
    vga_newline();
    vga_commit();
}

void clear(void)
{
    /* Display state resets on clear; invalidate the colour cache so the
     * next coloured run re-sends its attribute. */
    s_last_attr_set = false;

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

    /* See print() for rationale — colored runs in VGA mode coalesce
     * into a single Manifest submit instead of N syscalls. */
    bool we_began = false;
    if (g_io_mode == IO_MODE_VGA) { vga_begin(); we_began = true; }

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

        /* Length modifier — `l` / `ll` / `z`. Determines the size of the
         * va_arg pulled for %d/%u/%x. C99 promotion rules: variadic
         * int8/int16 promote to int, so %hhd/%hd are not needed; long
         * stays long; long long stays long long. `z` matches size_t
         * (== uint64_t in our LP64 userspace).
         *
         * width = 0 → int / unsigned int   (default)
         *         1 → long / unsigned long (also size_t)
         *         2 → long long / unsigned long long
         */
        int  width = 0;
        if (*fmt == 'l') {
            width = 1;
            fmt++;
            if (*fmt == 'l') { width = 2; fmt++; }
        } else if (*fmt == 'z') {
            width = 1;
            fmt++;
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
            case 'd': case 'i': {
                int64_t v;
                if (width == 0)      v = (int64_t)va_arg(args, int);
                else if (width == 1) v = (int64_t)va_arg(args, long);
                else                 v = (int64_t)va_arg(args, long long);
                int64_to_str(v, numbuf, sizeof(numbuf));
                ENSURE(24);
                pos = append_str(buf, pos, PRINTF_BUFLEN - 4, numbuf);
                break;
            }
            case 'u': {
                uint64_t v;
                if (width == 0)      v = (uint64_t)va_arg(args, unsigned int);
                else if (width == 1) v = (uint64_t)va_arg(args, unsigned long);
                else                 v = (uint64_t)va_arg(args, unsigned long long);
                uint64_to_str(v, numbuf, sizeof(numbuf));
                ENSURE(24);
                pos = append_str(buf, pos, PRINTF_BUFLEN - 4, numbuf);
                break;
            }
            case 'x': case 'X': {
                uint64_t v;
                if (width == 0)      v = (uint64_t)va_arg(args, unsigned int);
                else if (width == 1) v = (uint64_t)va_arg(args, unsigned long);
                else                 v = (uint64_t)va_arg(args, unsigned long long);
                /* uint64_to_hex emits a "0x" prefix; printf %x in C is
                 * "no prefix", so write the bare hex digits here. */
                if (v == 0) {
                    ENSURE(2);
                    buf[pos++] = '0';
                } else {
                    char hex_tmp[24];
                    fmt_htoa64((unsigned long)v, hex_tmp);
                    ENSURE(24);
                    pos = append_str(buf, pos, PRINTF_BUFLEN - 4, hex_tmp);
                }
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

    if (we_began) vga_commit();

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

        /* Block indefinitely. readline is conceptually a blocking primitive
         * — a finite timeout here was a leftover defence from earlier IPC
         * race investigations.  With reliable cross-core delivery (K-Core
         * fix 2026-05-03), waking on the user's first keypress is the only
         * correct exit; timing out and re-prompting created phantom prompts
         * + stash-poisoned input where the line typed at one prompt would
         * appear at the next. */
        Result result;
        if (!receive_wait(&result, 0))                            return -1;
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

        /* Same rationale as readline above: block until display delivers a
         * keypress.  No timeout — getchar is blocking by definition. */
        Result result;
        if (!receive_wait(&result, 0))                            return -1;
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
