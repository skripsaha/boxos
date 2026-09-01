#include "box/print.h"
#include "box/color.h"
#include "box/vga.h"
#include "box/keyboard.h"
#include "box/string.h"
#include "box/ipc.h"
#include "box/convert.h"
#include "box/core/notify.h"
#include "box/core/strand_self.h"
#include "box/display.h"

/* ===========================================================================
 * Per-strand print state — thread-confined, NO lock.
 *
 * Every strand (main + spawned) owns its own IPC output buffer, VGA-attr
 * cache and current colours. Concurrent strands calling print/printf never
 * touch each other's state, so the print path itself never takes a lock.
 *
 * That is NOT the same as "safe to call from inside any lock" — send()'s
 * result_wait can redirect a stray KCTX_STORAGE completion into
 * ferry_stash_push, which lazily mallocs a spawned strand's ferry stash
 * (core/result.c). A spawned strand's printf can therefore malloc, so
 * printf-under-heap_lock is only actually safe for a caller whose path has
 * no such allocation — "no malloc on the path" is the real requirement, not
 * "no lock". memory.c's heap_dump_tags calls printf while holding heap_lock;
 * its one caller (memtest.c) runs on the main strand, whose ferry stash is a
 * static ring (no malloc), so today's sole heap_lock caller is safe in
 * practice.
 *
 * Storage: the main strand uses a static instance (g_main_print_state); a
 * spawned strand's instance lives inline in its StrandInfo (print_state[]).
 * The kernel zero-inits that block, so `initialized` starts false and
 * print_state_self() corrects fg/bg to the process defaults on first touch.
 * Per owner decision, a spawned strand's colours always start FRESH
 * (COLOR_DEFAULT / COLOR_BLACK) — no inheritance from whoever spawned it.
 * =========================================================================== */
#define IO_BUF_SIZE 256

typedef struct StrandPrintState {
    char     io_buf[IO_BUF_SIZE];
    uint16_t io_buf_pos;
    uint8_t  last_set;      /* last_fg/last_bg carry a sent pair */
    uint8_t  initialized;
    uint8_t  _pad[4];
    uint32_t last_fg;       /* last pair pushed to the backend (resolved) */
    uint32_t last_bg;
    uint32_t color_fg;      /* current colours (may hold sentinels) */
    uint32_t color_bg;
} StrandPrintState;
_Static_assert(sizeof(StrandPrintState) == STRAND_PRINT_BYTES,
              "StrandPrintState must match strand_info.h STRAND_PRINT_BYTES");

static StrandPrintState g_main_print_state = {
    .color_fg = COLOR_DEFAULT, .color_bg = COLOR_BLACK, .initialized = 1
};

static StrandPrintState *print_state_self(void)
{
    StrandInfo *si = strand_info_or_null();
    if (!si) return &g_main_print_state;

    StrandPrintState *ps = (StrandPrintState *)(void *)si->print_state;
    if (!ps->initialized) {
        ps->color_fg    = COLOR_DEFAULT;
        ps->color_bg    = COLOR_BLACK;
        ps->initialized = 1;
    }
    return ps;
}

/* ===========================================================================
 * Cabin I/O state
 *
 *   1. g_io_mode      — VGA direct vs. display-daemon IPC routing.
 *   2. g_display_pid  — discovered display daemon (on first IPC operation).
 *
 * Both stay PROCESS-GLOBAL (not per-strand): every strand shares one
 * backend and one discovered daemon. g_display_pid's write is hardened
 * below (first-writer-wins CAS) since any strand's readline/getchar can
 * race to discover it; g_io_mode is a single-writer invariant (see
 * io_set_mode).
 * =========================================================================== */
static uint8_t  g_io_mode      = IO_MODE_IPC;
static uint32_t g_display_pid  = 0;

