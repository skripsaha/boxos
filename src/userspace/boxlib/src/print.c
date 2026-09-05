#include "box/print.h"
#include "box/color.h"
#include "box/vga.h"
#include "box/keyboard.h"
#include "box/string.h"
#include "box/ipc.h"
#include "box/convert.h"
#include "box/debug.h"
#include "box/brook.h"
#include "box/system.h"
#include "box/cpu.h"
#include "box/clock.h"
#include "box/error.h"
#include "box/core/notify.h"
#include "box/core/result.h"
#include "box/core/strand_self.h"
#include "box/memory.h"
#include "box/display.h"

/* ===========================================================================
 * Per-strand print state — thread-confined, NO lock.
 *
 * Every strand (main + spawned) owns its own console lane, frame under
 * construction, VGA-attr cache and current colours. Concurrent strands
 * calling print/printf never touch each other's state, so the print path
 * itself never takes a lock.
 *
 * The console is a stream: output rides a per-strand Brook lane the display
 * daemon reads ("console:N", granted over DISP_CMD_LANE). The lane opens
 * lazily on the strand's FIRST print — opening costs a daemon round-trip,
 * a brook_open (kmalloc + PMM + VMM in both cabins) and one malloc for the
 * handle, so only strands that actually print ever pay it (the
 * touch_stash_ptr pattern). After that the steady-state path is memcpy into
 * the frame plus a lock-free brook_push — no malloc, no syscall until the
 * daemon side needs waking. That preserves the real safety contract of
 * printf-under-heap_lock ("no malloc on the path"): memory.c's
 * heap_dump_tags caller prints long after its strand's first print.
 *
 * A full lane SLOWS the writer (lane_push waits politely) — printing is
 * never dropped. A dead daemon is an honest refusal: one kdbg trace, then
 * the process falls back to direct VGA (the shell's own no-daemon
 * precedent) so the machine keeps talking.
 *
 * Storage: the main strand uses a static instance (g_main_print_state); a
 * spawned strand's instance lives inline in its StrandInfo (print_state[]).
 * The kernel zero-inits that block, so `initialized` starts false and
 * print_state_self() corrects fg/bg to the process defaults on first touch.
 * Per owner decision, a spawned strand's colours always start FRESH
 * (COLOR_DEFAULT / COLOR_BLACK) — no inheritance from whoever spawned it.
 * =========================================================================== */

enum {
    LANE_UNOPENED = 0,   /* kernel zero-init — no lane yet */
    LANE_OPEN     = 1,
    LANE_DEAD     = 2,   /* daemon refused/left; strand emits direct VGA */
};

