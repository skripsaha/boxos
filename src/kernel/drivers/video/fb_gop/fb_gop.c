/*
 * fb_gop.c — GOP framebuffer text console driver for BoxOS
 *
 * Architecture:
 *   cell buffer (char + attr)
 *         │
 *         ▼
 *   shadow buffer (RAM pixels) ──► VRAM (WC-mapped)
 *
 * All rendering writes to the shadow buffer first.  VRAM is updated via
 * explicit flush operations using SSE2 non-temporal writes.
 *
 * Batch mode: when enabled, VRAM flushes are deferred.  A dirty scanline
 * range tracks which rows need updating.  BatchEnd flushes the dirty region
 * in a single blit — dramatically faster than per-character flushes.
 *
 * Degraded mode: if shadow buffer allocation fails, rendering goes directly
 * to VRAM with volatile writes.  Batch mode is unavailable.
 */

#include "fb_gop.h"
#include "../ops.h"
#include "fb_pixel.h"
#include "../font/vga_font.h"
#include "vmm.h"
#include "serial.h"
#include "video_colors.h"

/* =========================================================================
 * Types
 * ========================================================================= */

typedef struct {
    char    ch;
    uint8_t attr;
} TextCell;

typedef struct {
    volatile uint8_t *virt;     /* VRAM virtual address (WC-mapped)         */
    uint8_t          *shadow;   /* RAM shadow buffer (NULL if degraded)     */
    TextCell         *cells;    /* Text cell buffer (rows × cols)           */
    uint32_t          width;    /* Framebuffer pixel width                  */
    uint32_t          height;   /* Framebuffer pixel height                 */
    uint32_t          stride;   /* Bytes per scanline                       */
    uint32_t          format;   /* FB_FORMAT_RGB / BGR / BGRX               */
    uint32_t          cols;     /* Text columns  (width  / FONT_W)          */
    uint32_t          rows;     /* Text rows     (height / FONT_H)          */
    uint32_t          text_h;   /* Visible text pixel height (rows*FONT_H)  */
} FbState;

/* =========================================================================
 * State
 * ========================================================================= */

static FbState  s_fb       = {0};
static uint32_t s_col      = 0;
static uint32_t s_row      = 0;

/* Batch mode: defer VRAM flushes and track dirty scanline range. */
static int      s_batch    = 0;
static uint32_t s_dirty_y0 = 0xFFFFFFFFU;   /* top dirty pixel row           */
static uint32_t s_dirty_y1 = 0;             /* bottom dirty pixel row (excl.) */

/* =========================================================================
 * Fast memory operations (kernel-mode, no libc)
 * ========================================================================= */

static void FbMemMove(void *dst, const void *src, size_t n)
{
    size_t qwords = n >> 3;
    size_t tail   = n &  7;
    __asm__ volatile("rep movsq"
                     : "+D"(dst), "+S"(src), "+c"(qwords)
                     :: "memory");
    __asm__ volatile("rep movsb"
                     : "+D"(dst), "+S"(src), "+c"(tail)
                     :: "memory");
}

static void FbMemSet0(void *dst, size_t n)
{
    size_t qwords = n >> 3;
    size_t tail   = n &  7;
    __asm__ volatile("xorq %%rax,%%rax; rep stosq"
                     : "+D"(dst), "+c"(qwords)
                     :: "rax", "memory");
    __asm__ volatile("xorb %%al,%%al; rep stosb"
                     : "+D"(dst), "+c"(tail)
                     :: "rax", "memory");
}

/*
 * FbMemSet32 — fill count uint32_t values with val.
 * Uses rep stosl (AT&T for stosd): stores EAX to [RDI], increments RDI by 4.
 */
static void FbMemSet32(void *dst, uint32_t val, size_t count)
{
    __asm__ volatile("rep stosl"
                     : "+D"(dst), "+c"(count)
                     : "a"(val)
                     : "memory");
}

/*
 * FbBlitNT — SSE2 non-temporal blit: shadow → VRAM.
 * Bypasses CPU cache; optimal for WC-mapped framebuffer memory.
 * Copies 64 bytes per iteration (4 × 128-bit movntdq).
 */