void     io_set_mode(uint8_t mode)
{
    /* Switching between VGA and IPC paths invalidates the kernel/daemon
     * colour state we cached locally; force the next emit to re-send.
     * Single-writer invariant: call before any strand_spawn — no other
     * strand's cache exists yet, so invalidating only the caller's own is
     * sufficient (every strand spawned afterwards starts uninitialised
     * anyway and re-sends its colour on its first emit). */
    if (mode != g_io_mode) print_state_self()->last_set = false;
    g_io_mode = mode;
}
uint8_t  io_get_mode(void)                { return g_io_mode; }
void     io_set_display_pid(uint32_t pid) { g_display_pid = pid; }
uint32_t io_get_display_pid(void)         { return g_display_pid; }

/* ===========================================================================
 * IPC output buffer — coalesces bytes destined for the display daemon so
 * each tiny print() doesn't grab its own ResultRing slot. Per-strand
 * (StrandPrintState.io_buf): each strand flushes only its own bytes.
 * =========================================================================== */

static void io_flush_state(StrandPrintState *ps)
{
    if (g_io_mode == IO_MODE_IPC && ps->io_buf_pos > 0) {
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
         * to that single PID.
         *
         * Best-effort delivery: send/broadcast once and move on. Console
         * output is not a guaranteed-delivery channel — under sustained
         * saturation the display daemon's ResultRing can be full and a
         * batch is dropped rather than retried. Making delivery reliable
         * needs kernel-side IPC work (SysBroadcast full-ring handling,
         * ipc_copy_to_heap reclaim) and is tracked as a separate session. */
        if (g_display_pid != 0) {
            send(g_display_pid, ps->io_buf, ps->io_buf_pos);
        } else {
            broadcast("display", ps->io_buf, ps->io_buf_pos);
        }
        ps->io_buf_pos = 0;
    }
}

void io_flush(void)
{
    io_flush_state(print_state_self());
}

/* Both helpers clamp io_buf_pos BEFORE computing remaining space, so a
 * corrupted or stale io_buf_pos can never drive `space` negative (which,
 * cast to size_t, used to turn into a huge value and memcpy() past the end
 * of io_buf — the underflow that let concurrent printf smash whatever
 * followed io_buf in memory). With per-strand state there is no concurrent
 * writer left to race, but the clamp costs nothing and stays as
 * defense-in-depth. */
static void io_buf_putc(StrandPrintState *ps, char c)
{
    if (ps->io_buf_pos < IO_BUF_SIZE) ps->io_buf[ps->io_buf_pos++] = c;   /* clamp BEFORE write */
    if (ps->io_buf_pos >= IO_BUF_SIZE) io_flush_state(ps);
}

static void io_buf_append(StrandPrintState *ps, const char *data, size_t len)
{
    while (len > 0) {
        if (ps->io_buf_pos >= IO_BUF_SIZE) {
            io_flush_state(ps);
            /* io_flush_state is a no-op outside IPC mode, so io_buf_pos would
             * stay pinned at IO_BUF_SIZE forever — bail instead of spinning. */
            if (ps->io_buf_pos >= IO_BUF_SIZE) return;
            continue;
        }
        size_t space = (size_t)IO_BUF_SIZE - (size_t)ps->io_buf_pos;   /* >0 here, can't underflow */
        size_t chunk = len < space ? len : space;
        memcpy(ps->io_buf + ps->io_buf_pos, data, chunk);
        ps->io_buf_pos = (uint16_t)(ps->io_buf_pos + chunk);
        data += chunk;
        len  -= chunk;
        if (ps->io_buf_pos >= IO_BUF_SIZE) io_flush_state(ps);
    }
}

/* A colour record is parsed as one unit by the display daemon; unlike text
 * it must never straddle a flush boundary. io_buf_append chunks freely, so a
 * record that lands near the end of the buffer would arrive split across two
 * messages — the daemon would eat the command byte, render the colour bytes
 * as text, and a payload byte that happens to equal DISP_CMD_COLOR would
 * spawn a phantom record swallowing eight bytes of real output. Flush first
 * when the record would not fit whole. (The old 2-byte attribute record had
 * the same hazard, only narrower.) */
static void io_buf_put_record(StrandPrintState *ps, const char *rec, size_t len)
{
    if ((size_t)IO_BUF_SIZE - (size_t)ps->io_buf_pos < len) io_flush_state(ps);
    io_buf_append(ps, rec, len);
}

