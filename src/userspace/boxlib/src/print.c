#include "box/print.h"
#include "box/color.h"
#include "box/vga.h"
#include "box/keyboard.h"
#include "box/string.h"
#include "box/ipc.h"
#include "box/convert.h"
#include "box/debug.h"
#include "box/brook.h"
#include "box/bay.h"
#include "box/system.h"
#include "box/clock.h"
#include "box/error.h"
#include "box/core/notify.h"
#include "box/core/result.h"
#include "box/core/strand_self.h"
#include "box/memory.h"
#include "box/display.h"
#include "box/touch.h"
#include "box/turnin.h"


enum {
    LANE_UNOPENED = 0,
    LANE_OPEN     = 1,
    LANE_DEAD     = 2,
};

typedef struct StrandPrintState {
    void      *lane;
    uint64_t  *order;
    ConsoleRun run;
    uint32_t   last_fg;
    uint32_t   last_bg;
    uint32_t   color_fg;
    uint32_t   color_bg;
    uint16_t   ear;
    uint16_t   kb;
    uint8_t    lane_state;
    uint8_t    last_set;
    uint8_t    initialized;
    uint8_t    ear_flags;
} StrandPrintState;

enum { EAR_CLAIMED = 1u, KB_CLAIMED = 2u };
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

static uint8_t  g_io_mode      = IO_MODE_IPC;
static uint32_t g_display_pid  = 0;

void     io_set_mode(uint8_t mode)
{
    if (mode != g_io_mode) print_state_self()->last_set = false;
    g_io_mode = mode;
}
uint8_t  io_get_mode(void)                { return g_io_mode; }
void     io_set_display_pid(uint32_t pid) { g_display_pid = pid; }
uint32_t io_get_display_pid(void)         { return g_display_pid; }


static void emit_run(StrandPrintState *ps, const char *bytes, int len,
                     Color fg, Color bg);

static bool io_direct(const StrandPrintState *ps)
{
    return g_io_mode != IO_MODE_IPC || ps->lane_state == LANE_DEAD;
}

static void lane_fail(StrandPrintState *ps)
{
    ps->lane_state = LANE_DEAD;
    ps->last_set   = false;
    if (ps->order) { bay_release(ps->order); ps->order = NULL; }
    kdbg_print("[print] this strand's console lane is unavailable; it prints "
               "straight to the glass from here on");
}

static bool g_no_daemon;

static int daemon_say(const uint8_t *req, uint16_t len)
{
    static bool waited_before = false;

    for (;;) {
        int rc = send(g_display_pid, req, len);
        if (rc == 0) return 0;
        error_t why = box_errno_of(rc);
        if (why != ERR_ROUTE_TARGET_FULL && why != ERR_NO_MEMORY) return rc;
        if (!waited_before) {
            waited_before = true;
            kdbg_print("[print] the display daemon would not take a word just "
                       "now (%s); waiting for room rather than burying it",
                       why == ERR_ROUTE_TARGET_FULL ? "its ring is full"
                                                    : "no room for the record");
        }
        yield();
    }
}

