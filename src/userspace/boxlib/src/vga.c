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
 * Getter ops (vga_getcolor/cursor/dimensions) ALWAYS go immediate even
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
#include "box/string.h"
#include "box/timeouts.h"

#define VGA_TIMEOUT_MS BOX_TIMEOUT_FAST_MS

static uint8_t s_color        = 0x07;
static bool    s_color_valid  = false;

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

/* ===========================================================================
 * Batch state — sized so a typical printf (≈8 colored runs of ≤96 bytes) fits
 * comfortably in one commit. Hitting either ceiling auto-flushes and starts a
 * new batch so the caller never has to manage capacity.
 * =========================================================================== */
#define VGA_BATCH_MANIFEST_BYTES  1024u    /* ManifestBuilder scratch */
#define VGA_BATCH_PAYLOAD_BYTES   2048u    /* PUTSTRING in-crate arena */
#define VGA_BATCH_CRATES_MAX      32u

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

static inline bool batch_active(void) { return s_batch_depth > 0; }

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
                                   &r, VGA_TIMEOUT_MS);
    /* Reset BEFORE returning so the next vga_* call lands on a clean
     * builder regardless of which failure mode hit. */
    batch_reset_locked();
    /* Cursor and color caches are now ambiguous (no per-op output crates
     * were attached in batch mode), so invalidate them. The next getter
     * will re-fetch from the kernel. */
    s_cursor_valid = false;
    return rc;
}

static bool batch_have_capacity(uint32_t param_bytes,
                                uint32_t payload_bytes)
{
    /* Reserve sizeof(ManifestOp) + param_bytes in the builder, and
     * payload_bytes in the input arena, plus one Crate slot. */
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
    /* First begin in a chain resets the builder; nested begin just
     * increments the depth counter so inner commits won't flush. */
    if (s_batch_depth == 0) batch_reset_locked();
    s_batch_depth++;
}

int vga_commit(void)
{
    if (s_batch_depth == 0) return OK;
    /* Only the outermost commit fires the syscall. */
    if (--s_batch_depth > 0) return OK;
    return batch_flush_locked();
}

/* =========================================================================
 *  Cached query operations — always go immediate; getters can't be batched.
 * ========================================================================= */

int vga_getcolor(void)
{
    if (s_color_valid) return (int)s_color;

    /* Inside an active batch, accumulated ops have NOT been applied by
     * the kernel yet — an immediate kernel read here would return the
     * pre-batch color and disagree with what the caller thinks they
     * just set. Flush first so kernel state catches up; the outer
     * vga_commit is still safe because the depth counter is untouched. */
    if (batch_active()) (void)batch_flush_preserving_depth();

    uint8_t out = 0;
    int rc = MfCall1(DECK_HARDWARE, HW_VGA_GET_COLOR,
                     NULL, 0, NULL, 0,
                     &out, sizeof(out), NULL,
                     VGA_TIMEOUT_MS, NULL);
    if (rc != 0) return rc < 0 ? rc : -rc;
    s_color = out;
    s_color_valid = true;
    return (int)s_color;
}

int vga_getcursor(vga_pos_t *pos)
{
    if (!pos) return -ERR_INVALID_ARGS;
    if (s_cursor_valid) {
        pos->row = s_cursor_row;
        pos->col = s_cursor_col;
        return 0;
    }
    /* Same rationale as vga_getcolor: flush before reading so the
     * cursor we report reflects every batched op the caller issued. */
    if (batch_active()) (void)batch_flush_preserving_depth();

    uint8_t out[2] = {0};
    int rc = MfCall1(DECK_HARDWARE, HW_VGA_GET_CURSOR,
                     NULL, 0, NULL, 0,
                     out, sizeof(out), NULL,
                     VGA_TIMEOUT_MS, NULL);
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
                     VGA_TIMEOUT_MS, NULL);
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

/* Append an op with no input crate to the active batch. Caller has
 * already confirmed batch capacity. */
static int batch_add_op(uint16_t opcode,
                        const void *params, uint16_t param_size,
                        uint16_t in_idx)
{
    return ManifestBuilderAddOp(&s_mb, DECK_HARDWARE, opcode, 0,
                                in_idx, CRATE_INDEX_NONE,
                                params, param_size);
}

