/*
 * vga.c — userspace VGA wrappers (Phase 12: Manifest-only).
 *
 * Two execution modes:
 *   - Immediate: every public call builds a single-op Manifest via
 *     MfCall1 and submits + waits in line. Simple, one syscall per op.
 *   - Batched (vga_begin / vga_commit): public calls accumulate ops into
 *     a per-process scratch builder; vga_commit fires ONE multi-op
 *     Manifest containing the whole sequence. This is the BoxOS Deck
 *     dispatch advantage in practice — a printf with five colored runs
 *     used to cost up to 10 syscalls (alternating SET_COLOR + PUTSTRING);
 *     batched it's a single submit, one kernel re-entry total.
 *
 * Colour: full #RRGGBB pairs on the wire ([u32 fg][u32 bg] little-endian
 * in op params). Sentinels are resolved to concrete triples HERE, before
 * anything is cached or sent — the kernel stores concrete values only,
 * so a getter round-trips exactly what a setter shipped.
 *
 * Getter ops (vga_getcolor_rgb/cursor/dimensions) ALWAYS go immediate even
 * inside a batch — they need the kernel's answer before the caller can
 * decide what comes next. Cached values short-circuit them when
 * possible so most getters never reach the kernel anyway.
 *
 * The batch buffers are static (one per process) — boxlib runs in a
 * single thread of execution per Cabin, so contention is not possible.
 */

#include "box/vga.h"
#include "box/core/manifest.h"
#include "box/core/notify.h"
#include "box/core/result.h"
#include "box/core/strand_self.h"
#include "box/string.h"
#include "box/timeouts.h"

static Color   s_fg          = COLOR_LIGHT_GRAY;
static Color   s_bg          = COLOR_BLACK;
static bool    s_color_valid = false;

static uint8_t s_cursor_row   = 0;
static uint8_t s_cursor_col   = 0;
static bool    s_cursor_valid = false;

static uint8_t s_dims_rows    = 0;
static uint8_t s_dims_cols    = 0;
static bool    s_dims_valid   = false;

/* Hardware Deck VGA opcodes. */
#define HW_VGA_PUTCHAR        0x70
#define HW_VGA_PUTSTRING      0x71
#define HW_VGA_CLEAR_SCREEN   0x72
#define HW_VGA_CLEAR_LINE     0x73
#define HW_VGA_CLEAR_TO_EOL   0x74
#define HW_VGA_GET_CURSOR     0x75
#define HW_VGA_SET_CURSOR     0x76
#define HW_VGA_SET_COLOR      0x77
#define HW_VGA_GET_COLOR      0x78
#define HW_VGA_SCROLL_UP      0x79
#define HW_VGA_NEWLINE        0x7A
#define HW_VGA_GET_DIMENSIONS 0x7B
#define HW_VGA_PAINT          0x7C
#define HW_VGA_STEP_CURSOR    0x7D

/* Little-endian u32 into an op-param byte stream (params are unaligned). */
static inline void put_color_param(uint8_t *dst, Color c)
{
    memcpy(dst, &c, sizeof(uint32_t));
}

/* ===========================================================================
 * Batch state — sized for the console daemon's lane bursts, the heaviest
 * batcher: dozens of ConsoleRun frames (a puts + a newline each) render in
 * ONE commit, so a saturated console costs tens of submits per second, not
 * thousands (measured on print_stress: submit count is the render-side
 * wall). A typical printf still fits many times over. Hitting either
 * ceiling auto-flushes and starts a new batch so the caller never has to
 * manage capacity.
 * =========================================================================== */
#define VGA_BATCH_MANIFEST_BYTES  4096u    /* ManifestBuilder scratch */
#define VGA_BATCH_PAYLOAD_BYTES   8192u    /* PUTSTRING in-crate staging */
#define VGA_BATCH_CRATES_MAX      96u

/* Nesting depth so a wrapper that does its own vga_begin/vga_commit
 * pair composes with an outer caller's batch. Without this, an inner
 * vga_commit would flush mid-printf and the outer would re-flush an
 * empty batch — losing the syscall coalescing the outer intended.
 */
static uint32_t         s_batch_depth   = 0;
static uint8_t          s_mbuf[VGA_BATCH_MANIFEST_BYTES];
static ManifestBuilder  s_mb;
static uint8_t          s_payload[VGA_BATCH_PAYLOAD_BYTES];
static uint32_t         s_payload_used = 0;
static Crate            s_crates[VGA_BATCH_CRATES_MAX];
static uint16_t         s_crate_count  = 0;