static bool lane_await_grant(char *tag, size_t tag_cap, bool *no_daemon)
{
    Result  *held     = NULL;
    uint32_t held_n   = 0;
    uint32_t held_cap = 0;
    bool     got      = false;
    bool     done     = false;

    TouchTag pdied    = touch_pair_choose(touch_intern(TOUCH_TAG_PROCESS_DIED));
    bool     watching = (pdied != TOUCH_TAG_INVALID) &&
                        touch_claim(pdied, TOUCH_REST, 0, 0) == 0;

    uint8_t  req[5];
    uint32_t gen = strand_self_generation();
    req[0] = DISP_CMD_LANE;
    memcpy(req + 1, &gen, sizeof(gen));

    if (g_display_pid != 0) {
        if (daemon_say(req, sizeof(req)) < 0) done = true;
    } else {
        int rc = broadcast("display", req, sizeof(req));
        if (rc < 0 && box_errno_of(rc) == ERR_ROUTE_NO_SUBSCRIBERS) {
            *no_daemon = true;
            done = true;
        }
    }

    while (!done) {
        TurnInMark mark  = box_mark();
        bool       moved = false;

        Result r;
        while (!done && receive(&r)) {
            moved = true;
            const uint8_t *d = (const uint8_t *)(uintptr_t)r.data_addr;
            if (r.sender_pid != 0 && r.data_addr != 0 &&
                r.data_length >= 1 && d[0] == DISP_CMD_LANE) {
                if (r.data_length >= 2) {
                    uint32_t n = r.data_length - 1;
                    if (n >= tag_cap) n = (uint32_t)tag_cap - 1;
                    memcpy(tag, d + 1, n);
                    tag[n] = '\0';
                    if (tag[0] != '\0') {
                        __sync_bool_compare_and_swap(&g_display_pid, 0,
                                                     r.sender_pid);
                        got = true;
                    }
                }
                done = true;
                break;
            }

            if (held_n == held_cap) {
                uint32_t cap = held_cap ? held_cap * 2 : 8;
                Result *grown = (Result *)malloc(cap * sizeof(Result));
                if (!grown) {
                    result_restash(&r);
                    kdbg_print("[print] no room to hold what is not the lane "
                               "grant; this strand gives up the lane and "
                               "prints straight to the glass");
                    for (uint32_t i = 0; i < held_n; i++) result_restash(&held[i]);
                    if (held) free(held);
                    return false;
                }
                if (held) {
                    memcpy(grown, held, held_n * sizeof(Result));
                    free(held);
                }
                held = grown;
                held_cap = cap;
            }
            held[held_n++] = r;
        }
        if (done) break;

        bool  ask_again = false;
        Touch t;
        while (watching && touch_try_pop_tag(pdied, &t)) {
            moved = true;
            if (t.payload_len < sizeof(TouchProcessDied)) continue;
            TouchProcessDied dd;
            memcpy(&dd, t.payload, sizeof(dd));
            if (g_display_pid != 0) {
                if (dd.pid == g_display_pid) { done = true; break; }
            } else {
                ask_again = true;
            }
        }
        if (done) break;
        if (ask_again) {
            int rc = broadcast("display", NULL, 0);
            if (rc < 0 && box_errno_of(rc) == ERR_ROUTE_NO_SUBSCRIBERS) break;
        }

        if (!moved) box_turn_in(mark);
    }

    if (watching) touch_release(pdied);

    for (uint32_t i = 0; i < held_n; i++) result_restash(&held[i]);
    if (held) free(held);
    return got;
}


static bool console_order_ensure(StrandPrintState *ps)
{
    if (ps->order) return true;

    ps->order = (uint64_t *)bay_open(CONSOLE_ORDER_TAG, 0, BAY_OPEN);
    if (!ps->order) {
        kdbg_print("[print] the console granted a lane but no order; this "
                   "strand gives the lane back and prints straight to the glass");
        return false;
    }
    return true;
}

static uint64_t console_order_next(StrandPrintState *ps)
{
    return __atomic_add_fetch(ps->order, 1u, __ATOMIC_SEQ_CST);
}

static bool lane_ensure(StrandPrintState *ps)
{
    if (ps->lane_state == LANE_OPEN) return true;
    if (ps->lane_state == LANE_DEAD) return false;

    if (g_no_daemon) { lane_fail(ps); return false; }

    char tag[64];
    bool no_daemon = false;
    if (!lane_await_grant(tag, sizeof(tag), &no_daemon)) {
        if (no_daemon) {
            g_no_daemon = true;
            io_set_mode(IO_MODE_VGA);
        }
        lane_fail(ps);
        return false;
    }

    Brook *b = brook_open(tag, sizeof(ConsoleRun), CONSOLE_LANE_FRAMES,
                          BROOK_WRITER);
    if (!b) {
        lane_fail(ps);
        return false;
    }
    if (!console_order_ensure(ps)) {
        brook_release(b);
        lane_fail(ps);
        return false;
    }

    ps->lane       = b;
    ps->lane_state = LANE_OPEN;
    ps->ear        = touch_pair_choose(touch_intern(tag));
    ps->ear_flags &= (uint8_t)~EAR_CLAIMED;
    return true;
}

static bool lane_push(StrandPrintState *ps, const ConsoleRun *f)
{
    int rc = brook_push((Brook *)ps->lane, f);
    if (rc == 0) return true;

    Brook *dead = (Brook *)ps->lane;
    ps->lane = NULL;
    lane_fail(ps);
    if (dead) brook_release(dead);
    return false;
}

static void lane_flush_run(StrandPrintState *ps)
{
    if (ps->run.len == 0) return;
    ps->run.kind      = CONSOLE_RUN_TEXT;
    ps->run._reserved = 0;
    ps->run.order     = console_order_next(ps);

    bool    ok  = lane_push(ps, &ps->run);
    uint8_t len = ps->run.len;
    ps->run.len = 0;
    if (!ok) {
        emit_run(ps, ps->run.text, len, ps->run.fg, ps->run.bg);
    }
}