__attribute__((target("sse2")))
static void FbBlitNT(volatile uint8_t *dst, const uint8_t *src, size_t bytes)
{
    size_t i = 0;
    for (; i + 64 <= bytes; i += 64) {
        __asm__ volatile(
            "movdqu    (%1),  %%xmm0\n\t"
            "movdqu  16(%1),  %%xmm1\n\t"
            "movdqu  32(%1),  %%xmm2\n\t"
            "movdqu  48(%1),  %%xmm3\n\t"
            "movntdq %%xmm0,   (%0)\n\t"
            "movntdq %%xmm1, 16(%0)\n\t"
            "movntdq %%xmm2, 32(%0)\n\t"
            "movntdq %%xmm3, 48(%0)\n\t"
            :
            : "r"((uint8_t *)dst + i), "r"(src + i)
            : "xmm0", "xmm1", "xmm2", "xmm3", "memory"
        );
    }
    for (; i + 4 <= bytes; i += 4)
        *(volatile uint32_t *)((uint8_t *)dst + i) = *(const uint32_t *)(src + i);
    for (; i < bytes; i++)
        *((volatile uint8_t *)dst + i) = src[i];
    __asm__ volatile("sfence" ::: "memory");
}

/* =========================================================================
 * Dirty tracking + flush
 * ========================================================================= */

static inline void FbDirtyMark(uint32_t y, uint32_t h)
{
    if (y < s_dirty_y0)       s_dirty_y0 = y;
    if (y + h > s_dirty_y1)   s_dirty_y1 = y + h;
}

static void FbFlushRegion(uint32_t y, uint32_t h)
{
    if (!s_fb.shadow || h == 0) return;
    if (y >= s_fb.height) return;
    if (y + h > s_fb.height) h = s_fb.height - y;
    FbBlitNT(s_fb.virt  + (size_t)y * s_fb.stride,
             s_fb.shadow + (size_t)y * s_fb.stride,
             (size_t)h * s_fb.stride);
}

static void FbFlushDirty(void)
{
    if (s_dirty_y0 < s_dirty_y1)
        FbFlushRegion(s_dirty_y0, s_dirty_y1 - s_dirty_y0);
    s_dirty_y0 = 0xFFFFFFFFU;
    s_dirty_y1 = 0;
}

/* Request VRAM update for a region.  Batch mode: defer.  Otherwise: immediate. */
static inline void FbRequestFlush(uint32_t y, uint32_t h)
{
    if (s_batch)
        FbDirtyMark(y, h);
    else
        FbFlushRegion(y, h);
}

/* =========================================================================
 * Character rendering
 *
 * FbRenderGlyph draws one 8×16 glyph into the shadow buffer (or directly
 * to VRAM in degraded mode).  Pixel values are pre-encoded ONCE per call,
 * and the 8-pixel row is fully unrolled — no inner loop, no per-pixel
 * FbPixelEncode.  This is ~4× fewer operations than the old approach.
 * ========================================================================= */

static void FbRenderGlyph(uint32_t col, uint32_t row, char ch, uint8_t attr)
{
    uint32_t fg = FbPixelEncode(vga_palette[attr & 0x0F], s_fb.format);
    uint32_t bg = FbPixelEncode(vga_palette[(attr >> 4) & 0x0F], s_fb.format);

    uint32_t px = col * FONT_W;
    uint32_t py = row * FONT_H;

    const unsigned char *glyph = vga_font_8x16[(unsigned char)ch];

    if (s_fb.shadow) {
        for (uint32_t y = 0; y < FONT_H; y++) {
            uint32_t *p = (uint32_t *)(s_fb.shadow
                          + (size_t)(py + y) * s_fb.stride
                          + (size_t)px * 4);
            uint8_t bits = glyph[y];
            p[0] = (bits & 0x80) ? fg : bg;
            p[1] = (bits & 0x40) ? fg : bg;
            p[2] = (bits & 0x20) ? fg : bg;
            p[3] = (bits & 0x10) ? fg : bg;
            p[4] = (bits & 0x08) ? fg : bg;
            p[5] = (bits & 0x04) ? fg : bg;
            p[6] = (bits & 0x02) ? fg : bg;
            p[7] = (bits & 0x01) ? fg : bg;
        }
    } else {
        /* Degraded: write directly to VRAM (volatile) */
        for (uint32_t y = 0; y < FONT_H; y++) {
            volatile uint32_t *p = (volatile uint32_t *)(s_fb.virt
                                   + (size_t)(py + y) * s_fb.stride
                                   + (size_t)px * 4);
            uint8_t bits = glyph[y];
            p[0] = (bits & 0x80) ? fg : bg;
            p[1] = (bits & 0x40) ? fg : bg;
            p[2] = (bits & 0x20) ? fg : bg;
            p[3] = (bits & 0x10) ? fg : bg;
            p[4] = (bits & 0x08) ? fg : bg;
            p[5] = (bits & 0x04) ? fg : bg;
            p[6] = (bits & 0x02) ? fg : bg;
            p[7] = (bits & 0x01) ? fg : bg;
        }
    }
}

