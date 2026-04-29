/*
 * vga.c — userspace VGA wrappers (Phase 12: Manifest-only).
 *
 * Every public function builds a 1-op Manifest via MfCall1. The Hardware Deck
 * ops (hw.vga.*) take their parameters in op->params and route variable data
 * through Crates, so the legacy 192-byte cargo cult is gone.
 *
 * The local cache (color, cursor, dims) survives the migration unchanged —
 * cached reads still bypass the kernel entirely.
 *
 * vga_begin/vga_commit remain in the public API but are now transparent: each
 * op already flows through one syscall, and the unbounded PUTSTRING op makes
 * per-chunk batching obsolete. A future optimisation can re-introduce true
 * multi-op Manifest batching; for now correctness > throughput.
 */

#include "box/vga.h"
#include "box/core/manifest.h"
#include "box/core/notify.h"
#include "box/core/result.h"
#include "box/string.h"

#define VGA_TIMEOUT_MS 100000u

static uint8_t s_color        = 0x07;
static bool    s_color_valid  = false;

static uint8_t s_cursor_row   = 0;
static uint8_t s_cursor_col   = 0;
static bool    s_cursor_valid = false;

static uint8_t s_dims_rows    = 0;
static uint8_t s_dims_cols    = 0;
static bool    s_dims_valid   = false;

static bool    s_batch_active = false;

void vga_begin(void) { s_batch_active = true;  }
int  vga_commit(void) { s_batch_active = false; return 0; }

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

/* =========================================================================
 *  Cached query operations
 * ========================================================================= */

int vga_getcolor(void)
{
    if (s_color_valid) return (int)s_color;

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
 *  Write operations
 * ========================================================================= */

int vga_setcolor(uint8_t color)
{
    s_color = color;
    s_color_valid = true;
    uint8_t params[1] = { color };
    return MfCall1(DECK_HARDWARE, HW_VGA_SET_COLOR,
                   params, 1, NULL, 0,
                   NULL, 0, NULL,
                   VGA_TIMEOUT_MS, NULL);
}

int vga_setcursor(uint8_t row, uint8_t col)
{
    uint8_t params[2] = { row, col };
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

int vga_putchar(char c)
{
    if (!s_cursor_valid) {
        vga_pos_t pos;
        int rc = vga_getcursor(&pos);
        if (rc < 0) return rc;
    }
    int color = vga_getcolor();
    if (color < 0) color = VIDEO_ATTR_DEFAULT;

    uint8_t params[4] = { s_cursor_row, s_cursor_col, (uint8_t)c, (uint8_t)color };
    return MfCall1(DECK_HARDWARE, HW_VGA_PUTCHAR,
                   params, 4, NULL, 0,
                   NULL, 0, NULL,
                   VGA_TIMEOUT_MS, NULL);
}

int vga_puts(const char *str)
{
    if (!str) return -ERR_INVALID_ARGS;

    int color = vga_getcolor();
    if (color < 0) color = VIDEO_ATTR_DEFAULT;

    size_t len = strlen(str);
    uint8_t params[2] = { (uint8_t)color, 0 /* flags */ };

    /* New op: arbitrary length. The kernel reads in_crate.size bytes. */
    uint8_t out[3] = {0};
    uint32_t out_actual = 0;
    int rc = MfCall1(DECK_HARDWARE, HW_VGA_PUTSTRING,
                     params, 2,
                     str, (uint32_t)len,
                     out, sizeof(out), &out_actual,
                     VGA_TIMEOUT_MS, NULL);
    if (rc != 0) return rc < 0 ? rc : -rc;

    if (out_actual >= 3) {
        s_cursor_row = out[1];
        s_cursor_col = out[2];
        s_cursor_valid = true;
    } else {
        s_cursor_valid = false;
    }
    return (int)len;
}

int vga_newline(void)
{
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
    return MfCall1(DECK_HARDWARE, HW_VGA_CLEAR_LINE,
                   params, 2, NULL, 0,
                   NULL, 0, NULL,
                   VGA_TIMEOUT_MS, NULL);
}

int vga_clear_to_eol(uint8_t color)
{
    /* The new op ignores color; we keep the parameter in the public API for
     * source compat with callers that pass it. */
    (void)color;
    return MfCall1(DECK_HARDWARE, HW_VGA_CLEAR_TO_EOL,
                   NULL, 0, NULL, 0,
                   NULL, 0, NULL,
                   VGA_TIMEOUT_MS, NULL);
}

int vga_scroll_up(uint8_t lines, uint8_t fill_color)
{
    /* New op scrolls one line and ignores fill_color; preserve API surface. */
    (void)lines; (void)fill_color;
    int rc = MfCall1(DECK_HARDWARE, HW_VGA_SCROLL_UP,
                     NULL, 0, NULL, 0,
                     NULL, 0, NULL,
                     VGA_TIMEOUT_MS, NULL);
    if (rc != 0) return rc < 0 ? rc : -rc;
    s_cursor_valid = false;
    return 0;
}
