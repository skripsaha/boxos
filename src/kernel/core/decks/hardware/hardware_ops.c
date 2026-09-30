/*
 * Hardware Deck — Manifest-native handlers (Phase 8 complete).
 *
 * Every prefix-chain handler in hardware_deck.c has a corresponding op here,
 * but the ABI is freed from the 192-byte cargo cult: bytes flow through
 * Crates of arbitrary size, fixed inputs through op->params.
 *
 * Param/Crate layouts are documented inline next to each handler so the
 * userspace builder can be regenerated mechanically.
 */

#include "klib.h"
#include "klib_logring.h"
#include "op_registry.h"
#include "manifest_auth.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "hardware_deck.h"
#include "crate_io.h"
#include "process.h"
#include "video.h"
#include "pit.h"
#include "rtc.h"
#include "keyboard.h"
#include "ata.h"
#include "tagfs.h"
#include "boardroom.h"
#include "system_halt.h"
#include "xhci.h"
#include "xhci_port.h"
#include "xhci_enumeration.h"
#include "touch.h"
#include "kring.h"
#include "kernel_config.h"

#define VGA_PUTSTRING_FLAG_KEEP_COLOR 0x02u

/* What userspace puts on the screen goes into the log ring, where the serial
 * line reads it (the Wire) and `logsave` writes it down. A saved log that has
 * the kernel's answers but not the command that caused them is a log
 * somebody has to guess at — the board is exactly where nobody can afford
 * to. The ring costs a store; nothing here waits on a UART, so the gate that
 * kept this off a board (87 us a character) is gone with the wait. */
static inline void HwVgaMirrorChar(char ch)
{
    LogRingPut(ch);
}

/* Colours arrive as unaligned little-endian u32 inside op->params. */
static inline uint32_t hw_color_param(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

/* =========================================================================
 *  VGA
 * ========================================================================= */

/* HW_VGA_PUTCHAR  params:[u8 row][u8 col][u8 char][u32 fg][u32 bg] */
static int HwVgaPutChar(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 11) return ERR_INVALID_ARGUMENT;

    uint8_t  row = op->params[0];
    uint8_t  col = op->params[1];
    uint8_t  ch  = op->params[2];
    uint32_t fg  = hw_color_param(&op->params[3]);
    uint32_t bg  = hw_color_param(&op->params[7]);

    /* Compared as the counts they are, not through a byte. The cast was the
     * same defect as the one HwVgaGetDimensions carried: on a 320-column
     * console (uint8_t)320 is 64, so every cell from column 64 rightward was
     * refused as out of range on a screen that had it. */
    if ((int)row >= VideoGetRows() || (int)col >= VideoGetCols()) {
        return ERR_OUT_OF_RANGE;
    }

    console_lock_acquire();

    /* The cursor is put back where it was, so the cell this op paints does not
     * disturb the line somebody else is writing. Kept as counts, not bytes:
     * the borrowed position is the kernel's own and has no business being
     * squeezed through the ABI's byte on a console wider than 255 cells. */
    int old_x = VideoGetCursorX();
    int old_y = VideoGetCursorY();

    /* One batch, and therefore one commit and one blit for the whole op. Its
     * body moves the cursor twice, and since a cursor move now reaches the
     * glass on its own (canvas.c: CanvasSetCursor), an unbatched putchar_at
     * would light the caret at the target cell, paint, and light it again back
     * home — three presents to put down one character. */
    VideoBatchBegin();
    VideoSetCursor(col, row);
    VideoPrintCharRgb((char)ch, fg, bg);
    HwVgaMirrorChar((char)ch);
    VideoSetCursor(old_x, old_y);
    VideoBatchEnd();

    console_lock_release();
    return OK;
}

/* HW_VGA_PUTSTRING  params:[u32 fg][u32 bg][u8 flags]
 *                   in_crate: string bytes (size = byte count, no length cap)
 *                   out_crate (optional): [u8 chars_written][u8 row][u8 col] */
static int HwVgaPutString(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crate_count;
    if (op->in_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 9)                return ERR_INVALID_ARGUMENT;

    uint32_t fg    = hw_color_param(&op->params[0]);
    uint32_t bg    = hw_color_param(&op->params[4]);
    uint8_t  flags = op->params[8];

    Crate *str_crate = &crates[op->in_crate];
    /* Snapshot the user string BEFORE taking the console lock, so user memory
     * is page-walked (and any page straddle handled) outside the lock.
     *
     * str_crate->size is attacker-controlled and this op is OP_AUTH_NONE, so
     * snapshot at most one page — a console line is a screenful, never the
     * gigabytes a caller could claim. CrateIsValid (manifest_exec) already
     * guarantees size <= capacity, so `want` is always a safe read length. */
    uint64_t want = str_crate->size < 4096u ? str_crate->size : 4096u;
    if (want == 0) return ERR_INVALID_ADDRESS;
    char *str = kmalloc((size_t)want);
    if (!str) return ERR_NO_MEMORY;
    if (crate_read(str_crate, ctx, str, want) != OK) {
        kfree(str);
        return ERR_INVALID_ADDRESS;
    }

    /* Hold the console lock around the whole VGA run.
     * The framebuffer and cursor are global state; without serialisation
     * a kprintf from another core (or another user process calling
     * vga_puts in parallel) would interleave at cell-level and produce
     * the character-salad screen the user saw on 2026-05-15. */
    console_lock_acquire();

    uint32_t old_fg, old_bg;
    VideoGetColorRgb(&old_fg, &old_bg);
    VideoSetColorRgb(fg, bg);

    uint64_t chars_written = 0;
    VideoBatchBegin();
    for (uint64_t i = 0; i < want; i++) {
        char c = str[i];
        if (c == '\0') break;
        VideoPrintCharCur(c);
        LogRingPut(c);           /* see HwVgaMirrorChar */
        chars_written++;
    }
    VideoBatchEnd();
    VideoUpdateCursor();

    if (!(flags & VGA_PUTSTRING_FLAG_KEEP_COLOR)) {
        VideoSetColorRgb(old_fg, old_bg);
    }

    console_lock_release();

    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *out = &crates[op->out_crate];
        if (out->capacity >= 3) {
            uint8_t blob[3];
            blob[0] = (uint8_t)(chars_written > 255 ? 255 : chars_written);
            blob[1] = (uint8_t)VideoGetCursorY();
            blob[2] = (uint8_t)VideoGetCursorX();
            (void)crate_write(out, ctx, blob, 3);
        }
    }

    kfree(str);
    return OK;
}