/*
 * FbFlushChar — flush only a single character's pixel region to VRAM.
 *
 * Copies exactly FONT_W × FONT_H pixels (8×16 = 512 bytes) using 64-bit
 * writes: 4 per row × 16 rows = 64 writes total.  This is 240× less data
 * than blitting full scanlines (FONT_H × stride ≈ 120 KB at 1920 width).
 */
static void FbFlushChar(uint32_t px, uint32_t py)
{
    if (!s_fb.shadow) return;
    for (uint32_t y = 0; y < FONT_H; y++) {
        size_t off = (size_t)(py + y) * s_fb.stride + (size_t)px * 4;
        const uint64_t *src     = (const uint64_t *)(s_fb.shadow + off);
        volatile uint64_t *dst  = (volatile uint64_t *)(s_fb.virt + off);
        dst[0] = src[0];   /* pixels 0-1 */
        dst[1] = src[1];   /* pixels 2-3 */
        dst[2] = src[2];   /* pixels 4-5 */
        dst[3] = src[3];   /* pixels 6-7 */
    }
}

/*
 * FbDrawChar — render glyph, update cell buffer, flush.
 *
 * In batch mode: only marks dirty rows (no VRAM write).
 * Outside batch: flushes the character's pixel region directly — 512 bytes,
 * not the full scanline width.
 */
static void FbDrawChar(uint32_t col, uint32_t row, char ch, uint8_t attr)
{
    if (col >= s_fb.cols || row >= s_fb.rows) return;

    FbRenderGlyph(col, row, ch, attr);

    if (s_fb.cells)
        s_fb.cells[row * s_fb.cols + col] = (TextCell){ch, attr};

    if (s_batch) {
        FbDirtyMark(row * FONT_H, FONT_H);
    } else {
        FbFlushChar(col * FONT_W, row * FONT_H);
    }
}

/* =========================================================================
 * Fill operations — fast region fills using FbMemSet0 / FbMemSet32
 * ========================================================================= */

/*
 * FbFillStrip — fill pixel rectangle [px_x0, px_x1) × [py, py+h) with color.
 * Uses FbMemSet0 for black, FbMemSet32 for any other color.
 */
static void FbFillStrip(uint32_t px_x0, uint32_t px_x1,
                        uint32_t py, uint32_t h, uint32_t encoded_pixel)
{
    uint32_t pixel_count = px_x1 - px_x0;
    if (pixel_count == 0) return;

    if (s_fb.shadow) {
        for (uint32_t y = py; y < py + h && y < s_fb.height; y++) {
            uint32_t *p = (uint32_t *)(s_fb.shadow
                          + (size_t)y * s_fb.stride
                          + (size_t)px_x0 * 4);
            if (encoded_pixel == 0)
                FbMemSet0(p, (size_t)pixel_count * 4);
            else
                FbMemSet32(p, encoded_pixel, pixel_count);
        }
    } else {
        for (uint32_t y = py; y < py + h && y < s_fb.height; y++) {
            volatile uint32_t *p = (volatile uint32_t *)(s_fb.virt
                                   + (size_t)y * s_fb.stride
                                   + (size_t)px_x0 * 4);
            for (uint32_t x = 0; x < pixel_count; x++)
                p[x] = encoded_pixel;
        }
    }
}

/* =========================================================================
 * DisplayOps implementation
 * ========================================================================= */