int vga_setcolor(uint8_t color)
{
    s_color = color;
    s_color_valid = true;

    uint8_t params[1] = { color };

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
                   params, 1, NULL, 0,
                   NULL, 0, NULL,
                   VGA_TIMEOUT_MS, NULL);
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
                     VGA_TIMEOUT_MS, NULL);
    if (rc != 0) return rc < 0 ? rc : -rc;
    s_cursor_row = out[0];
    s_cursor_col = out[1];
    s_cursor_valid = true;
    return 0;
}

static int vga_putstring_immediate(const char *str, size_t len, uint8_t color)
{
    uint8_t params[2] = { color, 0 };
    uint8_t out[3]    = {0};
    uint32_t out_actual = 0;
    int rc = MfCall1(DECK_HARDWARE, HW_VGA_PUTSTRING,
                     params, sizeof(params),
                     str, (uint32_t)len,
                     out, sizeof(out), &out_actual,
                     VGA_TIMEOUT_MS, NULL);
    if (rc == 0 && out_actual >= 3) {
        s_cursor_row = out[1];
        s_cursor_col = out[2];
        s_cursor_valid = true;
    } else {
        s_cursor_valid = false;
    }
    return rc;
}

static int vga_putstring_batched(const char *str, size_t len, uint8_t color)
{
    if (len == 0) return OK;
    uint8_t params[2] = { color, 0 };

    /* Need: ManifestOp + 2-byte params  +  payload_bytes (the string)
     *       + 1 Crate slot. */
    if (!batch_have_capacity(sizeof(params), (uint32_t)len)) {
        int rc = batch_flush_locked();
        if (rc != OK) return rc;
    }
    /* If a single string is bigger than the whole arena, fall through
     * to the immediate path — there is no way to fit it batched. */
    if (len > sizeof(s_payload)) {
        return vga_putstring_immediate(str, len, color);
    }

    /* Copy the caller's bytes into our arena so the pointer remains
     * valid through vga_commit even after the caller's frame unwinds. */
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
    int color = vga_getcolor();
    if (color < 0) color = VIDEO_ATTR_DEFAULT;

    char buf[1] = { c };
    if (batch_active()) return vga_putstring_batched(buf, 1, (uint8_t)color);
    return vga_putstring_immediate(buf, 1, (uint8_t)color);
}

int vga_puts(const char *str)
{
    if (!str) return -ERR_INVALID_ARGS;
    int color = vga_getcolor();
    if (color < 0) color = VIDEO_ATTR_DEFAULT;

    size_t len = strlen(str);
    if (batch_active()) {
        int rc = vga_putstring_batched(str, len, (uint8_t)color);
        return rc == OK ? (int)len : rc;
    }
    int rc = vga_putstring_immediate(str, len, (uint8_t)color);
    return rc < 0 ? rc : (int)len;
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
                     VGA_TIMEOUT_MS, NULL);
    if (rc != 0) return rc < 0 ? rc : -rc;
    if (out_actual >= 2) {
        s_cursor_row = out[0];
        s_cursor_col = out[1];
        s_cursor_valid = true;
    }
    return 0;
}

int vga_clear(uint8_t color)
{
    uint8_t params[1] = { color };

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
                     params, 1, NULL, 0,
                     NULL, 0, NULL,
                     VGA_TIMEOUT_MS, NULL);
    if (rc != 0) return rc < 0 ? rc : -rc;
    s_cursor_row = 0;
    s_cursor_col = 0;
    s_cursor_valid = true;
    return 0;
}

int vga_clear_line(uint8_t row, uint8_t color)
{
    uint8_t params[2] = { row, color };

    if (batch_active()) {
        if (!batch_have_capacity(sizeof(params), 0)) {
            int rc = batch_flush_locked();
            if (rc != OK) return rc;
        }
        return batch_add_op(HW_VGA_CLEAR_LINE, params, sizeof(params),
                            CRATE_INDEX_NONE) == 0 ? OK : -ERR_INVALID_ARGS;
    }

    return MfCall1(DECK_HARDWARE, HW_VGA_CLEAR_LINE,
                   params, 2, NULL, 0,
                   NULL, 0, NULL,
                   VGA_TIMEOUT_MS, NULL);
}

int vga_clear_to_eol(uint8_t color)
{
    (void)color;
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
                   VGA_TIMEOUT_MS, NULL);
}

int vga_scroll_up(uint8_t lines, uint8_t fill_color)
{
    (void)lines; (void)fill_color;

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
                     VGA_TIMEOUT_MS, NULL);
    if (rc != 0) return rc < 0 ? rc : -rc;
    s_cursor_valid = false;
    return 0;
}