/* HW_VGA_PAINT  params:[u8 row][u8 col][u8 height][u8 width]
 *               in_crate: height*width TextCell, row-major, `width` per row
 *
 * A PICTURE, NOT SPEECH — and that is why nothing here reaches the log ring.
 * Every other op on this deck mirrors what it puts on screen into the ring so
 * the serial line and `logsave` carry the machine's account of itself. A
 * painted frame has no account to give: it is 6144 cells of colour thirty
 * times a second, and mirroring it would take the ring's global lock a
 * hundred thousand times a second and bury every word the machine actually
 * said under a screenful of spaces. What a painting program has to say, it
 * says in words, through printf, like everything else.
 *
 * One op is also one Canvas commit and therefore one Present: the whole
 * frame reaches the glass in a single blit, with no tear down its middle.
 * Painted through vga_putchar_at it would have been 6144 ops, ~141 KB of
 * Manifest, and — past boxlib's 4 KiB builder — a dozen separate blits.
 *
 * UTILITY, where every other VGA op on this deck is NONE, and the difference
 * is the crate. The others cost the kernel a cell, a line, or at most the one
 * page HwVgaPutString snapshots. This one is told its geometry in four bytes
 * and must then take the whole rectangle in: 255 × 255 cells of sizeof(TextCell)
 * is 780300 bytes of kernel memory, held for the length of one call, on the
 * word of the caller. The programs that legitimately need it are the ones that
 * own the whole screen and redraw it — draft, playtime, lavalamp, the display
 * daemon — and every one of them carries `utility`. A program that is only
 * saying something still has putchar, putstring and newline, unchanged. */
static int HwVgaPaint(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                      const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 4)               return ERR_INVALID_ARGUMENT;
    if (op->in_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    uint32_t row    = op->params[0];
    uint32_t col    = op->params[1];
    uint32_t height = op->params[2];
    uint32_t width  = op->params[3];
    if (height == 0 || width == 0) return ERR_INVALID_ARGUMENT;

    /* The rectangle's size is the contract, not a maximum: a crate that is
     * one cell short would otherwise be painted with whatever followed it in
     * the caller's address space. Exact, or refused. */
    Crate   *src  = &crates[op->in_crate];
    uint64_t want = (uint64_t)height * width * sizeof(TextCell);
    if (src->size != want) return ERR_INVALID_ARGUMENT;

    /* Snapshot outside the console lock — the same rule as PUTSTRING: user
     * memory is page-walked (and any page straddle handled) before any lock
     * is taken. crate_in_buf caps nothing, and it does not need to: `want` is
     * bounded by 255*255 cells because the geometry arrives as four bytes. */
    TextCell *cells = (TextCell *)crate_in_buf(src, ctx);
    if (!cells) return ERR_INVALID_ADDRESS;

    /* Resolve the sentinels here rather than in boxlib: this is the snapshot,
     * so the pass is free, and the Canvas keeps its rule that nothing but a
     * concrete triple ever reaches a cell. */
    for (uint64_t i = 0, n = (uint64_t)height * width; i < n; i++) {
        cells[i].fg = BoxColorResolveFg(cells[i].fg);
        cells[i].bg = BoxColorResolveBg(cells[i].bg);
    }

    console_lock_acquire();
    bool fit = VideoPaintCells(row, col, height, width, cells);
    console_lock_release();

    crate_buf_free(cells);
    return fit ? OK : ERR_OUT_OF_RANGE;
}

/* HW_VGA_CLEAR_SCREEN  params:[u32 fg][u32 bg] — every cell becomes a
 * space in this pair; the current colour state is untouched. */
static int HwVgaClearScreen(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                            const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 8) return ERR_INVALID_ARGUMENT;

    VideoClearScreenRgb(hw_color_param(&op->params[0]),
                        hw_color_param(&op->params[4]));
    return OK;
}

/* HW_VGA_CLEAR_LINE  params:[u8 row][u32 fg][u32 bg] */
static int HwVgaClearLine(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 9) return ERR_INVALID_ARGUMENT;

    uint8_t row = op->params[0];
    if ((int)row >= VideoGetRows()) return ERR_OUT_OF_RANGE;

    VideoClearLineRgb(row, hw_color_param(&op->params[1]),
                           hw_color_param(&op->params[5]));
    return OK;
}

/* HW_VGA_CLEAR_TO_EOL  params: none */
static int HwVgaClearToEol(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                           const OpContext *ctx)
{
    (void)op; (void)crates; (void)crate_count; (void)ctx;
    VideoClearToEol();
    return OK;
}

/* HW_VGA_GET_CURSOR  out_crate:[u8 row][u8 col] */
static int HwVgaGetCursor(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < 2) return ERR_BUFFER_TOO_SMALL;

    uint8_t blob[2];
    blob[0] = (uint8_t)VideoGetCursorY();
    blob[1] = (uint8_t)VideoGetCursorX();
    if (crate_write(out, ctx, blob, 2) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* HW_VGA_SET_CURSOR  params:[u8 row][u8 col]
 *                    out_crate (optional): clamped [u8 row][u8 col] */
static int HwVgaSetCursor(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 2) return ERR_INVALID_ARGUMENT;

    uint8_t row = op->params[0];
    uint8_t col = op->params[1];
    VideoSetCursor(col, row);

    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *out = &crates[op->out_crate];
        if (out->capacity >= 2) {
            uint8_t blob[2];
            blob[0] = (uint8_t)VideoGetCursorY();
            blob[1] = (uint8_t)VideoGetCursorX();
            (void)crate_write(out, ctx, blob, 2);
        }
    }
    return OK;
}

