/*
 * vga.c — BoxOS userspace VGA library
 *
 * Two orthogonal optimizations:
 *
 *   1. Local state cache — getcolor/getcursor/getdimensions are served from
 *      RAM with 0 SYSCALLs.  Write ops update the cache from kernel results.
 *
 *   2. Batch sessions — vga_begin() / vga_commit() let callers queue N VGA
 *      operations into PocketRing, then one SYSCALL processes all of them.
 *      Guide already drains the entire ring per SYSCALL — we just fill it.
 *
 * Outside a batch, every write op is still 1 SYSCALL (backwards compatible).
 */

#include "box/io/vga.h"
#include "box/notify.h"
#include "box/result.h"
#include "box/string.h"

/* =========================================================================
 * Local state cache
 * ========================================================================= */

static uint8_t s_color        = 0x07;
static bool    s_color_valid   = false;

static uint8_t s_cursor_row   = 0;
static uint8_t s_cursor_col   = 0;
static bool    s_cursor_valid  = false;

static uint8_t s_dims_rows    = 0;
static uint8_t s_dims_cols    = 0;
static bool    s_dims_valid    = false;

/* =========================================================================
 * Batch session state
 * ========================================================================= */

#define VGA_BATCH_MAX 32

static bool     s_batch_active = false;
static int      s_batch_count  = 0;
static uint8_t  s_batch_data[VGA_BATCH_MAX][192];
static uint8_t  s_batch_ops[VGA_BATCH_MAX];
static uint32_t s_batch_lens[VGA_BATCH_MAX];

/* =========================================================================
 * Helpers
 * ========================================================================= */

static int vga_send_wait(uint8_t opcode, void *data, uint32_t len, Result *out) {
    pocket_send(DECK_HARDWARE, opcode, data, len);
    if (!result_wait(out, 100000)) return -ERR_TIMEOUT;
    if (out->error_code != OK)     return -(int)out->error_code;
    return 0;
}

/* Update cursor cache from result data at given offsets. */
static void cache_cursor_from(const Result *r, int row_off, int col_off) {
    if (!r->data_addr) return;
    uint8_t *d     = (uint8_t *)(uintptr_t)r->data_addr;
    s_cursor_row   = d[row_off];
    s_cursor_col   = d[col_off];
    s_cursor_valid = true;
}

/* =========================================================================
 * Batch internals
 * ========================================================================= */

static int vga_batch_push(uint8_t opcode, const void *data, uint32_t len);
static void vga_batch_process_results(Result *results, int count);

void vga_begin(void) {
    s_batch_active = true;
    s_batch_count  = 0;
}

int vga_commit(void) {
    if (!s_batch_active || s_batch_count == 0) {
        s_batch_active = false;
        s_batch_count  = 0;
        return 0;
    }

    /* Queue all pockets into PocketRing */
    for (int i = 0; i < s_batch_count; i++) {
        pocket_queue(DECK_HARDWARE, s_batch_ops[i],
                     s_batch_data[i], s_batch_lens[i]);
    }

    /* One SYSCALL — Guide drains all pockets */
    Result results[VGA_BATCH_MAX];
    int got = pocket_flush_wait(results, s_batch_count, 100000);

    /* Update caches from results */
    vga_batch_process_results(results, got);

    int committed   = s_batch_count;
    s_batch_active  = false;
    s_batch_count   = 0;
    return committed;
}

static int vga_batch_push(uint8_t opcode, const void *data, uint32_t len) {
    if (s_batch_count >= VGA_BATCH_MAX) {
        /* Auto-commit full batch, then re-enter batch mode */
        vga_commit();
        s_batch_active = true;
    }
    int idx = s_batch_count++;
    s_batch_ops[idx] = opcode;
    s_batch_lens[idx] = len;
    if (data && len > 0) {
        uint32_t copy = len > 192 ? 192 : len;
        memcpy(s_batch_data[idx], data, copy);
    }
    return 0;
}

static void vga_batch_process_results(Result *results, int count) {
    for (int i = 0; i < count; i++) {
        if (results[i].error_code != OK) continue;
        switch (s_batch_ops[i]) {
            case 0x71: /* PUTSTRING */
                cache_cursor_from(&results[i], 2, 3);
                break;
            case 0x7A: /* NEWLINE */
                cache_cursor_from(&results[i], 1, 2);
                break;
            case 0x76: /* SET_CURSOR */
                cache_cursor_from(&results[i], 1, 2);
                break;
            case 0x72: /* CLEAR_SCREEN */
                s_cursor_row = 0;
                s_cursor_col = 0;
                s_cursor_valid = true;
                break;
            case 0x79: /* SCROLL_UP */
                s_cursor_valid = false;
                break;
        }
    }
}

/* =========================================================================
 * Query operations — always from cache (0 SYSCALLs after first call)
 * ========================================================================= */

int vga_getcolor(void) {
    if (s_color_valid) return (int)s_color;

    uint8_t buf[4] = {0};
    Result result;
    int rc = vga_send_wait(0x78, buf, 4, &result);
    if (rc < 0) return rc;

    uint8_t *data = (uint8_t *)(uintptr_t)result.data_addr;
    s_color       = data[1];
    s_color_valid = true;
    return (int)s_color;
}

int vga_getcursor(vga_pos_t *pos) {
    if (!pos) return -ERR_INVALID_ARGS;

    if (s_cursor_valid) {
        pos->row = s_cursor_row;
        pos->col = s_cursor_col;
        return 0;
    }

    uint8_t buf[4] = {0};
    Result result;
    int rc = vga_send_wait(0x75, buf, 4, &result);
    if (rc < 0) return rc;

    cache_cursor_from(&result, 1, 2);
    pos->row = s_cursor_row;
    pos->col = s_cursor_col;
    return 0;
}