typedef struct StrandPrintState {
    void      *lane;        /* Brook* — this strand's console lane */
    ConsoleRun run;         /* frame under construction; run.len = fill */
    uint32_t   last_fg;     /* VGA-direct colour cache (resolved pair) */
    uint32_t   last_bg;
    uint32_t   color_fg;    /* current colours (may hold sentinels) */
    uint32_t   color_bg;
    uint8_t    lane_state;  /* LANE_* */
    uint8_t    last_set;    /* last_fg/last_bg carry a sent pair */
    uint8_t    initialized;
    uint8_t    _pad[5];
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
 * below (first-writer-wins CAS) since any strand's lane grant / readline /
 * getchar can race to discover it; g_io_mode is a single-writer invariant
 * (see io_set_mode) with one sanctioned exception — the daemon-death
 * fallback flips it to IO_MODE_VGA so the machine keeps talking.
 * =========================================================================== */
static uint8_t  g_io_mode      = IO_MODE_IPC;
static uint32_t g_display_pid  = 0;

void     io_set_mode(uint8_t mode)
{
    /* Switching between VGA and IPC paths invalidates the kernel colour
     * state we cached locally; force the next emit to re-send.
     * Single-writer invariant: call before any strand_spawn — no other
     * strand's cache exists yet, so invalidating only the caller's own is
     * sufficient (every strand spawned afterwards starts uninitialised
     * anyway and re-sends its colour on its first emit). A strand that
     * already opened a console lane and is then switched to VGA simply
     * stops pushing; the daemon closes the idle lane when the strand
     * exits (writer-leave drains to STREAM_CLOSED). */
    if (mode != g_io_mode) print_state_self()->last_set = false;
    g_io_mode = mode;
}
uint8_t  io_get_mode(void)                { return g_io_mode; }
void     io_set_display_pid(uint32_t pid) { g_display_pid = pid; }
uint32_t io_get_display_pid(void)         { return g_display_pid; }

/* ===========================================================================
 * Console lane — the strand's output stream to the display daemon.
 *
 * Grant protocol ("checkroom"): send DISP_CMD_LANE, the daemon opens the
 * reader side of a fresh "console:N" Brook FIRST and only then replies
 * [DISP_CMD_LANE][tag NUL] — the writer arrives at a laid table. A 1-byte
 * reply is an honest refusal.
 * =========================================================================== */

static void emit_run(StrandPrintState *ps, const char *bytes, int len,
                     Color fg, Color bg);

static void lane_fail(StrandPrintState *ps)
{
    /* Honest refusal, not silent swallowing: leave one trace, then keep
     * the machine talking through direct VGA. Process-wide flip mirrors
     * the shell's own no-daemon fallback; other strands' pushes fail the
     * same way and converge here on their next emit. */
    ps->lane_state = LANE_DEAD;
    kdbg_print("[print] console lane unavailable; direct VGA from here on");
    io_set_mode(IO_MODE_VGA);
}

/* Wait for the daemon's DISP_CMD_LANE reply. Messages that are not the
 * grant are HELD aside and restashed after the wait — restashing inside
 * the loop would hand the same message straight back to us (receive
 * consults the ipc stash first), and dropping would eat a payload the
 * application is owed (spawn args arrive before a utility's first print).
 * The hold grows on demand so no flood can force a drop.
 *
 * The wait carries NO wall-clock deadline — a guessed number of
 * milliseconds is exactly the timeout class that turns a slow stand into
 * a phantom failure (measured: 16 vCPUs on one TCG thread stretched a
 * grant past five seconds, and the old 2×2500 ms guess declared a live
 * daemon dead). The only watchdog is a fact, not a clock: if the daemon
 * PROCESS is gone, the wait ends. readline blocks on its reply under the
 * same contract. */
static bool lane_await_grant(char *tag, size_t tag_cap)
{
    Result  *held     = NULL;
    uint32_t held_n   = 0;
    uint32_t held_cap = 0;
    bool     got      = false;
    bool     asked    = false;

    for (;;) {
        if (!asked) {
            uint8_t req = DISP_CMD_LANE;
            if (g_display_pid != 0) {
                if (send(g_display_pid, &req, 1) < 0) break;
                asked = true;
            } else {
                /* The kernel answers "is anyone wearing the tag" on the
                 * spot: ERR_ROUTE_NO_SUBSCRIBERS means there is no daemon
                 * to wait for, so waiting would be watching a clock for an
                 * event that cannot happen. A daemon that exists but has
                 * not reached its loop yet banks the broadcast and answers
                 * when it gets there — that is worth waiting out. */
                int rc = broadcast("display", &req, 1);
                if (rc < 0 && box_errno_of(rc) == ERR_ROUTE_NO_SUBSCRIBERS)
                    break;
                asked = true;
            }
        }

        Result r;
        if (!receive_wait(&r, 1000)) {
            /* A quiet second — not a verdict. Check the fact that would
             * make further waiting a lie: the daemon process being gone. */
            if (g_display_pid != 0) {
                proc_info_t info;
                if (proc_info((uint16_t)g_display_pid, &info) != 0) break;
            }
            continue;
        }

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
            break;   /* grant, or the daemon's explicit refusal */
        }

        if (held_n == held_cap) {
            uint32_t cap = held_cap ? held_cap * 2 : 8;
            Result *grown = (Result *)malloc(cap * sizeof(Result));
            if (!grown) { result_restash(&r); break; }
            if (held) {
                memcpy(grown, held, held_n * sizeof(Result));
                free(held);
            }
            held = grown;
            held_cap = cap;
        }
        held[held_n++] = r;
    }

    /* Give every held message back in arrival order — the stash is
     * consulted before the ring, so later consumers see them first. */
    for (uint32_t i = 0; i < held_n; i++) result_restash(&held[i]);
    if (held) free(held);
    return got;
}

static bool lane_ensure(StrandPrintState *ps)
{
    if (ps->lane_state == LANE_OPEN) return true;
    if (ps->lane_state == LANE_DEAD) return false;

    char tag[64];
    if (!lane_await_grant(tag, sizeof(tag))) {
        lane_fail(ps);
        return false;
    }

    /* Shape is part of tag identity; passing the canonical constants makes
     * a header drift between writer and daemon fail loudly at open. */
    Brook *b = brook_open(tag, sizeof(ConsoleRun), CONSOLE_LANE_FRAMES,
                          BROOK_WRITER);
    if (!b) {
        lane_fail(ps);
        return false;
    }
    ps->lane       = b;
    ps->lane_state = LANE_OPEN;
    return true;
}