static void FbScrollUp(void)
{
    size_t row_bytes = (size_t)FONT_H * s_fb.stride;
    size_t move_sz   = (size_t)(s_fb.rows - 1) * row_bytes;

    if (s_fb.shadow) {
        FbMemMove(s_fb.shadow, s_fb.shadow + row_bytes, move_sz);
        FbMemSet0(s_fb.shadow + move_sz, row_bytes);
    } else {
        uint8_t *base = (uint8_t *)s_fb.virt;
        FbMemMove(base, base + row_bytes, move_sz);
        FbMemSet0(base + move_sz, row_bytes);
    }

    /* Shift cell buffer up by one row */
    if (s_fb.cells) {
        size_t cell_row_bytes = (size_t)s_fb.cols * sizeof(TextCell);
        FbMemMove(s_fb.cells,
                  (uint8_t *)s_fb.cells + cell_row_bytes,
                  cell_row_bytes * (s_fb.rows - 1));
        TextCell empty = {' ', VIDEO_ATTR_DEFAULT};
        TextCell *bot  = s_fb.cells + (s_fb.rows - 1) * s_fb.cols;
        for (uint32_t c = 0; c < s_fb.cols; c++)
            bot[c] = empty;
    }

    /* Flush only the text area, not the unused bottom pixels */
    FbRequestFlush(0, s_fb.text_h);
}

static void FbClearScreen(void)
{
    size_t total = (size_t)s_fb.height * s_fb.stride;
    if (s_fb.shadow)
        FbMemSet0(s_fb.shadow, total);
    else
        FbMemSet0((void *)s_fb.virt, total);

    if (s_fb.cells) {
        TextCell empty = {' ', VIDEO_ATTR_DEFAULT};
        uint32_t count = s_fb.rows * s_fb.cols;
        for (uint32_t i = 0; i < count; i++)
            s_fb.cells[i] = empty;
    }

    s_col = 0;
    s_row = 0;
    FbRequestFlush(0, s_fb.height);
}

static void FbClearLine(int line)
{
    if (line < 0 || (uint32_t)line >= s_fb.rows) return;

    uint32_t py     = (uint32_t)line * FONT_H;
    uint32_t bg_rgb = vga_palette[VIDEO_BG(VIDEO_ATTR_DEFAULT)];
    uint32_t bg     = FbPixelEncode(bg_rgb, s_fb.format);

    FbFillStrip(0, s_fb.width, py, FONT_H, bg);

    if (s_fb.cells) {
        TextCell empty = {' ', VIDEO_ATTR_DEFAULT};
        TextCell *row  = s_fb.cells + (uint32_t)line * s_fb.cols;
        for (uint32_t c = 0; c < s_fb.cols; c++)
            row[c] = empty;
    }

    FbRequestFlush(py, FONT_H);
}

static void FbClearToEol(uint8_t attr)
{
    if (s_col >= s_fb.cols) return;

    uint32_t bg_rgb = vga_palette[(attr >> 4) & 0x0F];
    uint32_t bg     = FbPixelEncode(bg_rgb, s_fb.format);
    uint32_t py     = s_row * FONT_H;

    FbFillStrip(s_col * FONT_W, s_fb.width, py, FONT_H, bg);

    if (s_fb.cells) {
        TextCell empty = {' ', attr};
        for (uint32_t c = s_col; c < s_fb.cols; c++)
            s_fb.cells[s_row * s_fb.cols + c] = empty;
    }

    FbRequestFlush(py, FONT_H);
}

static void FbPrintChar(char c, uint8_t attr)
{
    if (c == '\n') {
        s_col = 0;
        s_row++;
        if (s_row >= s_fb.rows) { FbScrollUp(); s_row = s_fb.rows - 1; }
        return;
    }
    if (c == '\r') { s_col = 0; return; }
    if (c == '\b') {
        if (s_col > 0) { s_col--; FbDrawChar(s_col, s_row, ' ', attr); }
        return;
    }
    if (c == '\t') {
        s_col = (s_col + 8u) & ~7u;
        if (s_col >= s_fb.cols) {
            s_col = 0;
            s_row++;
            if (s_row >= s_fb.rows) { FbScrollUp(); s_row = s_fb.rows - 1; }
        }
        return;
    }

    FbDrawChar(s_col, s_row, c, attr);
    s_col++;
    if (s_col >= s_fb.cols) {
        s_col = 0;
        s_row++;
        if (s_row >= s_fb.rows) { FbScrollUp(); s_row = s_fb.rows - 1; }
    }
}

/* =========================================================================
 * Cursor and query
 * ========================================================================= */

static void FbSetCursor(int x, int y)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    s_col = (uint32_t)x < s_fb.cols ? (uint32_t)x : s_fb.cols - 1;
    s_row = (uint32_t)y < s_fb.rows ? (uint32_t)y : s_fb.rows - 1;
}