/* ===========================================================================
 * Color state.
 *
 * push_color() resolves the current (fg, bg) — sentinels become concrete
 * triples — and routes the FULL 24-bit pair to whichever backend is
 * active: a 9-byte DISP_CMD_COLOR record on the daemon wire, or a
 * SET_COLOR op with an RGB pair on the direct VGA path. Nothing here
 * rounds; the VGA text backend quantises at draw time inside the kernel.
 * =========================================================================== */
static void push_color(StrandPrintState *ps)
{
    uint32_t fg = BoxColorResolveFg(ps->color_fg);
    uint32_t bg = BoxColorResolveBg(ps->color_bg);
    if (ps->last_set && ps->last_fg == fg && ps->last_bg == bg) return;

    if (g_io_mode == IO_MODE_IPC) {
        char cmd[9];
        cmd[0] = (char)DISP_CMD_COLOR;
        memcpy(&cmd[1], &fg, 4);
        memcpy(&cmd[5], &bg, 4);
        io_buf_put_record(ps, cmd, sizeof(cmd));
    } else {
        vga_setcolor_rgb(fg, bg);
    }
    ps->last_fg  = fg;
    ps->last_bg  = bg;
    ps->last_set = true;
}

void  set_color(Color fg)
{
    StrandPrintState *ps = print_state_self();
    ps->color_fg = fg;
    push_color(ps);
}
Color get_color(void) { return print_state_self()->color_fg; }

void  set_color_bg(Color bg)
{
    StrandPrintState *ps = print_state_self();
    ps->color_bg = bg;
    push_color(ps);
}
Color get_color_bg(void) { return print_state_self()->color_bg; }

/* ===========================================================================
 * Low-level emit — push a chunk of ASCII bytes with a given Color. Honours
 * g_io_mode: VGA direct path uses vga_puts; IPC path embeds a DISP_CMD_COLOR
 * marker plus raw text into ps->io_buf.
 * =========================================================================== */
