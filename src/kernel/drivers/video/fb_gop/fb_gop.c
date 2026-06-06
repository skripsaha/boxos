/*
 * fb_gop.c — UEFI GOP framebuffer backend.
 *
 * Surface model:
 *   shadow buffer (WB DRAM, owned by this TU)
 *      │  rendered glyphs in device pixel format
 *      ▼
 *   VRAM (WC-mapped, owned by firmware/GPU)
 *      • flushed via SSE2 movntdq + sfence
 *
 * All Canvas-visible operations (DrawCells, Scroll, FillRow) write to the
 * shadow only; Present is the single point that touches VRAM.  This keeps
 * the WC sfence stride large (one fence covers an entire damage rectangle)
 * and lets multiple intra-batch ops coalesce into one NT blit.
 *
 * Caret: a 2-pixel underline rendered into shadow at (col, row+FONT_H-2).
 * Canvas already brings the old caret cell into the dirty range before
 * commit, so DrawCells repaints it; DrawCaret then overlays the new caret
 * — both end up in the same Present call.
 *
 * Degraded mode: if shadow allocation fails (low memory, e.g. PMM
 * exhaustion under 64 MB QEMU runs), DrawCells writes directly to VRAM
 * using volatile uint32_t stores.  Present becomes a no-op.  Scroll
 * fallback is a VRAM-to-VRAM memmove (slow but correct).  No SSE NT
 * blits are used in degraded mode because src and dst alias.
 */

#include "fb_gop.h"
#include "fb_pixel.h"
#include "../canvas.h"
#include "../font/vga_font.h"
#include "vmm.h"
#include "klib.h"

typedef struct {
    DisplayBackend    base;

    volatile uint8_t *vram;        /* VRAM base (WC-mapped)              */
    uint8_t          *shadow;      /* shadow base (WB, NULL = degraded)  */
    uint32_t          width;
    uint32_t          height;
    uint32_t          stride;       /* bytes per scanline                */
    uint32_t          format;       /* FB_FORMAT_*                       */
    uint32_t          text_h;       /* rows*FONT_H — visible text height */
} FbGopState;

static FbGopState s_gop;

/* =========================================================================
 *  Low-level memory operations
 * ========================================================================= */

static inline void fb_memmove_qw(void *dst, const void *src, size_t bytes)
{
    size_t qwords = bytes >> 3;
    size_t tail   = bytes &  7u;
    __asm__ volatile("rep movsq" : "+D"(dst), "+S"(src), "+c"(qwords) :: "memory");
    __asm__ volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(tail)   :: "memory");
}

static inline void fb_memset0(void *dst, size_t bytes)
{
    size_t qwords = bytes >> 3;
    size_t tail   = bytes &  7u;
    __asm__ volatile("xorq %%rax,%%rax; rep stosq"
                     : "+D"(dst), "+c"(qwords) :: "rax", "memory");
    __asm__ volatile("xorb %%al,%%al; rep stosb"
                     : "+D"(dst), "+c"(tail) :: "rax", "memory");
}

static inline void fb_memset32(void *dst, uint32_t value, size_t count)
{
    __asm__ volatile("rep stosl"
                     : "+D"(dst), "+c"(count)
                     : "a"(value)
                     : "memory");
}

/*
 * fb_blit_nt — SSE2 non-temporal blit shadow → VRAM (4 × movntdq per
 * 64-byte chunk).  Bypasses CPU cache; ideal for WC-mapped framebuffer
 * memory.  Trailing tail under 64 bytes falls back to plain 32/8-bit
 * stores so we keep correctness even at odd stride sizes.
 */
__attribute__((target("sse2")))
static void fb_blit_nt(volatile uint8_t *dst, const uint8_t *src, size_t bytes)
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
            : "xmm0", "xmm1", "xmm2", "xmm3", "memory");
    }
    for (; i + 4 <= bytes; i += 4)
        *(volatile uint32_t *)((uint8_t *)dst + i) = *(const uint32_t *)(src + i);
    for (; i < bytes; i++)
        *((volatile uint8_t *)dst + i) = src[i];
    __asm__ volatile("sfence" ::: "memory");
}