static int      FbGetCursorX(void)    { return (int)s_col; }
static int      FbGetCursorY(void)    { return (int)s_row; }
static void     FbUpdateCursor(void)  { /* no hardware cursor in GOP mode */ }
static uint16_t FbGetCols(void)       { return (uint16_t)s_fb.cols; }
static uint16_t FbGetRows(void)       { return (uint16_t)s_fb.rows; }

/*
 * FbChangeBackground — change background color of every cell and re-render.
 * Requires both cell buffer and shadow buffer.
 */
static void FbChangeBackground(uint8_t bg)
{
    if (!s_fb.cells || !s_fb.shadow) return;

    uint8_t bg4 = (bg & 0x0F) << 4;

    for (uint32_t r = 0; r < s_fb.rows; r++) {
        for (uint32_t c = 0; c < s_fb.cols; c++) {
            TextCell *cell = &s_fb.cells[r * s_fb.cols + c];
            cell->attr = (cell->attr & 0x0F) | bg4;
            FbRenderGlyph(c, r, cell->ch, cell->attr);
        }
    }

    FbFlushRegion(0, s_fb.text_h);
}

/* =========================================================================
 * Batch mode
 * ========================================================================= */

static void FbBatchBegin(void)
{
    if (!s_fb.shadow) return;   /* no batching without shadow buffer */
    s_batch    = 1;
    s_dirty_y0 = 0xFFFFFFFFU;
    s_dirty_y1 = 0;
}

static void FbBatchEnd(void)
{
    if (!s_batch) return;
    s_batch = 0;
    FbFlushDirty();
}

/* =========================================================================
 * Initialization
 * ========================================================================= */

int FbGopInit(uint64_t phys_addr, uint32_t width, uint32_t height,
              uint32_t stride, uint32_t format)
{
    size_t fb_size = (size_t)height * stride;
    volatile void *virt = vmm_map_framebuffer((uintptr_t)phys_addr, fb_size);
    if (!virt) {
        debug_printf("[VIDEO/GOP] vmm_map_framebuffer failed\n");
        return 0;
    }

    s_fb.virt   = (volatile uint8_t *)virt;
    s_fb.width  = width;
    s_fb.height = height;
    s_fb.stride = stride;
    s_fb.format = format;
    s_fb.cols   = width  / FONT_W;
    s_fb.rows   = height / FONT_H;
    s_fb.text_h = s_fb.rows * FONT_H;
    s_col = 0;
    s_row = 0;

    /* Shadow buffer: full framebuffer in RAM for fast read-modify-write */
    s_fb.shadow = (uint8_t *)vmalloc(fb_size);
    if (!s_fb.shadow)
        debug_printf("[VIDEO/GOP] shadow alloc failed (%zu B) — degraded mode\n",
                     fb_size);

    /* Cell buffer: character + attribute per text cell (~32 KB for 1080p) */
    size_t cell_size = (size_t)s_fb.rows * s_fb.cols * sizeof(TextCell);
    s_fb.cells = (TextCell *)vmalloc(cell_size);
    if (s_fb.cells) {
        TextCell empty = {' ', VIDEO_ATTR_DEFAULT};
        for (uint32_t i = 0; i < s_fb.rows * s_fb.cols; i++)
            s_fb.cells[i] = empty;
    } else {
        debug_printf("[VIDEO/GOP] cell buffer alloc failed (%zu B)\n", cell_size);
    }

    return 1;
}

/* =========================================================================
 * DisplayOps export
 * ========================================================================= */

const DisplayOps fb_gop_ops = {
    .PrintChar        = FbPrintChar,
    .ScrollUp         = FbScrollUp,
    .ClearScreen      = FbClearScreen,
    .ClearLine        = FbClearLine,
    .ClearToEol       = FbClearToEol,
    .SetCursor        = FbSetCursor,
    .GetCursorX       = FbGetCursorX,
    .GetCursorY       = FbGetCursorY,
    .UpdateCursor     = FbUpdateCursor,
    .ChangeBackground = FbChangeBackground,
    .GetCols          = FbGetCols,
    .GetRows          = FbGetRows,
    .BatchBegin       = FbBatchBegin,
    .BatchEnd         = FbBatchEnd,
};