static void lane_flush_lines(StrandPrintState *ps)
{
    uint8_t len = ps->run.len;
    if (len == 0) return;

    int cut = -1;
    for (int i = (int)len - 1; i >= 0; i--) {
        if (ps->run.text[i] == '\n') { cut = i + 1; break; }
    }
    if (cut <= 0 || (uint8_t)cut == len) { lane_flush_run(ps); return; }

    char    tail[CONSOLE_RUN_TEXT_MAX];
    uint8_t tlen = (uint8_t)(len - cut);
    memcpy(tail, ps->run.text + cut, tlen);
    Color fg = ps->run.fg, bg = ps->run.bg;

    ps->run.len = (uint8_t)cut;
    lane_flush_run(ps);

    if (ps->lane_state == LANE_OPEN) {
        ps->run.fg  = fg;
        ps->run.bg  = bg;
        memcpy(ps->run.text, tail, tlen);
        ps->run.len = tlen;
    } else {
        emit_run(ps, tail, tlen, fg, bg);
    }
}

static void io_flush_state(StrandPrintState *ps)
{
    if (g_io_mode == IO_MODE_IPC && ps->lane_state == LANE_OPEN)
        lane_flush_run(ps);
}

void io_flush(void)
{
    io_flush_state(print_state_self());
}