/* The batch builder is MAIN-STRAND state. It is a set of bare statics
 * (builder, payload staging, crate table, depth counter) with no lock —
 * fine for the processes that batch today (shell, daemon: single strand),
 * fatal if concurrent strands ever interleaved ops into one builder. A
 * spawned strand therefore never batches: its begin/commit are no-ops and
 * every write op submits immediately. The value caches (colour/cursor)
 * stay shared — a spawned strand racing them can at worst leave a stale
 * cached pair (one wrong-coloured run), never a corrupted submit. */
static inline bool on_main_strand(void) { return strand_info_or_null() == NULL; }

static inline bool batch_active(void) { return s_batch_depth > 0 && on_main_strand(); }

static void batch_reset_locked(void)
{
    ManifestBuilderInit(&s_mb, s_mbuf, sizeof(s_mbuf));
    s_payload_used = 0;
    s_crate_count  = 0;
}

/* Flush a non-empty batch via one Manifest submit. Returns the kernel
 * status (OK on success). After this call the batch is empty regardless
 * of submission outcome — callers must not retain stale crate indices. */
static int batch_flush_locked(void)
{
    if (s_mb.op_count == 0) {
        batch_reset_locked();
        return OK;
    }
    if (ManifestBuilderFinalize(&s_mb) != 0) {
        batch_reset_locked();
        return -ERR_INVALID_ARGS;
    }
    Result r;
    int rc = ManifestSubmitTimeout((Manifest *)s_mbuf,
                                   s_crates, s_crate_count,
                                   &r, BOX_ANSWER_GUARANTEED);
    /* Reset BEFORE returning so the next vga_* call lands on a clean
     * builder regardless of which failure mode hit. */
    batch_reset_locked();
    /* Cursor cache is now ambiguous (no per-op output crates were
     * attached in batch mode), so invalidate it. The next getter will
     * re-fetch from the kernel. */
    s_cursor_valid = false;
    return rc;
}

static bool batch_have_capacity(uint32_t param_bytes,
                                uint32_t payload_bytes)
{
    /* Reserve sizeof(ManifestOp) + param_bytes in the builder, and
     * payload_bytes in the input staging buffer, plus one Crate slot. */
    uint32_t need_mfs = sizeof(ManifestOp) + param_bytes;
    if (s_mb.size + need_mfs > s_mb.capacity) return false;
    if (s_payload_used + payload_bytes > sizeof(s_payload)) return false;
    if ((uint32_t)s_crate_count + 1u > VGA_BATCH_CRATES_MAX) return false;
    return true;
}

/* Flush the current batch while leaving the begin/commit depth intact.
 * Used by getters and by big-string immediate fallback so accumulated
 * ops dispatch in the order the caller issued them — without forcing
 * the outer caller's vga_commit to no-op. */
static int batch_flush_preserving_depth(void)
{
    return batch_flush_locked();
}

void vga_begin(void)
{
    if (!on_main_strand()) return;   /* spawned strands never batch */
    /* First begin in a chain resets the builder; nested begin just
     * increments the depth counter so inner commits won't flush. */
    if (s_batch_depth == 0) batch_reset_locked();
    s_batch_depth++;
}

int vga_commit(void)
{
    if (!on_main_strand()) return OK;
    if (s_batch_depth == 0) return OK;
    /* Only the outermost commit fires the syscall. */
    if (--s_batch_depth > 0) return OK;
    return batch_flush_locked();
}

/* =========================================================================
 *  Cached query operations — always go immediate; getters can't be batched.
 * ========================================================================= */