int vga_getdimensions(vga_dimensions_t *dims) {
    if (!dims) return -ERR_INVALID_ARGS;

    if (s_dims_valid) {
        dims->rows = s_dims_rows;
        dims->cols = s_dims_cols;
        return 0;
    }

    uint8_t buf[4] = {0};
    Result result;
    int rc = vga_send_wait(0x7B, buf, 4, &result);
    if (rc < 0) return rc;

    /* Kernel: data[0]=SUCCESS, data[1]=cols, data[2]=rows */
    uint8_t *data = (uint8_t *)(uintptr_t)result.data_addr;
    s_dims_cols   = data[1];
    s_dims_rows   = data[2];
    s_dims_valid  = true;
    dims->rows = s_dims_rows;
    dims->cols = s_dims_cols;
    return 0;
}

/* =========================================================================
 * Write operations — batch-aware with cache updates
 * ========================================================================= */

int vga_setcolor(uint8_t color) {
    s_color       = color;
    s_color_valid = true;

    if (s_batch_active)
        return vga_batch_push(0x77, &color, 1);

    Result result;
    return vga_send_wait(0x77, &color, 1, &result);
}

int vga_setcursor(uint8_t row, uint8_t col) {
    uint8_t buf[2] = {row, col};

    if (s_batch_active) {
        s_cursor_row   = row;
        s_cursor_col   = col;
        s_cursor_valid = true;
        return vga_batch_push(0x76, buf, 2);
    }

    Result result;
    int rc = vga_send_wait(0x76, buf, 2, &result);
    if (rc < 0) return rc;

    cache_cursor_from(&result, 1, 2);
    return 0;
}

int vga_putchar(char c) {
    if (!s_cursor_valid) {
        vga_pos_t pos;
        int rc = vga_getcursor(&pos);
        if (rc < 0) return rc;
    }
    int color = vga_getcolor();
    if (color < 0) color = VGA_SCHEME_DEFAULT;

    uint8_t buf[4] = {s_cursor_row, s_cursor_col, (uint8_t)c, (uint8_t)color};

    if (s_batch_active)
        return vga_batch_push(0x70, buf, 4);

    Result result;
    return vga_send_wait(0x70, buf, 4, &result);
    /* PUTCHAR restores cursor — cache stays valid */
}

int vga_puts(const char *str) {
    if (!str) return -ERR_INVALID_ARGS;

    int color = vga_getcolor();
    if (color < 0) color = VGA_SCHEME_DEFAULT;

    size_t total_len = strlen(str);
    size_t offset = 0;
    int total_written = 0;

    while (offset < total_len) {
        size_t chunk_size = total_len - offset;
        if (chunk_size > 185) chunk_size = 185;

        uint8_t putstr_buf[192];
        memset(putstr_buf, 0, sizeof(putstr_buf));
        putstr_buf[0] = (uint8_t)chunk_size;
        putstr_buf[1] = (uint8_t)color;
        memcpy(putstr_buf + 4, str + offset, chunk_size);

        if (s_batch_active) {
            vga_batch_push(0x71, putstr_buf, (uint32_t)(chunk_size + 4));
            total_written += (int)chunk_size;
        } else {
            Result result;
            int rc = vga_send_wait(0x71, putstr_buf,
                                   (uint32_t)(chunk_size + 4), &result);
            if (rc < 0) return rc;

            uint8_t *rdata = (uint8_t *)(uintptr_t)result.data_addr;
            total_written += rdata[1];
            cache_cursor_from(&result, 2, 3);
        }

        offset += chunk_size;
    }

    return total_written;
}

int vga_newline(void) {
    uint8_t buf[4] = {0};

    if (s_batch_active)
        return vga_batch_push(0x7A, buf, 4);

    Result result;
    int rc = vga_send_wait(0x7A, buf, 4, &result);
    if (rc < 0) return rc;

    cache_cursor_from(&result, 1, 2);
    return 0;
}

int vga_clear(uint8_t color) {
    if (s_batch_active) {
        s_cursor_row = 0;
        s_cursor_col = 0;
        s_cursor_valid = true;
        return vga_batch_push(0x72, &color, 1);
    }

    Result result;
    int rc = vga_send_wait(0x72, &color, 1, &result);
    if (rc < 0) return rc;

    s_cursor_row   = 0;
    s_cursor_col   = 0;
    s_cursor_valid = true;
    return 0;
}

int vga_clear_line(uint8_t row, uint8_t color) {
    uint8_t buf[2] = {row, color};

    if (s_batch_active)
        return vga_batch_push(0x73, buf, 2);

    Result result;
    return vga_send_wait(0x73, buf, 2, &result);
}

int vga_clear_to_eol(uint8_t color) {
    if (s_batch_active)
        return vga_batch_push(0x74, &color, 1);

    Result result;
    return vga_send_wait(0x74, &color, 1, &result);
}

int vga_scroll_up(uint8_t lines, uint8_t fill_color) {
    uint8_t buf[2] = {lines, fill_color};

    if (s_batch_active) {
        s_cursor_valid = false;
        return vga_batch_push(0x79, buf, 2);
    }

    Result result;
    int rc = vga_send_wait(0x79, buf, 2, &result);
    if (rc < 0) return rc;

    s_cursor_valid = false;
    return 0;
}