/* Push one built frame. A full ring SLOWS the writer — output is never
 * dropped — but the wait must be a POLITE one: the console is a fan-in of
 * every printing strand into ONE daemon, and both naive waits fail it at
 * scale (measured, print_stress 16 strands):
 *   - brook_push's UMWAIT path holds the core while it watches the
 *     cursor; sixteen watchers starve the very reader they wait for
 *     (24k lines sat banked until the writers died);
 *   - yield-per-attempt floods the Guide with a syscall storm from every
 *     blocked writer, and the daemon's render ops drown in that queue
 *     (299 frames rendered in 160 s).
 * The shape that serves a fan-in is pause-spin with EXPONENTIAL backoff
 * between yields: a fresh stall probes hot (microseconds), a standing
 * stall backs off toward a few milliseconds of pause per yield — always
 * shorter than the ring-drain it is waiting out, and hundreds (not
 * hundreds of thousands) of syscalls per second per blocked writer.
 * On daemon death flips to the VGA fallback and returns false so the
 * caller re-delivers its content by the direct path. */
#define LANE_PUSH_SPIN_MIN  2048u      /* same class as BROOK_SPIN_BUDGET */
#define LANE_PUSH_SPIN_MAX  (1u << 20) /* ~ a few ms of PAUSE — µarch class */

/* How long a jammed lane may stay silent before it says so. Not a deadline —
 * this wait never expires and never drops a line — a WATCH over silence. */
#define LANE_JAM_ANNOUNCE_MS  60000u

static bool lane_push(StrandPrintState *ps, const ConsoleRun *f)
{
    int      rc;
    uint32_t budget = LANE_PUSH_SPIN_MIN;
    uint32_t spins  = 0;
    uint64_t began  = 0;
    bool     said   = false;
    while ((rc = brook_try_push((Brook *)ps->lane, f)) == -ERR_WOULD_BLOCK) {
        if (++spins < budget) {
            __asm__ volatile("pause" ::: "memory");
        } else {
            spins = 0;
            if (budget < LANE_PUSH_SPIN_MAX) budget <<= 1;

            /* A full lane must not drop the line, so this waits without a
             * deadline — and a deadline-free wait with no voice is how a
             * machine stands still in perfect silence. MEASURED: a matrix run
             * stopped mid-line inside phase35 with a writer here, and nothing
             * in the log said anything at all; Nightwatch could not see it
             * either, because a writer waiting for ring space has no
             * unanswered submit to notice — it is waiting for a READER.
             *
             * So the wait keeps waiting, and after long enough it says once
             * what no one else can: the reader is attached (a departed one
             * fails the push outright, below) and it is not draining.
             * kdbg_print reaches the kernel directly through HW_DEBUG_PRINT
             * rather than through this lane, so the complaint cannot queue
             * behind the jam it is describing. */
            if (!said) {
                uint64_t now = cpu_rdtsc();
                if (began == 0) {
                    began = now;
                } else if (now - began >= cpu_ms_to_tsc(LANE_JAM_ANNOUNCE_MS)) {
                    said = true;
                    kdbg_print("[print] DEFECT: console lane full for %u ms — "
                               "the reader is attached but not draining. This "
                               "writer is still waiting (it will not drop the "
                               "line); output stops here until the lane moves",
                               (unsigned)LANE_JAM_ANNOUNCE_MS);
                }
            }
            yield();
        }
    }
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
    ps->run.tsc       = cpu_rdtsc();

    bool    ok  = lane_push(ps, &ps->run);
    uint8_t len = ps->run.len;
    ps->run.len = 0;
    if (!ok) {
        /* The frame never reached the daemon; its text still must reach
         * the screen. lane_fail switched us to VGA, so this re-emit takes
         * the direct path (run.fg/bg are already resolved — resolving
         * again is the identity). */
        emit_run(ps, ps->run.text, len, ps->run.fg, ps->run.bg);
    }
}