int vga_getcolor_rgb(Color *fg, Color *bg)
{
    if (!fg || !bg) return -ERR_INVALID_ARGS;
    if (s_color_valid) {
        *fg = s_fg;
        *bg = s_bg;
        return 0;
    }

    /* Inside an active batch, accumulated ops have NOT been applied by
     * the kernel yet — an immediate kernel read here would return the
     * pre-batch colour and disagree with what the caller thinks they
     * just set. Flush first so kernel state catches up; the outer
     * vga_commit is still safe because the depth counter is untouched. */
    if (batch_active()) (void)batch_flush_preserving_depth();

    uint32_t out[2] = {0, 0};
    int rc = MfCall1(DECK_HARDWARE, HW_VGA_GET_COLOR,
                     NULL, 0, NULL, 0,
                     out, sizeof(out), NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    if (rc != 0) return rc < 0 ? rc : -rc;
    s_fg = out[0];
    s_bg = out[1];
    s_color_valid = true;
    *fg = s_fg;
    *bg = s_bg;
    return 0;
}

int vga_getcursor(vga_pos_t *pos)
{
    if (!pos) return -ERR_INVALID_ARGS;
    if (s_cursor_valid) {
        pos->row = s_cursor_row;
        pos->col = s_cursor_col;
        return 0;
    }
    /* Same rationale as vga_getcolor_rgb: flush before reading so the
     * cursor we report reflects every batched op the caller issued. */
    if (batch_active()) (void)batch_flush_preserving_depth();

    uint8_t out[2] = {0};
    int rc = MfCall1(DECK_HARDWARE, HW_VGA_GET_CURSOR,
                     NULL, 0, NULL, 0,
                     out, sizeof(out), NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    if (rc != 0) return rc < 0 ? rc : -rc;
    s_cursor_row = out[0];
    s_cursor_col = out[1];
    s_cursor_valid = true;
    pos->row = s_cursor_row;
    pos->col = s_cursor_col;
    return 0;
}

int vga_getdimensions(vga_dimensions_t *dims)
{
    if (!dims) return -ERR_INVALID_ARGS;
    if (s_dims_valid) {
        dims->rows = s_dims_rows;
        dims->cols = s_dims_cols;
        return 0;
    }
    /* Dimensions never change at runtime, but the same flush-before-read
     * rule keeps the contract uniform across getters. */
    if (batch_active()) (void)batch_flush_preserving_depth();

    uint8_t out[2] = {0};
    int rc = MfCall1(DECK_HARDWARE, HW_VGA_GET_DIMENSIONS,
                     NULL, 0, NULL, 0,
                     out, sizeof(out), NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    if (rc != 0) return rc < 0 ? rc : -rc;
    s_dims_cols = out[0];
    s_dims_rows = out[1];
    s_dims_valid = true;
    dims->rows = s_dims_rows;
    dims->cols = s_dims_cols;
    return 0;
}

/* =========================================================================
 *  Write operations — batched when batch_active(), immediate otherwise.
 * ========================================================================= */

/* Append an op to the active batch. Caller has already confirmed capacity.
 *
 * ‼ EVERY OP IN A CONSOLE BATCH IS OPTIONAL, AND THAT IS NOT A RELAXATION.
 *
 * A Manifest stops at its first refusal: manifest_exec.c breaks out of the
 * dispatch loop on any op that fails and is not marked OPTIONAL, so every op
 * BEHIND it is never executed. That rule is right for a Manifest that is one
 * transaction — do not write the file if the seek failed — and wrong for this
 * one, which is a rope of independent things said to a screen. The display
 * daemon packs a whole burst of output into one batch, so a single cell the
 * kernel would not draw — a rectangle that no longer fits, a snapshot that
 * found no memory — silently took the entire REST of the burst with it, up to
 * and including the prompt printed at the end of it. Nothing was said, because
 * the batch reports its first error and the daemon has no use for it.
 *
 * A console op that fails costs its own cell. It must not cost the ones after
 * it. Where the caller wants to know, the answer is still there: the submit
 * returns the first error either way (vga_commit), and the daemon now says so.
 */
static int batch_add_op(uint16_t opcode,
                        const void *params, uint16_t param_size,
                        uint16_t in_idx)
{
    return ManifestBuilderAddOp(&s_mb, DECK_HARDWARE, opcode, OP_FLAG_OPTIONAL,
                                in_idx, CRATE_INDEX_NONE,
                                params, param_size);
}

int vga_setcolor_rgb(Color fg, Color bg)
{
    fg = BoxColorResolveFg(fg);
    bg = BoxColorResolveBg(bg);
    s_fg = fg;
    s_bg = bg;
    s_color_valid = true;

    uint8_t params[8];
    put_color_param(&params[0], fg);
    put_color_param(&params[4], bg);

    if (batch_active()) {
        if (!batch_have_capacity(sizeof(params), 0)) {
            /* Auto-flush to make room, then proceed. */
            int rc = batch_flush_locked();
            if (rc != OK) return rc;
        }
        if (batch_add_op(HW_VGA_SET_COLOR, params, sizeof(params),
                         CRATE_INDEX_NONE) != 0) return -ERR_INVALID_ARGS;
        return OK;
    }

    return MfCall1(DECK_HARDWARE, HW_VGA_SET_COLOR,
                   params, sizeof(params), NULL, 0,
                   NULL, 0, NULL,
                   BOX_ANSWER_GUARANTEED, NULL);
}

int vga_setcursor(uint8_t row, uint8_t col)
{
    uint8_t params[2] = { row, col };

    if (batch_active()) {
        if (!batch_have_capacity(sizeof(params), 0)) {
            int rc = batch_flush_locked();
            if (rc != OK) return rc;
        }
        /* Invalidate the cursor cache — we cannot pre-populate (row, col)
         * because the kernel clamps out-of-bounds coordinates against the
         * real screen dimensions, and an immediate getcursor inside the
         * same batch would otherwise read the unclamped input instead of
         * the post-clamp truth. The getter's flush-preserving-depth path
         * brings the kernel state up to date when needed. */
        s_cursor_valid = false;
        return batch_add_op(HW_VGA_SET_CURSOR, params, sizeof(params),
                            CRATE_INDEX_NONE) == 0 ? OK : -ERR_INVALID_ARGS;
    }

    uint8_t out[2] = {0};
    int rc = MfCall1(DECK_HARDWARE, HW_VGA_SET_CURSOR,
                     params, 2, NULL, 0,
                     out, sizeof(out), NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    if (rc != 0) return rc < 0 ? rc : -rc;
    s_cursor_row = out[0];
    s_cursor_col = out[1];
    s_cursor_valid = true;
    return 0;
}

int vga_step_cursor(int32_t delta)
{
    if (delta == 0) return 0;

    uint8_t params[4];
    memcpy(params, &delta, sizeof(delta));

    /* Batched like any other write, and that is the point of having it: a
     * backspace is then ONE Manifest — the text that redraws the tail and the
     * step that puts the caret back — instead of a flush, a synchronous read
     * and a write. Where the caret ends up is the kernel's answer, so the
     * cached position no longer stands. */
    if (batch_active()) {
        if (!batch_have_capacity(sizeof(params), 0)) {
            int rc = batch_flush_locked();
            if (rc != OK) return rc;
        }
        s_cursor_valid = false;
        return batch_add_op(HW_VGA_STEP_CURSOR, params, sizeof(params),
                            CRATE_INDEX_NONE) == 0 ? OK : -ERR_INVALID_ARGS;
    }

    s_cursor_valid = false;
    return MfCall1(DECK_HARDWARE, HW_VGA_STEP_CURSOR,
                   params, sizeof(params), NULL, 0,
                   NULL, 0, NULL,
                   BOX_ANSWER_GUARANTEED, NULL);
}

/* Current pair for the write ops below. First use without an explicit
 * setcolor fetches the kernel's truth once (another Cabin may have set
 * the console colour before us). */
static int current_pair(Color *fg, Color *bg)
{
    if (s_color_valid) {
        *fg = s_fg;
        *bg = s_bg;
        return 0;
    }
    return vga_getcolor_rgb(fg, bg);
}

static int vga_putstring_immediate(const char *str, size_t len,
                                   Color fg, Color bg)
{
    uint8_t params[9];
    put_color_param(&params[0], fg);
    put_color_param(&params[4], bg);
    params[8] = 0;

    uint8_t out[3]    = {0};
    uint32_t out_actual = 0;
    int rc = MfCall1(DECK_HARDWARE, HW_VGA_PUTSTRING,
                     params, sizeof(params),
                     str, (uint32_t)len,
                     out, sizeof(out), &out_actual,
                     BOX_ANSWER_GUARANTEED, NULL);
    if (rc == 0 && out_actual >= 3) {
        s_cursor_row = out[1];
        s_cursor_col = out[2];
        s_cursor_valid = true;
    } else {
        s_cursor_valid = false;
    }
    return rc;
}

static int vga_putstring_batched(const char *str, size_t len,
                                 Color fg, Color bg)
{
    if (len == 0) return OK;
    uint8_t params[9];
    put_color_param(&params[0], fg);
    put_color_param(&params[4], bg);
    params[8] = 0;

    /* Need: ManifestOp + 9-byte params  +  payload_bytes (the string)
     *       + 1 Crate slot. */
    if (!batch_have_capacity(sizeof(params), (uint32_t)len)) {
        int rc = batch_flush_locked();
        if (rc != OK) return rc;
    }
    /* If a single string is bigger than the whole staging buffer, fall
     * through to the immediate path — there is no way to fit it batched. */
    if (len > sizeof(s_payload)) {
        return vga_putstring_immediate(str, len, fg, bg);
    }

    /* Copy the caller's bytes into our staging buffer so the pointer
     * remains valid through vga_commit even after the caller's frame
     * unwinds. */
    uint8_t *dst = s_payload + s_payload_used;
    memcpy(dst, str, len);
    s_payload_used += (uint32_t)len;

    CrateSetInput(&s_crates[s_crate_count], dst, (uint64_t)len);
    uint16_t in_idx = s_crate_count++;

    /* Batched putstring has no output crate (we don't collect cursor
     * updates per-op). The cursor cache is invalidated NOW (not at
     * flush) so a mid-batch getcursor flushes and re-reads truth
     * instead of returning the pre-puts row/col. */
    s_cursor_valid = false;
    if (batch_add_op(HW_VGA_PUTSTRING, params, sizeof(params),
                     in_idx) != 0) {
        /* Roll back the crate + payload reservation on failure. */
        s_crate_count--;
        s_payload_used -= (uint32_t)len;
        return -ERR_INVALID_ARGS;
    }
    return OK;
}

int vga_putchar(char c)
{
    /* Route through PUTSTRING (1-byte input) so the kernel advances
     * the cursor naturally; PUTCHAR has different cursor semantics. */
    Color fg, bg;
    if (current_pair(&fg, &bg) != 0) {
        fg = COLOR_LIGHT_GRAY;
        bg = COLOR_BLACK;
    }

    char buf[1] = { c };
    if (batch_active()) return vga_putstring_batched(buf, 1, fg, bg);
    return vga_putstring_immediate(buf, 1, fg, bg);
}

int vga_puts(const char *str)
{
    if (!str) return -ERR_INVALID_ARGS;
    Color fg, bg;
    if (current_pair(&fg, &bg) != 0) {
        fg = COLOR_LIGHT_GRAY;
        bg = COLOR_BLACK;
    }

    size_t len = strlen(str);
    if (batch_active()) {
        int rc = vga_putstring_batched(str, len, fg, bg);
        return rc == OK ? (int)len : rc;
    }
    int rc = vga_putstring_immediate(str, len, fg, bg);
    return rc < 0 ? rc : (int)len;
}

int vga_putchar_at(uint8_t row, uint8_t col, char ch, Color fg, Color bg)
{
    uint8_t params[11];
    params[0] = row;
    params[1] = col;
    params[2] = (uint8_t)ch;
    put_color_param(&params[3], BoxColorResolveFg(fg));
    put_color_param(&params[7], BoxColorResolveBg(bg));

    if (batch_active()) {
        if (!batch_have_capacity(sizeof(params), 0)) {
            int rc = batch_flush_locked();
            if (rc != OK) return rc;
        }
        return batch_add_op(HW_VGA_PUTCHAR, params, sizeof(params),
                            CRATE_INDEX_NONE) == 0 ? OK : -ERR_INVALID_ARGS;
    }

    return MfCall1(DECK_HARDWARE, HW_VGA_PUTCHAR,
                   params, sizeof(params), NULL, 0,
                   NULL, 0, NULL,
                   BOX_ANSWER_GUARANTEED, NULL);
}

int vga_newline(void)
{
    if (batch_active()) {
        if (!batch_have_capacity(0, 0)) {
            int rc = batch_flush_locked();
            if (rc != OK) return rc;
        }
        /* Newline advances row (possibly triggers scroll); we cannot
         * predict the post-op cursor without the kernel's bookkeeping
         * — invalidate so the next getcursor flushes and re-reads. */
        s_cursor_valid = false;
        return batch_add_op(HW_VGA_NEWLINE, NULL, 0, CRATE_INDEX_NONE) == 0
                   ? OK : -ERR_INVALID_ARGS;
    }

    uint8_t out[2] = {0};
    uint32_t out_actual = 0;
    int rc = MfCall1(DECK_HARDWARE, HW_VGA_NEWLINE,
                     NULL, 0, NULL, 0,
                     out, sizeof(out), &out_actual,
                     BOX_ANSWER_GUARANTEED, NULL);
    if (rc != 0) return rc < 0 ? rc : -rc;
    if (out_actual >= 2) {
        s_cursor_row = out[0];
        s_cursor_col = out[1];
        s_cursor_valid = true;
    }
    return 0;
}

int vga_paint(uint8_t row, uint8_t col, uint8_t height, uint8_t width,
              const TextCell *cells)
{
    if (!cells || height == 0 || width == 0) return -ERR_INVALID_ARGS;

    /* Never batched, and it does not need to be: a paint is already ONE op.
     * The staging buffer a batched input crate copies through is 8 KiB and a
     * frame is ten times that, so batching it would mean either a second copy
     * of every frame or a silent split. Pending ops go first so the caller's
     * order survives. */
    if (batch_active()) {
        int rc = batch_flush_preserving_depth();
        if (rc != OK) return rc;
    }

    /* Sentinels are the one thing this path does NOT resolve before the wire.
     * Every other op carries one pair and resolving it here costs two
     * branches; a frame carries six thousand, and resolving them would mean
     * either walking the caller's buffer (it is const, and theirs) or copying
     * the whole frame to walk the copy. The kernel already makes one snapshot
     * of these bytes to get them out of user memory — it resolves as it
     * paints, so the Canvas still never sees a sentinel. */
    uint32_t n = (uint32_t)height * (uint32_t)width;

    uint8_t params[4];
    params[0] = row;
    params[1] = col;
    params[2] = height;
    params[3] = width;

    return MfCall1(DECK_HARDWARE, HW_VGA_PAINT,
                   params, sizeof(params),
                   cells, n * (uint32_t)sizeof(TextCell),
                   NULL, 0, NULL,
                   BOX_ANSWER_GUARANTEED, NULL);
}

int vga_clear_rgb(Color fg, Color bg)
{
    uint8_t params[8];
    put_color_param(&params[0], BoxColorResolveFg(fg));
    put_color_param(&params[4], BoxColorResolveBg(bg));

    if (batch_active()) {
        if (!batch_have_capacity(sizeof(params), 0)) {
            int rc = batch_flush_locked();
            if (rc != OK) return rc;
        }
        s_cursor_row = 0;
        s_cursor_col = 0;
        s_cursor_valid = true;
        return batch_add_op(HW_VGA_CLEAR_SCREEN, params, sizeof(params),
                            CRATE_INDEX_NONE) == 0 ? OK : -ERR_INVALID_ARGS;
    }

    int rc = MfCall1(DECK_HARDWARE, HW_VGA_CLEAR_SCREEN,
                     params, sizeof(params), NULL, 0,
                     NULL, 0, NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    if (rc != 0) return rc < 0 ? rc : -rc;
    s_cursor_row = 0;
    s_cursor_col = 0;
    s_cursor_valid = true;
    return 0;
}

int vga_clear_line_rgb(uint8_t row, Color fg, Color bg)
{
    uint8_t params[9];
    params[0] = row;
    put_color_param(&params[1], BoxColorResolveFg(fg));
    put_color_param(&params[5], BoxColorResolveBg(bg));

    if (batch_active()) {
        if (!batch_have_capacity(sizeof(params), 0)) {
            int rc = batch_flush_locked();
            if (rc != OK) return rc;
        }
        return batch_add_op(HW_VGA_CLEAR_LINE, params, sizeof(params),
                            CRATE_INDEX_NONE) == 0 ? OK : -ERR_INVALID_ARGS;
    }

    return MfCall1(DECK_HARDWARE, HW_VGA_CLEAR_LINE,
                   params, sizeof(params), NULL, 0,
                   NULL, 0, NULL,
                   BOX_ANSWER_GUARANTEED, NULL);
}

int vga_clear_to_eol(void)
{
    if (batch_active()) {
        if (!batch_have_capacity(0, 0)) {
            int rc = batch_flush_locked();
            if (rc != OK) return rc;
        }
        return batch_add_op(HW_VGA_CLEAR_TO_EOL, NULL, 0, CRATE_INDEX_NONE) == 0
                   ? OK : -ERR_INVALID_ARGS;
    }

    return MfCall1(DECK_HARDWARE, HW_VGA_CLEAR_TO_EOL,
                   NULL, 0, NULL, 0,
                   NULL, 0, NULL,
                   BOX_ANSWER_GUARANTEED, NULL);
}

int vga_scroll_up(void)
{
    if (batch_active()) {
        if (!batch_have_capacity(0, 0)) {
            int rc = batch_flush_locked();
            if (rc != OK) return rc;
        }
        s_cursor_valid = false;
        return batch_add_op(HW_VGA_SCROLL_UP, NULL, 0, CRATE_INDEX_NONE) == 0
                   ? OK : -ERR_INVALID_ARGS;
    }

    int rc = MfCall1(DECK_HARDWARE, HW_VGA_SCROLL_UP,
                     NULL, 0, NULL, 0,
                     NULL, 0, NULL,
                     BOX_ANSWER_GUARANTEED, NULL);
    if (rc != 0) return rc < 0 ? rc : -rc;
    s_cursor_valid = false;
    return 0;
}