/* HW_VGA_STEP_CURSOR  params:[i32 delta]
 *
 * ‼ A RELATIVE MOVE IS RESOLVED WHERE THE POSITION LIVES, NOT WHERE IT IS
 * GUESSED.
 *
 * A line editor knows how far the caret must move — one cell left, four cells
 * back to the start of what it echoed — and it never knows where on the screen
 * that is: the column belongs to the console, which the kernel, the daemon and
 * every other program write to as well. Until this op the move was done in two
 * halves from outside: read the cursor with HW_VGA_GET_CURSOR, do the
 * arithmetic in userspace, write it back with HW_VGA_SET_CURSOR. Three things
 * were wrong with that, and this op ends all three.
 *
 * It read a position that could be stale by the time it was written. Anything
 * printed between the two calls — a kprintf from another core, another lane's
 * output — moved the cursor, and the step then landed relative to somebody
 * else's text.
 *
 * It made the caller do the arithmetic in cells, so it had to know how wide
 * the screen is. The width travels in a byte (HW_VGA_GET_DIMENSIONS), so on a
 * console wider than 255 columns the caller's arithmetic was wrong, and a
 * caller that failed to learn the width at all silently stopped moving the
 * cursor for the rest of the boot.
 *
 * And it cost three synchronous round trips per keystroke where one would do —
 * a read that had to flush the writer's batch, then the write. Backspace and
 * the arrow keys were three times heavier than typing a letter.
 *
 * The position is linear, row * cols + col, so a step crosses line ends the
 * way a reader expects, and it is clamped to the screen rather than wrapping
 * round: a caret cannot be stepped off the console it belongs to. */
static int HwVgaStepCursor(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                           const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 4) return ERR_INVALID_ARGUMENT;

    int32_t delta;
    memcpy(&delta, &op->params[0], sizeof(delta));
    if (delta == 0) return OK;

    console_lock_acquire();

    int cols = VideoGetCols();
    int rows = VideoGetRows();
    if (cols <= 0 || rows <= 0) {          /* no console to move a caret on */
        console_lock_release();
        return ERR_UNSUPPORTED;
    }

    int64_t linear = (int64_t)VideoGetCursorY() * cols + VideoGetCursorX() + delta;
    int64_t last   = (int64_t)rows * cols - 1;
    if (linear < 0)    linear = 0;
    if (linear > last) linear = last;

    VideoSetCursor((int)(linear % cols), (int)(linear / cols));

    console_lock_release();
    return OK;
}

/* HW_VGA_SET_COLOR  params:[u32 fg][u32 bg]
 *                   out_crate (optional): [u32 old_fg][u32 old_bg] */
static int HwVgaSetColor(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                         const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 8) return ERR_INVALID_ARGUMENT;

    uint32_t old[2];
    VideoGetColorRgb(&old[0], &old[1]);
    VideoSetColorRgb(hw_color_param(&op->params[0]),
                     hw_color_param(&op->params[4]));

    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *out = &crates[op->out_crate];
        if (out->capacity >= sizeof(old)) {
            (void)crate_write(out, ctx, old, sizeof(old));
        }
    }
    return OK;
}