static void push_color(StrandPrintState *ps)
{
    if (g_io_mode == IO_MODE_IPC) return;

    uint32_t fg = BoxColorResolveFg(ps->color_fg);
    uint32_t bg = BoxColorResolveBg(ps->color_bg);
    if (ps->last_set && ps->last_fg == fg && ps->last_bg == bg) return;

    vga_setcolor_rgb(fg, bg);
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

static void emit_run(StrandPrintState *ps, const char *bytes, int len, Color fg, Color bg)
{
    if (len <= 0) return;

    fg = BoxColorResolveFg(fg);
    bg = BoxColorResolveBg(bg);

    if (g_io_mode == IO_MODE_IPC && lane_ensure(ps)) {
        if (ps->run.len > 0 && (ps->run.fg != fg || ps->run.bg != bg))
            lane_flush_run(ps);

        int off = 0;
        while (off < len && ps->lane_state == LANE_OPEN) {
            if (ps->run.len == CONSOLE_RUN_TEXT_MAX) {
                lane_flush_lines(ps);
                continue;
            }
            if (ps->run.len == 0) {
                ps->run.fg = fg;
                ps->run.bg = bg;
            }
            uint32_t space = CONSOLE_RUN_TEXT_MAX - ps->run.len;
            uint32_t chunk = (uint32_t)(len - off);
            if (chunk > space) chunk = space;
            memcpy(ps->run.text + ps->run.len, bytes + off, chunk);
            ps->run.len = (uint8_t)(ps->run.len + chunk);
            off += chunk;
        }
        if (ps->lane_state == LANE_OPEN && memchr(ps->run.text, '\n', ps->run.len))
            lane_flush_lines(ps);
        if (off >= len) return;

        bytes += off;
        len   -= off;
    }

    bool changed = !ps->last_set || ps->last_fg != fg || ps->last_bg != bg;
    if (changed) {
        vga_setcolor_rgb(fg, bg);
        ps->last_fg  = fg;
        ps->last_bg  = bg;
        ps->last_set = true;
    }
    char tmp[256];
    int  off = 0;
    while (off < len) {
        int take = len - off;
        if (take > (int)sizeof(tmp) - 1) {
            take = (int)sizeof(tmp) - 1;
            for (int k = off + take - 1; k >= off; k--) {
                if (bytes[k] == '\n') { take = k - off + 1; break; }
            }
        }
        memcpy(tmp, bytes + off, (size_t)take);
        tmp[take] = '\0';
        vga_puts(tmp);
        off += take;
    }
}

static void emit_filtered(StrandPrintState *ps, const char* data, size_t len,
                          bool newline)
{
    char   chunk[256];
    size_t src = 0;
    do {
        int chunk_pos = 0;
        while (chunk_pos < (int)sizeof(chunk) - 1 && src < len) {
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
        if (newline && src >= len) chunk[chunk_pos++] = '\n';
        if (chunk_pos == 0) break;
        emit_run(ps, chunk, chunk_pos, ps->color_fg, ps->color_bg);
    } while (src < len);
}

void print(const char* str)
{
    if (!str) return;
    StrandPrintState *ps = print_state_self();

    bool we_began = false;
    if (io_direct(ps)) { vga_begin(); we_began = true; }

    emit_filtered(ps, str, strlen(str), false);

    if (we_began) vga_commit();
}

void print_bytes(const char* data, size_t len)
{
    if (!data || len == 0) return;
    StrandPrintState *ps = print_state_self();

    bool we_began = false;
    if (io_direct(ps)) { vga_begin(); we_began = true; }

    emit_filtered(ps, data, len, false);

    if (we_began) vga_commit();
}

void println(const char* str)
{
    StrandPrintState *ps = print_state_self();

    bool we_began = false;
    if (io_direct(ps)) { vga_begin(); we_began = true; }

    if (str) emit_filtered(ps, str, strlen(str), true);
    else     emit_run(ps, "\n", 1, ps->color_fg, ps->color_bg);

    if (we_began) vga_commit();
}

void clear(void)
{
    StrandPrintState *ps = print_state_self();

    ps->last_set = false;

    if (g_io_mode == IO_MODE_IPC && lane_ensure(ps)) {
        lane_flush_run(ps);
        if (ps->lane_state == LANE_OPEN) {
            ConsoleRun f;
            memset(&f, 0, sizeof(f));
            f.kind = CONSOLE_RUN_CLEAR;
            f.fg   = COLOR_LIGHT_GRAY;
            f.bg   = COLOR_BLACK;
            f.order = console_order_next(ps);
            if (lane_push(ps, &f)) return;
        }
    }
    vga_clear_rgb(COLOR_LIGHT_GRAY, COLOR_BLACK);
}


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

__attribute__((weak)) int printf(const char *fmt, ...)
{
    if (!fmt) return -1;
    StrandPrintState *ps = print_state_self();

    va_list args;
    va_start(args, fmt);

    bool we_began = false;
    if (io_direct(ps)) { vga_begin(); we_began = true; }

    char buf[PRINTF_BUFLEN];
    int  pos       = 0;
    int  total_out = 0;
    Color cur_fg   = ps->color_fg;
    Color cur_bg   = ps->color_bg;
    char  numbuf[24];

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

        const char *spec_start = fmt;
        fmt++;
        if (*fmt == '\0') {
            ENSURE(2);
            buf[pos++] = '%';
            break;
        }

        if (*fmt == 'c' && fmt[1] == 'o' && fmt[2] == 'l' &&
            fmt[3] == 'o' && fmt[4] == 'r') {
            FLUSH();
            cur_fg = (Color)va_arg(args, uint32_t);
            fmt += 5;
            continue;
        }

        if (*fmt == 'b' && fmt[1] == 'g' && fmt[2] == 'c' &&
            fmt[3] == 'o' && fmt[4] == 'l' && fmt[5] == 'o' &&
            fmt[6] == 'r') {
            FLUSH();
            cur_bg = (Color)va_arg(args, uint32_t);
            fmt += 7;
            continue;
        }

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

TouchTag console_listen(void)
{
    StrandPrintState *ps = print_state_self();
    io_flush_state(ps);

    if (g_io_mode == IO_MODE_IPC && lane_ensure(ps)) {
        if (!(ps->ear_flags & EAR_CLAIMED)) {
            if (ps->ear == TOUCH_TAG_INVALID ||
                touch_claim(ps->ear, TOUCH_REST, 0, 0) != 0)
                return TOUCH_TAG_INVALID;
            ps->ear_flags |= EAR_CLAIMED;
        }
        uint8_t  req[6];
        uint32_t gen = strand_self_generation();
        req[0] = DISP_CMD_LISTEN;
        memcpy(req + 1, &gen, sizeof(gen));
        req[5] = 1;
        if (daemon_say(req, sizeof(req)) == 0) return ps->ear;

        Brook *dead = (Brook *)ps->lane;
        ps->lane = NULL;
        lane_fail(ps);
        if (dead) brook_release(dead);
    }

    if (!(ps->ear_flags & KB_CLAIMED)) {
        TouchTag kb = touch_pair_choose(touch_intern(TOUCH_TAG_KEYBOARD));
        if (kb == TOUCH_TAG_INVALID || touch_claim(kb, TOUCH_REST, 0, 0) != 0)
            return TOUCH_TAG_INVALID;
        ps->kb         = kb;
        ps->ear_flags |= KB_CLAIMED;
    }
    return ps->kb;
}

void console_unlisten(void)
{
    StrandPrintState *ps = print_state_self();
    if (g_io_mode != IO_MODE_IPC || ps->lane_state != LANE_OPEN ||
        !(ps->ear_flags & EAR_CLAIMED))
        return;
    uint8_t  req[6];
    uint32_t gen = strand_self_generation();
    req[0] = DISP_CMD_LISTEN;
    memcpy(req + 1, &gen, sizeof(gen));
    req[5] = 0;
    (void)daemon_say(req, sizeof(req));
}

void console_step(int32_t delta)
{
    if (delta == 0) return;
    StrandPrintState *ps = print_state_self();

    if (g_io_mode == IO_MODE_IPC && lane_ensure(ps)) {
        lane_flush_run(ps);
        if (ps->lane_state == LANE_OPEN) {
            ConsoleRun f;
            memset(&f, 0, sizeof(f));
            f.kind = CONSOLE_RUN_STEP;
            f.len  = sizeof(delta);
            memcpy(f.text, &delta, sizeof(delta));
            f.fg   = BoxColorResolveFg(ps->color_fg);
            f.bg   = BoxColorResolveBg(ps->color_bg);
            f.order = console_order_next(ps);
            if (lane_push(ps, &f)) return;
        }
    }

    (void)vga_step_cursor(delta);
}

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