static void emit_run(StrandPrintState *ps, const char *bytes, int len, Color fg, Color bg)
{
    if (len <= 0) return;

    fg = BoxColorResolveFg(fg);
    bg = BoxColorResolveBg(bg);
    bool changed = !ps->last_set || ps->last_fg != fg || ps->last_bg != bg;

    if (g_io_mode == IO_MODE_IPC) {
        if (changed) {
            char cmd[9];
            cmd[0] = (char)DISP_CMD_COLOR;
            memcpy(&cmd[1], &fg, 4);
            memcpy(&cmd[5], &bg, 4);
            io_buf_put_record(ps, cmd, sizeof(cmd));
            ps->last_fg  = fg;
            ps->last_bg  = bg;
            ps->last_set = true;
        }
        /* Split on '\n' so the daemon's renderer keeps newline semantics. */
        int seg_start = 0;
        for (int i = 0; i <= len; i++) {
            int at_end = (i == len);
            int is_nl  = !at_end && bytes[i] == '\n';
            if (at_end || is_nl) {
                if (i > seg_start) io_buf_append(ps, bytes + seg_start, (size_t)(i - seg_start));
                if (is_nl) io_buf_putc(ps, '\n');
                seg_start = i + 1;
            }
        }
        return;
    }

    /* VGA direct: set colour only when it actually changed; emit text
     * honouring newlines. vga_puts() takes a NUL-terminated string of
     * arbitrary length, but the underlying syscall packs into a kernel
     * buffer; for large segments we stream in 192-byte chunks. */
    if (changed) {
        vga_setcolor_rgb(fg, bg);
        ps->last_fg  = fg;
        ps->last_bg  = bg;
        ps->last_set = true;
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
/* Stream `len` bytes through the UTF-8 filter (ASCII pass-through; each
 * multi-byte sequence collapses to one '?') into fixed-size chunks, handing
 * each chunk to emit_run with the current colours. Shared by print() and
 * print_bytes() so the filter lives in exactly one place. */
static void emit_filtered(StrandPrintState *ps, const char* data, size_t len)
{
    char   chunk[256];
    size_t src = 0;
    while (src < len) {
        int chunk_pos = 0;
        while (chunk_pos < (int)sizeof(chunk) && src < len) {
            unsigned char b = (unsigned char)data[src];
            if (b < 0x80) {
                chunk[chunk_pos++] = (char)b;
                src++;
            } else {
                chunk[chunk_pos++] = '?';
                int seq;
                if (b < 0xC2)      seq = 1;
                else if (b < 0xE0) seq = 2;
                else if (b < 0xF0) seq = 3;
                else if (b < 0xF5) seq = 4;
                else               seq = 1;
                src++;
                for (int k = 1; k < seq && src < len; k++) {
                    if (((unsigned char)data[src] & 0xC0) != 0x80) break;
                    src++;
                }
            }
        }
        if (chunk_pos == 0) break;
        emit_run(ps, chunk, chunk_pos, ps->color_fg, ps->color_bg);
    }
}

void print(const char* str)
{
    if (!str) return;
    StrandPrintState *ps = print_state_self();

    /* IO_MODE_IPC already batches into io_buf and flushes via one
     * send/broadcast; IO_MODE_VGA wins a syscall reduction by feeding
     * every internal vga_setcolor/vga_puts/vga_newline into a single
     * Manifest. The nested vga_begin/vga_commit composes with an outer
     * caller that may itself be batching (printf, shell renderer). */
    bool we_began = false;
    if (g_io_mode == IO_MODE_VGA) { vga_begin(); we_began = true; }

    emit_filtered(ps, str, strlen(str));

    if (we_began) vga_commit();
}

void print_bytes(const char* data, size_t len)
{
    if (!data || len == 0) return;
    StrandPrintState *ps = print_state_self();

    bool we_began = false;
    if (g_io_mode == IO_MODE_VGA) { vga_begin(); we_began = true; }

    emit_filtered(ps, data, len);

    if (we_began) vga_commit();
}

void println(const char* str)
{
    if (g_io_mode == IO_MODE_IPC) {
        StrandPrintState *ps = print_state_self();
        if (str) print(str);
        io_buf_putc(ps, '\n');
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
    StrandPrintState *ps = print_state_self();

    /* Display state resets on clear; invalidate the colour cache so the
     * next coloured run re-sends its pair. */
    ps->last_set = false;

    if (g_io_mode == IO_MODE_IPC) {
        io_flush_state(ps);
        uint8_t cmd = DISP_CMD_CLEAR;
        broadcast("display", &cmd, 1);
        return;
    }
    vga_clear_rgb(COLOR_LIGHT_GRAY, COLOR_BLACK);
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

/* WEAK on purpose. boxlib's printf is BoxOS's coloured-run console printer and
 * converts %s %d %i %u %x %X %c %p %% — the set a C program on this system has
 * always had. boxcxx's <cstdio> defines the full C set, including the floating
 * conversions, and a program that links boxcxx must get THAT one: [cstdio.syn]
 * asks for conversions this function does not have, and two functions cannot
 * share an ELF symbol.
 *
 * Weak rather than moved to its own translation unit because printf lives on
 * this file's statics — g_io_mode, the per-strand colour state, the shared
 * io_buf — and splitting it would mean exposing all three across a boundary to
 * solve a linking question. The link order that makes this work is already
 * stated in apps/Makefile: libboxcxx.a before libbox.a, "so our runtime symbols
 * always win". */
__attribute__((weak)) int printf(const char *fmt, ...)
{
    if (!fmt) return -1;
    StrandPrintState *ps = print_state_self();

    va_list args;
    va_start(args, fmt);

    /* See print() for rationale — colored runs in VGA mode coalesce
     * into a single Manifest submit instead of N syscalls. */
    bool we_began = false;
    if (g_io_mode == IO_MODE_VGA) { vga_begin(); we_began = true; }

    char buf[PRINTF_BUFLEN];
    int  pos       = 0;
    int  total_out = 0;
    Color cur_fg   = ps->color_fg;
    Color cur_bg   = ps->color_bg;
    char  numbuf[24];

    /* Local helpers — re-emit and reset the working buffer. */
    #define FLUSH() do {                                    \
        if (pos > 0) {                                      \
            emit_run(ps, buf, pos, cur_fg, cur_bg);         \
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

        /* %bgcolor — the same for the background: colour is a (fg, bg)
         * pair everywhere, and printf can steer both halves. */
        if (*fmt == 'b' && fmt[1] == 'g' && fmt[2] == 'c' &&
            fmt[3] == 'o' && fmt[4] == 'l' && fmt[5] == 'o' &&
            fmt[6] == 'r') {
            FLUSH();
            cur_bg = (Color)va_arg(args, uint32_t);
            fmt += 7;
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
    StrandPrintState *ps = print_state_self();
    io_flush_state(ps);
    if (!buffer || max_len < 2) return -1;

    if (g_io_mode == IO_MODE_IPC && g_display_pid == 0) {
        uint8_t ping = DISP_CMD_PING;
        broadcast("display", &ping, 1);
        Result ping_result;
        if (receive_wait(&ping_result, 2000) && ping_result.sender_pid != 0)
            __sync_bool_compare_and_swap(&g_display_pid, 0, ping_result.sender_pid);
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
        /* Accept only the display daemon's reply to OUR request.
         *
         * This used to take whatever arrived: the first four bytes of ANY
         * message became the line length and the rest of it became the line.
         * That is exactly the failure shell.c:96-106 describes — a second
         * daemon's PING reply read as a length-prefixed line — and
         * ShellDrainStaleIpc() exists to drain such messages BEFORE they can
         * be misread, not because readline could tell them apart. Filtering by
         * sender closes the class at the point of use. Bounded, so a mailbox
         * someone else keeps filling cannot hold readline here forever. */
        Result result;
        int foreign = 0;
        for (;;) {
            if (!receive_wait(&result, 0))                        return -1;
            if (result.sender_pid == g_display_pid)               break;
            if (++foreign > 64)                                   return -1;
        }
        if (result.error_code != OK)                              return -1;
        if (result.data_addr == 0 || result.data_length < 4)      return -1;

        const uint8_t* resp = (const uint8_t*)(uintptr_t)result.data_addr;
        uint32_t len;
        memcpy(&len, resp, 4);
        /* Bound by what actually ARRIVED as well as by the destination: the
         * length lives in the message body, so a short message carrying a
         * large prefix would otherwise copy from past the received payload. */
        uint32_t avail = (uint32_t)(result.data_length - 4);
        if (len > avail)             len = avail;
        if (len > max_len - 1)       len = (uint32_t)(max_len - 1);
        memcpy(buffer, resp + 4, len);
        buffer[len] = '\0';
        return (int)len;
    }

    int len = kb_readline(buffer, max_len, true);
    return len < 0 ? -1 : len;
}

/* WEAK for the same reason as printf above: boxcxx's getchar reads through the
 * same FILE as fgetc(stdin), so a byte pushed back with ungetc comes back to
 * it. This one cannot see that pushback, which is correct for a C program that
 * has no FILE and wrong for a C++ one that does. */
__attribute__((weak)) int getchar(void)
{
    StrandPrintState *ps = print_state_self();
    io_flush_state(ps);
    if (g_io_mode == IO_MODE_IPC && g_display_pid == 0) {
        uint8_t ping = DISP_CMD_PING;
        broadcast("display", &ping, 1);
        Result ping_result;
        if (receive_wait(&ping_result, 2000) && ping_result.sender_pid != 0)
            __sync_bool_compare_and_swap(&g_display_pid, 0, ping_result.sender_pid);
    }

    if (g_io_mode == IO_MODE_IPC && g_display_pid != 0) {
        uint8_t req = DISP_CMD_GETCHAR;
        send(g_display_pid, &req, 1);

        /* Same rationale as readline above: block until display delivers a
         * keypress.  No timeout — getchar is blocking by definition. */
        /* Same sender filter as readline: a foreign message's first byte
         * would otherwise be handed back as the user's keypress. */
        Result result;
        int foreign = 0;
        for (;;) {
            if (!receive_wait(&result, 0))                        return -1;
            if (result.sender_pid == g_display_pid)               break;
            if (++foreign > 64)                                   return -1;
        }
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