/* Flush WHOLE LINES, and keep the unfinished one for the next frame.
 *
 * A frame is the unit the daemon interleaves. It merges every lane in global
 * push order, so two strands printing at once alternate at frame boundaries —
 * and a boundary that falls in the middle of a line puts half of one strand's
 * line inside another's. MEASURED, sixteen strands on sixteen cores: roughly
 * three hundred of thirty-two thousand lines came out spliced, e.g.
 *
 *     [PS-11] 00000[PS-04] 00000000
 *     [PS-04787
 *
 * Nothing is LOST — the character count is exact, every time — but a spliced
 * line is unreadable, and worse, unmatchable: every marker this system is
 * checked by is a grep, and a torn "[STRAND] PASS" is a green run reported as
 * a hang. That is what made a sixteen-core matrix a lottery.
 *
 * The cut is therefore taken at the last newline in the run rather than at the
 * end of the buffer, and the remainder — an unfinished line — stays behind to
 * be completed by the writer that owns it. A line then occupies whole frames
 * and cannot be split by anybody else's.
 *
 * Two cases still end a frame mid-line, and both are honest: a single line
 * longer than a whole frame has nowhere else to break, and a colour change
 * inside a line genuinely ends a run (the run IS the colour). Neither is a
 * splice between two writers waiting to happen the way an arbitrary
 * 108th-byte boundary was.
 *
 * Frame count is unchanged in the case that matters — the cut moves by at most
 * one line's worth, it does not add frames. */
static void lane_flush_lines(StrandPrintState *ps)
{
    uint8_t len = ps->run.len;
    if (len == 0) return;

    int cut = -1;
    for (int i = (int)len - 1; i >= 0; i--) {
        if (ps->run.text[i] == '\n') { cut = i + 1; break; }
    }
    /* No line ends inside this run, or the run already ends on one: it goes
     * whole either way. */
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
        /* The lane died inside the flush and it has already re-delivered its
         * own bytes the direct way; the carried remainder must follow them. */
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

/* ===========================================================================
 * Color state.
 *
 * On the lane, colour is frame METADATA — every ConsoleRun carries its
 * resolved (fg, bg) pair, so set_color is pure state and the pair rides
 * the next emitted run. Only the direct VGA path pushes colour eagerly
 * (the kernel keeps a current pair for kprintf and cursor-relative ops).
 * =========================================================================== */
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

/* ===========================================================================
 * Low-level emit — push a chunk of ASCII bytes with a given colour pair.
 * Honours g_io_mode: the IPC path packs ConsoleRun frames onto the lane
 * ('\n' travels inside the text — it is content, not a control record);
 * the VGA direct path uses vga_puts.
 * =========================================================================== */
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
                /* Always makes room: the cut leaves either nothing or the
                 * unfinished tail, both shorter than a full run. */
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
        if (off >= len) return;

        /* The daemon died mid-run (lane_flush_run above fell back); the
         * pending frame was already re-emitted, deliver the remainder the
         * direct way too. */
        bytes += off;
        len   -= off;
    }

    /* VGA direct: set colour only when it actually changed; emit text
     * honouring newlines. vga_puts() takes a NUL-terminated string of
     * arbitrary length, but the underlying syscall packs into a kernel
     * buffer; for large segments we stream in 192-byte chunks. */
    bool changed = !ps->last_set || ps->last_fg != fg || ps->last_bg != bg;
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

    /* IO_MODE_IPC batches into the lane frame and flushes lazily;
     * IO_MODE_VGA wins a syscall reduction by feeding every internal
     * vga_setcolor/vga_puts/vga_newline into a single Manifest. The
     * nested vga_begin/vga_commit composes with an outer caller that may
     * itself be batching (printf, shell renderer). */
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
        emit_run(ps, "\n", 1, ps->color_fg, ps->color_bg);
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

    if (g_io_mode == IO_MODE_IPC && lane_ensure(ps)) {
        lane_flush_run(ps);
        if (ps->lane_state == LANE_OPEN) {
            ConsoleRun f;
            memset(&f, 0, sizeof(f));
            f.kind = CONSOLE_RUN_CLEAR;
            f.fg   = COLOR_LIGHT_GRAY;
            f.bg   = COLOR_BLACK;
            f.tsc  = cpu_rdtsc();
            if (lane_push(ps, &f)) return;
        }
        /* Daemon died on the way — the fallback below still clears. */
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
 * everything coalesces into lane frames and flushes lazily.
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
 * this file's statics — g_io_mode, the per-strand colour state, the lane
 * machinery — and splitting it would mean exposing all three across a boundary
 * to solve a linking question. The link order that makes this work is already
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