/* =========================================================================
 *  Glyph rendering — writes one 8×16 cell into shadow (or VRAM in degraded)
 * ========================================================================= */

static void render_glyph(FbGopState *s, uint32_t col, uint32_t row,
                         char ch, uint8_t attr)
{
    uint32_t fg = FbPixelEncode(vga_palette[attr        & 0x0F], s->format);
    uint32_t bg = FbPixelEncode(vga_palette[(attr >> 4) & 0x0F], s->format);

    uint32_t px = col * FONT_W;
    uint32_t py = row * FONT_H;
    const unsigned char *glyph = vga_font_8x16[(unsigned char)ch];

    if (s->shadow) {
        for (uint32_t y = 0; y < FONT_H; y++) {
            uint32_t *p = (uint32_t *)(s->shadow
                                       + (size_t)(py + y) * s->stride
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
        for (uint32_t y = 0; y < FONT_H; y++) {
            volatile uint32_t *p = (volatile uint32_t *)(s->vram
                                       + (size_t)(py + y) * s->stride
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

static void fill_pixel_strip(FbGopState *s, uint32_t px_x0, uint32_t px_x1,
                             uint32_t py, uint32_t h, uint32_t encoded_bg)
{
    uint32_t pixels = px_x1 - px_x0;
    if (!pixels) return;

    if (s->shadow) {
        for (uint32_t y = py; y < py + h && y < s->height; y++) {
            uint32_t *p = (uint32_t *)(s->shadow
                                       + (size_t)y * s->stride
                                       + (size_t)px_x0 * 4);
            if (encoded_bg == 0) fb_memset0(p, (size_t)pixels * 4);
            else                 fb_memset32(p, encoded_bg, pixels);
        }
    } else {
        for (uint32_t y = py; y < py + h && y < s->height; y++) {
            volatile uint32_t *p = (volatile uint32_t *)(s->vram
                                       + (size_t)y * s->stride
                                       + (size_t)px_x0 * 4);
            for (uint32_t x = 0; x < pixels; x++) p[x] = encoded_bg;
        }
    }
}

/* =========================================================================
 *  Backend vtable
 * ========================================================================= */

static void op_DrawCells(DisplayBackend *be,
                         uint32_t row, uint32_t col_lo, uint32_t col_hi,
                         const TextCell *cells_row)
{
    FbGopState *s = (FbGopState *)be;
    if (row >= be->rows) return;
    if (col_hi > be->cols) col_hi = be->cols;
    for (uint32_t c = col_lo; c < col_hi; c++)
        render_glyph(s, c, row, cells_row[c].ch, cells_row[c].attr);
}

static void op_Scroll(DisplayBackend *be, uint32_t dy)
{
    FbGopState *s = (FbGopState *)be;
    if (!dy) return;
    if (dy >= be->rows) {
        if (s->shadow) fb_memset0(s->shadow, (size_t)s->text_h * s->stride);
        else           fb_memset0((void *)s->vram, (size_t)s->text_h * s->stride);
        return;
    }

    size_t row_bytes = (size_t)FONT_H * s->stride;
    size_t move_sz   = (size_t)(be->rows - dy) * row_bytes;
    size_t clear_off = move_sz;
    size_t clear_sz  = (size_t)dy * row_bytes;

    if (s->shadow) {
        fb_memmove_qw(s->shadow, s->shadow + (size_t)dy * row_bytes, move_sz);
        fb_memset0(s->shadow + clear_off, clear_sz);
    } else {
        uint8_t *base = (uint8_t *)s->vram;
        fb_memmove_qw(base, base + (size_t)dy * row_bytes, move_sz);
        fb_memset0(base + clear_off, clear_sz);
    }
}

static void op_FillRow(DisplayBackend *be, uint32_t row, uint8_t attr)
{
    FbGopState *s = (FbGopState *)be;
    if (row >= be->rows) return;
    uint32_t bg = FbPixelEncode(vga_palette[(attr >> 4) & 0x0F], s->format);
    fill_pixel_strip(s, 0, s->width, row * FONT_H, FONT_H, bg);
}

static void op_Present(DisplayBackend *be, uint32_t row_lo, uint32_t row_hi)
{
    FbGopState *s = (FbGopState *)be;
    if (!s->shadow) return;                    /* degraded: writes are direct */
    if (row_hi <= row_lo) return;
    if (row_hi > be->rows) row_hi = be->rows;

    uint32_t y    = row_lo * FONT_H;
    uint32_t hpx  = (row_hi - row_lo) * FONT_H;
    if (y >= s->height) return;
    if (y + hpx > s->height) hpx = s->height - y;

    fb_blit_nt(s->vram   + (size_t)y * s->stride,
               s->shadow + (size_t)y * s->stride,
               (size_t)hpx * s->stride);
}

static void op_DrawCaret(DisplayBackend *be, uint32_t col, uint32_t row, uint8_t attr)
{
    FbGopState *s = (FbGopState *)be;
    if (col >= be->cols || row >= be->rows) return;

    uint32_t px = col * FONT_W;
    uint32_t py = row * FONT_H + (FONT_H - 2);
    uint32_t fg = FbPixelEncode(vga_palette[attr & 0x0F], s->format);

    if (s->shadow) {
        for (uint32_t y = 0; y < 2; y++) {
            uint32_t *p = (uint32_t *)(s->shadow
                                       + (size_t)(py + y) * s->stride
                                       + (size_t)px * 4);
            for (uint32_t x = 0; x < FONT_W; x++) p[x] = fg;
        }
    } else {
        for (uint32_t y = 0; y < 2; y++) {
            volatile uint32_t *p = (volatile uint32_t *)(s->vram
                                       + (size_t)(py + y) * s->stride
                                       + (size_t)px * 4);
            for (uint32_t x = 0; x < FONT_W; x++) p[x] = fg;
        }
    }
}

/* =========================================================================
 *  Initialisation
 * ========================================================================= */

DisplayBackend *FbGopBackendInit(uint64_t phys_addr,
                                 uint32_t width, uint32_t height,
                                 uint32_t stride, uint32_t format)
{
    if (!phys_addr || !width || !height || !stride) return NULL;

    size_t fb_size = (size_t)height * stride;
    volatile void *virt = vmm_map_framebuffer((uintptr_t)phys_addr, fb_size);
    if (!virt) {
        debug_printf("[VIDEO/GOP] vmm_map_framebuffer failed\n");
        return NULL;
    }

    s_gop.vram   = (volatile uint8_t *)virt;
    s_gop.width  = width;
    s_gop.height = height;
    s_gop.stride = stride;
    s_gop.format = format;

    s_gop.base.cols = width  / FONT_W;
    s_gop.base.rows = height / FONT_H;
    s_gop.text_h    = s_gop.base.rows * FONT_H;

    /* Caps stay at 0 for GOP — there is no portable pan-display interface.
     * Future i915/amdgpu drivers will report DISP_CAP_HW_SCROLL after
     * binding their KMS plane to this surface. */
    s_gop.base.caps      = 0;
    s_gop.base.DrawCells = op_DrawCells;
    s_gop.base.Scroll    = op_Scroll;
    s_gop.base.FillRow   = op_FillRow;
    s_gop.base.Present   = op_Present;
    s_gop.base.DrawCaret = op_DrawCaret;

    s_gop.shadow = (uint8_t *)vmalloc(fb_size);
    if (!s_gop.shadow)
        debug_printf("[VIDEO/GOP] shadow alloc failed (%zu B) — degraded mode\n",
                     fb_size);

    return &s_gop.base;
}