/* HW_VGA_GET_COLOR  out_crate: [u32 fg][u32 bg] */
static int HwVgaGetColor(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                         const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < 8) return ERR_BUFFER_TOO_SMALL;

    uint32_t pair[2];
    VideoGetColorRgb(&pair[0], &pair[1]);
    if (crate_write(out, ctx, pair, sizeof(pair)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* HW_VGA_SCROLL_UP  no params */
static int HwVgaScrollUp(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                         const OpContext *ctx)
{
    (void)op; (void)crates; (void)crate_count; (void)ctx;
    console_lock_acquire();
    VideoScrollUp();
    console_lock_release();
    return OK;
}

/* HW_VGA_NEWLINE  out_crate (optional): [u8 row][u8 col] */
static int HwVgaNewline(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    console_lock_acquire();
    VideoPrintNewline();
    HwVgaMirrorChar('\n');
    uint8_t blob[2];
    blob[0] = (uint8_t)VideoGetCursorY();
    blob[1] = (uint8_t)VideoGetCursorX();
    console_lock_release();

    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *out = &crates[op->out_crate];
        if (out->capacity >= 2) {
            (void)crate_write(out, ctx, blob, 2);
        }
    }
    return OK;
}

/* HW_VGA_GET_DIMENSIONS  out_crate:[u8 cols][u8 rows]
 *
 * ‼ CLAMPED, NOT TRUNCATED, AND THE DIFFERENCE IS THE WHOLE POINT.
 *
 * The geometry travels in two bytes, so a console with more than 255 of
 * anything cannot be named in full here. This used to answer with a CAST:
 * a 2560-pixel GOP is 320 columns, and (uint8_t)320 is 64 — so a program
 * asked how wide the screen was, was told sixty-four, painted a fifth of the
 * glass and reported success. Worse, the value it was told was not even a
 * ceiling it could trust: 3840 px answered 224 of 480, and a program checking
 * for the zero that "too wide" was supposed to produce saw a plausible number
 * instead, because only an exact multiple of 2048 pixels wraps to zero.
 *
 * Clamping says the true thing this ABI can say: "255 is as far as you can
 * name". A program then paints the part of the screen it can address, and
 * everything it addresses is really there. A zero now has one meaning left —
 * there is no console — which is what VideoGetCols answers when the Canvas is
 * not ready, and that is worth being able to tell apart.
 *
 * The widening of the whole VGA ABI to sixteen bits is a separate piece of
 * work: it moves putchar_at, setcursor, clear_line, paint and this op, plus
 * every caller of vga_dimensions_t. Until then, this is the honest answer. */
static int HwVgaGetDimensions(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                              const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < 2) return ERR_BUFFER_TOO_SMALL;

    int cols = VideoGetCols();
    int rows = VideoGetRows();

    uint8_t blob[2];
    blob[0] = (uint8_t)(cols > 255 ? 255 : (cols < 0 ? 0 : cols));
    blob[1] = (uint8_t)(rows > 255 ? 255 : (rows < 0 ? 0 : rows));
    if (crate_write(out, ctx, blob, 2) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* =========================================================================
 *  Timer
 * ========================================================================= */

static int HwTimerGetTicks(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                           const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(uint64_t)) return ERR_BUFFER_TOO_SMALL;

    uint64_t ticks = pit_get_ticks();
    if (crate_write(out, ctx, &ticks, sizeof(uint64_t)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

static int HwTimerGetMs(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(uint64_t)) return ERR_BUFFER_TOO_SMALL;

    /* Use the dedicated monotonic uptime counter (advanced per-tick by
     * 1_000_000/freq µs) instead of deriving from `ticks * 1000 / freq`,
     * which is NOT monotonic when the scheduler reprograms the PIT under
     * load. The old derivation produced backwards-jumps causing S1's
     * "elapsed=0xFFFFFFFFFFFF…" underflow. */
    uint64_t ms = pit_get_uptime_ms();
    if (crate_write(out, ctx, &ms, sizeof(uint64_t)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

static int HwTimerGetFreq(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(uint32_t)) return ERR_BUFFER_TOO_SMALL;

    uint32_t freq = pit_get_frequency();
    if (crate_write(out, ctx, &freq, sizeof(uint32_t)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* =========================================================================
 *  RTC
 * ========================================================================= */

static int HwRtcGetUnix64(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(uint64_t)) return ERR_BUFFER_TOO_SMALL;

    uint64_t secs = rtc_get_unix64();
    if (crate_write(out, ctx, &secs, sizeof(uint64_t)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* HW_RTC_GET_TIME  out_crate: BoxTime (20 bytes packed) */
static int HwRtcGetTime(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(BoxTime)) return ERR_BUFFER_TOO_SMALL;

    BoxTime t;
    rtc_get_boxtime(&t);
    if (crate_write(out, ctx, &t, sizeof(BoxTime)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* HW_RTC_GET_UPTIME  out_crate: u64 ns */
static int HwRtcGetUptime(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(uint64_t)) return ERR_BUFFER_TOO_SMALL;

    uint64_t ns = rtc_get_uptime_ns();
    if (crate_write(out, ctx, &ns, sizeof(uint64_t)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* =========================================================================
 *  ‼ WHAT THIS DECK DELIBERATELY NO LONGER OFFERS
 *
 *  Fourteen opcodes are withdrawn and their numbers retired for ever — see
 *  hardware_deck.h. A deck answers for the hardware with NAMED operations the
 *  kernel understands and can carry through properly: hw.usb.reset is the
 *  whole eleven-step repair of a controller, hw.disk.info is the drive as the
 *  driver knows it, hw.vga.paint lays down a finished frame in one commit.
 *  None of the fourteen was one of those.
 *
 *  hw.port.inb/outb/inw/outw/inl/outl (0x20-0x25) were the `in` and `out`
 *      instructions with a name put on them — the screwdriver taken out of the
 *      lock, with which every other lock in the house can be worked. The port
 *      was assembled from two parameter bytes, so all 65536 of them were
 *      reachable, and not one of the six ever looked at the address it was
 *      handed. One call rewrote BAR0 of an xHCI controller, which the driver
 *      reads exactly once at bring-up and never again (xhci.c): the silicon
 *      then decodes a new window while the driver keeps writing to the old
 *      one, and the USB volume is simply gone with nothing in the log to say
 *      so. One call cleared the NMI mask through bit 7 of port 0x70 — a
 *      register the RTC never reads back and therefore tracks only as a
 *      variable of its own (rtc.c), so the kernel would go on believing NMI
 *      was armed. Three calls were the chipset reset sequence on 0xCF9. A
 *      fixed list of permitted ports could not have helped: five of the
 *      twenty ranges this kernel drives — BMIDE from BAR4, and SMI_CMD,
 *      PM1a/PM1b, PM_TMR, GPE0/GPE1 from the FADT — are not known until the
 *      machine is running and differ from board to board.
 *
 *  hw.irq.register/unregister/enable/disable/isr/irr/eoi (0x30-0x36) were the
 *      same hole one storey up. The check they shared refused line 0 and line
 *      2 and nothing else, so every other line could be masked from outside:
 *      the keyboard on 1, the Wire on 4, the disk on 14 and 15 — or somebody
 *      else's EOI could be sent through the middle of their handler. Two of
 *      the seven, 0x30 and 0x31, never had a handler at all: they were names
 *      in a header and nothing else, and the registry had never heard of them.
 *
 *  hw.cpu.halt (0x40) could not be made honest. A Manifest arrives through
 *      SYSCALL with IF clear — SFMASK clears it (amp/per_core.c) and nothing
 *      between there and a handler raises it again (idt.c says so of the
 *      dispatch itself) — so the bare `hlt` this op ran was a halt with
 *      interrupts masked: on a single-core machine the kernel stopped there
 *      until an NMI. What it was reaching for exists already and is woken by a
 *      real event rather than a guess: system.turn_in (SYSTEM_OP_TURN_IN,
 *      boxos_decks.h) lays a strand down until something addressed to it moves
 *      a cursor, and the machine as a whole goes quiet through Turn In and the
 *      Bell without anybody having to ask it to.
 * ========================================================================= */

/* =========================================================================
 *  Disk (ATA primary master/slave info + cache flush)
 * ========================================================================= */

/* HW_DISK_INFO  params:[u8 is_master]
 *               out_crate: [u8 exists][char model[40]][char serial[20]]
 *                          [u64 total_sectors][u64 size_mb]   = 77 bytes */
static int HwDiskInfo(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                      const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 1) return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < 77) return ERR_BUFFER_TOO_SMALL;

    uint8_t       is_master = op->params[0];
    ATADevice    *dev       = is_master ? &ata_primary_master : &ata_primary_slave;

    uint8_t blob[77];
    blob[0] = (uint8_t)dev->exists;
    memcpy(blob + 1,  dev->model,  40);
    memcpy(blob + 41, dev->serial, 20);
    memcpy(blob + 61, &dev->total_sectors, sizeof(uint64_t));
    memcpy(blob + 69, &dev->size_mb,       sizeof(uint64_t));
    if (crate_write(out, ctx, blob, 77) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* HW_DISK_FLUSH  params:[u8 is_master]
 *
 * The ABI still carries the legacy 0/1 master/slave flag, but the only durable
 * thing it can mean today is "commit the TagFS volume" — that is the device
 * any commit really cares about. tagfs_flush_cache() resolves where the volume
 * lives (AHCI port number or ATA drive index) at the storage layer, so this
 * stays correct on every (ATA, AHCI) × (boot port) combination. The parameter
 * itself names nothing this op can act on and is read only to keep the wire
 * shape.
 *
 * UTILITY, not NONE. It takes no file and names nothing belonging to the
 * caller: it makes the WHOLE medium commit, and every program on the machine
 * pays for the pause. A program that wants its own writing to be durable has
 * the operation that was built for it — storage.anchor (OP_AUTH_APP) waits
 * for a Settle cycle and this flush together and then tells the file's watchers —
 * so nothing an app can legitimately want is behind this level.
 */
static int HwDiskFlush(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 1) return ERR_INVALID_ARGUMENT;
    (void)op->params[0];
    return tagfs_flush_cache();
}


/* =========================================================================
 *  System power (reboot / shutdown — noreturn)
 * ========================================================================= */

/* kprintf, not debug_printf: this is the last line either op says before
 * system_halt()'s one-way reset/shutdown ladder runs, and debug_printf
 * compiles to nothing outside a CONFIG_DEBUG_ENABLED build (klib.h) — on
 * real hardware there is no such build, so "who asked for this" would
 * never reach the log at all. */
static int HwSystemReboot(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)op; (void)crates; (void)crate_count;
    kprintf("[HardwareDeck] reboot requested by PID %u\n",
            (ctx && ctx->proc) ? ctx->proc->pid : 0);
    system_halt(true);
    return OK; /* unreachable */
}

static int HwSystemShutdown(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                            const OpContext *ctx)
{
    (void)op; (void)crates; (void)crate_count;
    kprintf("[HardwareDeck] shutdown requested by PID %u\n",
            (ctx && ctx->proc) ? ctx->proc->pid : 0);
    system_halt(false);
    return OK; /* unreachable */
}

/* =========================================================================
 *  USB (xHCI control surface)
 * ========================================================================= */

/*
 * ‼ WHAT THIS SURFACE DELIBERATELY NO LONGER OFFERS
 *
 * `hw.usb.init` (opcode 0x90) and `hw.usb.stop` (0x93) are gone, and their
 * numbers are retired for ever — see hardware_deck.h. They were not operations.
 * They were two of the driver's own boot steps, exposed by name to anything
 * carrying the system tag.
 *
 *   init  ran xhci_init() a second time. That begins with a memset of the whole
 *         device-slot table — every live device on the machine, including the
 *         medium the filesystem is mounted from, silently becomes idle — and
 *         then finds the SAME PCI function again and brings it up a second time
 *         into a second controller structure: a second MMIO mapping, a second
 *         set of rings and DCBAA (the first leaks), a reset that drops the bus,
 *         and MSI moved to the neighbouring vector so interrupts arrive at the
 *         new structure while the old one is drained for ever by every "for
 *         each controller" loop in the driver.
 *
 *   stop  returned OK and did nothing at all, and said so in its own comment.
 *         There is no xhci_stop, and halting the controller a machine reads its
 *         volume through is not an operation anybody wants offered by name.
 *
 * What replaced them is one honest thing: hw.usb.reset now means "put this
 * controller back in service", which is the whole eleven-step repair the driver
 * already implements — not the single step it used to run.
 */

/*
 * Every USB op below names the controller it is about, as its first parameter.
 *
 * A machine has as many USB controllers as it has: one on the chipset where
 * the sockets on the case are, and very often another on a graphics card. Which
 * of them a bus walk reaches first is decided by topology — a PCIe bridge sits
 * at device 1 and the chipset controller at device 0x14, so a depth-first walk
 * meets the graphics card first. "The controller" names nothing.
 *
 * Controllers are numbered from zero in the order they were brought up, and an
 * index past the last one is refused. That is also how a caller learns how many
 * there are: ask, and be told no.
 */
static xhci_controller_t *usb_named_controller(const ManifestOp *op)
{
    if (op->param_size < 1) return NULL;
    return xhci_controller_at(op->params[0]);
}

/*
 * HW_USB_RESET  params:[u8 controller] — put this controller back in service.
 *
 * The whole repair, not the first step of it. What this used to be was a bare
 * xhci_reset: the controller was halted and cleared, and then nothing — its
 * registers left at zero, this driver's own `running` and `initialized` still
 * claiming it was healthy, `error_state` still clear so the automatic repair
 * would never come for it, and every device slot still naming a device the
 * silicon had just forgotten. One system-authorised call and the machine's USB
 * was dead until it was switched off and on, with no line anywhere saying so.
 *
 * A door, not a screwdriver from the lock.
 */
static int HwUsbReset(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                      const OpContext *ctx)
{
    (void)crates; (void)crate_count;
    xhci_controller_t *c = usb_named_controller(op);
    if (!c) return ERR_NOT_INITIALIZED;

    kprintf("[HardwareDeck] PID %u asked for the controller on %s to be put "
            "back in service\n",
            (ctx && ctx->proc) ? ctx->proc->pid : 0, c->name);

    return xhci_put_back_in_service(c) ? OK : ERR_INTERNAL;
}

static int HwUsbStart(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                      const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    xhci_controller_t *c = usb_named_controller(op);
    if (!c) return ERR_NOT_INITIALIZED;
    return xhci_start(c) == 0 ? OK : ERR_INTERNAL;
}

/* HW_USB_PORT_STATUS  params:[u8 controller, u8 port]  out_crate: u32 portsc */
static int HwUsbPortStatus(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                           const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 1) return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    if (op->param_size < 2) return ERR_INVALID_ARGUMENT;
    uint8_t port = op->params[1];
    xhci_controller_t *c = usb_named_controller(op);
    if (!c || !c->initialized) return ERR_NOT_INITIALIZED;
    if (port == 0 || port > c->max_ports) return ERR_INVALID_ARGUMENT;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(uint32_t)) return ERR_BUFFER_TOO_SMALL;
    uint32_t portsc = xhci_get_port_status(c, port);

    if (crate_write(out, ctx, &portsc, sizeof(uint32_t)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* HW_USB_PORT_RESET  params:[u8 controller, u8 port] */
static int HwUsbPortReset(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 2) return ERR_INVALID_ARGUMENT;
    uint8_t port = op->params[1];

    xhci_controller_t *c = usb_named_controller(op);
    if (!c || !c->initialized)            return ERR_NOT_INITIALIZED;
    if (port == 0 || port > c->max_ports) return ERR_INVALID_ARGUMENT;
    if (!xhci_port_has_device(c, port))   return ERR_DEVICE_NOT_READY;

    /* Starting the reset is the whole of the operation. The port announces
     * its own completion through a port-status change, and the enumeration
     * state machine is what listens for it — so this returns as soon as the
     * reset is in flight rather than holding a caller for the tens of
     * milliseconds the hardware takes. A port that needed no reset (a USB 3
     * link that trained itself) reports success without touching it. */
    int rc = xhci_port_begin_reset(c, port, NULL);
    return rc >= 0 ? OK : ERR_INTERNAL;
}

/* HW_USB_PORT_QUERY  params:[u8 controller]
 * out_crate:[u8 max_ports][u8 max_slots][u8 irq][u8 polling][u8 controllers]
 *
 * The last byte is how many controllers there are, so a caller that wants to
 * walk them all learns the number from the first one it asks rather than by
 * counting refusals. */
static int HwUsbPortQuery(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    xhci_controller_t *c = usb_named_controller(op);
    if (!c || !c->initialized) return ERR_NOT_INITIALIZED;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < 5) return ERR_BUFFER_TOO_SMALL;
    uint8_t blob[5];
    blob[0] = c->max_ports;
    blob[1] = c->max_slots;
    blob[2] = c->irq_line;
    blob[3] = c->use_polling ? 1 : 0;
    blob[4] = xhci_controller_count();
    if (crate_write(out, ctx, blob, 5) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* HW_USB_ENUM_DEVICE  params:[u8 controller, u8 port] */
static int HwUsbEnumDevice(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                           const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 2) return ERR_INVALID_ARGUMENT;
    uint8_t port = op->params[1];
    xhci_controller_t *c = usb_named_controller(op);
    if (!c || !c->running) return ERR_NOT_INITIALIZED;

    int rc = xhci_enumerate_device(c, port);
    if (rc == 0)        return OK;
    if (rc == -2 || rc == -3) return ERR_INVALID_ARGUMENT;
    return ERR_INTERNAL;
}

/* HW_USB_GET_INFO  params:[u8 slot_id]  out_crate:[u8 slot][u8 port][u8 state] */
static int HwUsbGetInfo(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 1) return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    if (op->param_size < 2) return ERR_INVALID_ARGUMENT;
    uint8_t slot_id = op->params[1];
    xhci_controller_t *c = usb_named_controller(op);
    if (!c || !c->running) return ERR_NOT_INITIALIZED;
    xhci_device_slot_t *slot = xhci_get_device_slot(c, slot_id);
    if (!slot) return ERR_DEVICE_NOT_READY;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < 3) return ERR_BUFFER_TOO_SMALL;
    uint8_t blob[3];
    blob[0] = slot->slot_id;
    blob[1] = slot->port_num;
    blob[2] = slot->state;
    if (crate_write(out, ctx, blob, 3) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* ── proving the door, on the machine it has to hold ──────────────────────────
 *
 * A boot self-test rather than a unit test, for the reason the Boardroom's and
 * the ring's already give: the thing being tested is a conversation with the
 * silicon in front of it. Whether a controller comes back from a reset, whether
 * the devices on it are found again, and whether the keyboard a person is
 * typing on survives the whole of it are facts about THIS machine and cannot be
 * established anywhere else.
 *
 * ‼ IT IS THE OP THAT IS PROVED, NOT THE DRIVER BEHIND IT. The handler is
 * reached the way the dispatcher reaches it — looked up in the registry by
 * op_kind, called with a ManifestOp built the way a caller builds one. A test
 * that called xhci_put_back_in_service directly would prove the driver and say
 * nothing about the surface, and the surface is what had two ops on it that
 * broke the machine.
 *
 * WHEN, and it is a fact rather than a clock: the first pass through the idle
 * or guide loop on which this machine has a controller in service AND a volume
 * mounted. Before that there is nothing to lose and the proof would prove
 * nothing; there is no moment to wait out and no deadline to expire.
 *
 * Gated behind USBRECOVER=on. This costs the machine every USB device it has,
 * once, and is not something a shipped build does to itself.
 */
#if CONFIG_USB_RECOVER_PROOF
void HardwareDeckUsbRecoverProof(void)
{
    static volatile uint32_t done = 0;

    if (__atomic_load_n(&done, __ATOMIC_ACQUIRE) != 0)   return;
    if (xhci_controller_count() == 0)                    return;
    if (tagfs_get_seat() == BOARDROOM_NO_SEAT)           return;
    if (__atomic_exchange_n(&done, 1u, __ATOMIC_ACQUIRE) != 0) return;

    kprintf("[USB RECOVER TEST] begin — a controller in service and a volume "
            "on seat %u\n", tagfs_get_seat());

    /* Every number this deck has withdrawn, not just the USB pair — the ports,
     * the lines and the halt went the same way and are spent for the same
     * reason (see the account above the Disk section). The only right answer
     * the registry can give about any of them is "no such op", and the one
     * that matters is the number, so they are written out as numbers here:
     * the names are gone from the header on purpose and must not come back
     * just to be tested against. */
    static const uint16_t retired[] = {
        0x20u, 0x21u, 0x22u, 0x23u, 0x24u, 0x25u,        /* hw.port.*   */
        0x30u, 0x31u, 0x32u, 0x33u, 0x34u, 0x35u, 0x36u, /* hw.irq.*    */
        0x40u,                                           /* hw.cpu.halt */
        0x90u, 0x93u,                                    /* hw.usb.init, .stop */
    };
    size_t reissued = 0;
    for (size_t i = 0; i < sizeof(retired) / sizeof(retired[0]); i++) {
        if (OpRegistryLookup(OP_KIND(HARDWARE_DECK_ID, retired[i]))) {
            kprintf("[USB RECOVER TEST] FAIL: retired opcode 0x%x is "
                    "registered again\n", (unsigned)retired[i]);
            reissued++;
        }
    }
    if (reissued == 0) {
        kprintf("[USB RECOVER TEST] PASS: all %zu withdrawn opcodes answer to "
                "nothing\n", sizeof(retired) / sizeof(retired[0]));
    }

    const OpRegistration *reg =
        OpRegistryLookup(OP_KIND(HARDWARE_DECK_ID, HW_USB_RESET));
    if (!reg || !reg->handler) {
        kprintf("[USB RECOVER TEST] FAILED: hw.usb.reset is not registered\n");
        return;
    }
    if (reg->security_mask != OP_AUTH_SYSTEM) {
        kprintf("[USB RECOVER TEST] FAIL: hw.usb.reset is not system-only\n");
    } else {
        kprintf("[USB RECOVER TEST] PASS: hw.usb.reset is registered, "
                "system-only\n");
    }

    /* One op, one parameter: which controller. Built the way a caller builds
     * one, packed so `params` really does begin where the header ends. */
    struct __packed {
        ManifestOp head;
        uint8_t    params[1];
    } req;
    req.head.op_kind    = OP_KIND(HARDWARE_DECK_ID, HW_USB_RESET);
    req.head.flags      = 0;
    req.head.in_crate   = CRATE_INDEX_NONE;
    req.head.out_crate  = CRATE_INDEX_NONE;
    req.head.param_size = 1;
    req.params[0]       = 0;                    /* the first controller */

    OpContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    int rc = reg->handler(&req.head, NULL, 0, &ctx);

    if (rc == OK) {
        kprintf("[USB RECOVER TEST] %[S]PASSED%[D]: the controller was put back "
                "in service through hw.usb.reset\n");
    } else {
        kprintf("[USB RECOVER TEST] %[R]FAILED%[D]: hw.usb.reset answered %d\n",
                rc);
    }
}
#endif /* CONFIG_USB_RECOVER_PROOF */

/* =========================================================================
 *  Debug print — serial output from userspace via kernel kprintf
 *  HW_DEBUG_PRINT  in_crate: NUL-terminated string (max 256 bytes)
 *
 *  APP, not NONE. What this writes goes into the same log ring the kernel
 *  writes its own account into, and that ring is what the Wire reads out of
 *  COM1 and what `logsave` writes down — on a board it is the only report
 *  that comes back. Every boxlib program can reach it, because boxlib's own
 *  refusal paths speak through kdbg_print and the plainest program on the
 *  volume carries `app`; nothing below that has any business in the kernel's
 *  record of itself, and a frozen process (AUTH_TAG_STOPPED) now cannot write
 *  there at all, which at NONE it could — auth_level_permits answers a NONE
 *  op before it ever looks at the stopped bit.
 * ========================================================================= */

static int HwDebugPrint(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    if (op->in_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    Crate *c = &crates[op->in_crate];
    uint64_t bytes = c->size < 256 ? c->size : 256;
    if (bytes == 0) return ERR_INVALID_ARGUMENT;

    char buf[257];
    if (crate_read(c, ctx, buf, bytes) != OK) return ERR_INVALID_ADDRESS;
    buf[bytes] = '\0';

    uint32_t pid = (ctx && ctx->proc) ? ctx->proc->pid : 0;
    kprintf("[%u] %s\n", pid, buf);
    return OK;
}

/* =========================================================================
 *  Log ring — what this kernel has said, asked for from userspace
 *
 *  HW_LOG_READ  params:    [u64 from]  — ring position to read from
 *               out_crate: [u64 oldest][u64 written][u64 copied][bytes...]
 *
 *  Self-describing on purpose: `oldest` says where the ring actually begins
 *  now, so a reader the writers overtook learns the size of its gap instead
 *  of splicing two ends of the log together and believing the seam.
 *
 *  Bounded to a page per call. LogRingRead copies with interrupts off, and
 *  the length of that window is the only price the rest of the machine pays
 *  for being asked what it said; a reader that wants more asks again.
 * ========================================================================= */

#define HW_LOG_READ_HEADER  24u
#define HW_LOG_READ_CHUNK   4096u

/* One reader for both banks: the log this boot is still writing, and the one
 * the boot before it left behind. They differ only in which function supplies
 * the bytes, and a second copy of the crate arithmetic would be a second place
 * for the header offsets to drift. */
typedef uint64_t (*LogSource)(uint64_t, void *, uint64_t, uint64_t *, uint64_t *);

static int HwLogReadFrom(LogSource source, const ManifestOp *op, Crate *crates,
                         const OpContext *ctx)
{
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 8)                return ERR_INVALID_ARGUMENT;

    uint64_t from = 0;
    for (unsigned i = 0; i < 8; i++) {
        from |= (uint64_t)op->params[i] << (i * 8);
    }

    Crate *out = &crates[op->out_crate];
    if (out->capacity < HW_LOG_READ_HEADER) return ERR_BUFFER_TOO_SMALL;

    uint64_t room = out->capacity - HW_LOG_READ_HEADER;
    if (room > HW_LOG_READ_CHUNK) room = HW_LOG_READ_CHUNK;

    uint8_t *kp = crate_out_alloc(out, HW_LOG_READ_HEADER + room);
    if (!kp) return ERR_INVALID_ADDRESS;

    uint64_t oldest = 0, written = 0;
    uint64_t copied = source(from, kp + HW_LOG_READ_HEADER, room,
                             &oldest, &written);

    memcpy(kp,      &oldest,  sizeof(uint64_t));
    memcpy(kp + 8,  &written, sizeof(uint64_t));
    memcpy(kp + 16, &copied,  sizeof(uint64_t));

    int crc = crate_out_commit(out, ctx, kp, HW_LOG_READ_HEADER + copied);
    crate_buf_free(kp);
    return (crc == OK) ? OK : ERR_INVALID_ADDRESS;
}

/* What the run before this one said. A kernel built without the carry-over
 * window (PRINTTOFILE) says so, rather than answering with an empty log that
 * reads exactly like a machine that never spoke; an empty answer from a
 * kernel that HAS the window means nothing came through the last reset,
 * which is a different fact and one the caller is left to report. */
static int HwLogPrevious(const ManifestOp *op, Crate *crates,
                         uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
#ifndef CONFIG_PRINTTOFILE
    (void)op; (void)crates; (void)ctx;
    return ERR_UNSUPPORTED;
#else
    return HwLogReadFrom(LogKeepPreviousRead, op, crates, ctx);
#endif
}

/* What this run has said so far. Every kernel keeps it: the ring is the
 * serial line's source. */
static int HwLogRead(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                     const OpContext *ctx)
{
    (void)crate_count;
    return HwLogReadFrom(LogRingRead, op, crates, ctx);
}

/* =========================================================================
 *  Registration
 * ========================================================================= */

error_t HardwareDeckRegister(void)
{
    struct {
        uint16_t    opcode;
        OpHandler   handler;
        uint32_t    auth;
        const char *name;
    } table[] = {
        /* Timer + RTC: read-only telemetry, anyone can use. */
        { HW_TIMER_GET_TICKS,    HwTimerGetTicks,    OP_AUTH_NONE,   "hw.timer.ticks"   },
        { HW_TIMER_GET_MS,       HwTimerGetMs,       OP_AUTH_NONE,   "hw.timer.ms"      },
        { HW_TIMER_GET_FREQ,     HwTimerGetFreq,     OP_AUTH_NONE,   "hw.timer.freq"    },
        { HW_RTC_GET_TIME,       HwRtcGetTime,       OP_AUTH_NONE,   "hw.rtc.time"      },
        { HW_RTC_GET_UNIX64,     HwRtcGetUnix64,     OP_AUTH_NONE,   "hw.rtc.unix64"    },
        { HW_RTC_GET_UPTIME,     HwRtcGetUptime,     OP_AUTH_NONE,   "hw.rtc.uptime"    },
        /* Disk: info is the drive as the driver already knows it, so anyone
         * may ask. flush commits the whole medium on one caller's say-so —
         * see HwDiskFlush for why that is a utility's act and not an app's. */
        { HW_DISK_INFO,          HwDiskInfo,         OP_AUTH_NONE,   "hw.disk.info"     },
        { HW_DISK_FLUSH,         HwDiskFlush,        OP_AUTH_UTILITY,"hw.disk.flush"    },
        /* VGA: cosmetic and bounded, so anyone — except paint, which is
         * neither. See HwVgaPaint. */
        { HW_VGA_PUTCHAR,        HwVgaPutChar,       OP_AUTH_NONE,   "hw.vga.putchar"   },
        { HW_VGA_PUTSTRING,      HwVgaPutString,     OP_AUTH_NONE,   "hw.vga.putstring" },
        { HW_VGA_CLEAR_SCREEN,   HwVgaClearScreen,   OP_AUTH_NONE,   "hw.vga.clear"     },
        { HW_VGA_CLEAR_LINE,     HwVgaClearLine,     OP_AUTH_NONE,   "hw.vga.clear_line"},
        { HW_VGA_CLEAR_TO_EOL,   HwVgaClearToEol,    OP_AUTH_NONE,   "hw.vga.clear_eol" },
        { HW_VGA_GET_CURSOR,     HwVgaGetCursor,     OP_AUTH_NONE,   "hw.vga.cursor.get"},
        { HW_VGA_SET_CURSOR,     HwVgaSetCursor,     OP_AUTH_NONE,   "hw.vga.cursor.set"},
        { HW_VGA_SET_COLOR,      HwVgaSetColor,      OP_AUTH_NONE,   "hw.vga.color.set" },
        { HW_VGA_GET_COLOR,      HwVgaGetColor,      OP_AUTH_NONE,   "hw.vga.color.get" },
        { HW_VGA_SCROLL_UP,      HwVgaScrollUp,      OP_AUTH_NONE,   "hw.vga.scroll"    },
        { HW_VGA_NEWLINE,        HwVgaNewline,       OP_AUTH_NONE,   "hw.vga.newline"   },
        { HW_VGA_GET_DIMENSIONS, HwVgaGetDimensions, OP_AUTH_NONE,   "hw.vga.dims"      },
        { HW_VGA_PAINT,          HwVgaPaint,         OP_AUTH_UTILITY,"hw.vga.paint"     },
        { HW_VGA_STEP_CURSOR,    HwVgaStepCursor,    OP_AUTH_NONE,   "hw.vga.step"      },
        /* System power: only system-tagged processes can reboot/shutdown. */
        { HW_SYSTEM_REBOOT,      HwSystemReboot,     OP_AUTH_SYSTEM, "hw.system.reboot"  },
        { HW_SYSTEM_SHUTDOWN,    HwSystemShutdown,   OP_AUTH_SYSTEM, "hw.system.shutdown"},
        { HW_DEBUG_PRINT,        HwDebugPrint,       OP_AUTH_APP,    "hw.debug.print"    },
        { HW_LOG_READ,           HwLogRead,          OP_AUTH_UTILITY,"hw.log.read"       },
        { HW_LOG_PREVIOUS,       HwLogPrevious,      OP_AUTH_UTILITY,"hw.log.previous"   },
        /* USB: hardware control, system+. Nothing here can leave a controller
         * halfway through anything — see the note above HwUsbReset for the two
         * that could and are gone. */
        { HW_USB_RESET,          HwUsbReset,         OP_AUTH_SYSTEM, "hw.usb.reset"     },
        { HW_USB_START,          HwUsbStart,         OP_AUTH_SYSTEM, "hw.usb.start"     },
        { HW_USB_PORT_STATUS,    HwUsbPortStatus,    OP_AUTH_SYSTEM, "hw.usb.port.status"},
        { HW_USB_PORT_RESET,     HwUsbPortReset,     OP_AUTH_SYSTEM, "hw.usb.port.reset"},
        { HW_USB_PORT_QUERY,     HwUsbPortQuery,     OP_AUTH_SYSTEM, "hw.usb.port.query"},
        { HW_USB_ENUM_DEVICE,    HwUsbEnumDevice,    OP_AUTH_SYSTEM, "hw.usb.enum"      },
        { HW_USB_GET_INFO,       HwUsbGetInfo,       OP_AUTH_SYSTEM, "hw.usb.info"      },
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        error_t rc = OpRegistryRegister(OP_KIND(DECK_HARDWARE, table[i].opcode),
                                        table[i].handler,
                                        table[i].auth,
                                        table[i].name);
        if (rc != OK && rc != ERR_ALREADY_EXISTS) {
            kprintf("[HardwareDeck] register %s failed: %s\n",
                    table[i].name, ErrorString(rc));
            return rc;
        }
    }
    debug_printf("[HardwareDeck] registered %zu named operations\n",
                 sizeof(table) / sizeof(table[0]));
    return OK;
}
