#include "fb_gop.h"
#include "../ops.h"
#include "fb_pixel.h"
#include "../font/vga_font.h"
#include "vmm.h"
#include "serial.h"
#include "video_colors.h"

typedef struct {
    volatile uint8_t *virt;
    uint8_t          *shadow;
    uint32_t          width;
    uint32_t          height;
    uint32_t          stride;
    uint32_t          format;
    uint32_t          cols;
    uint32_t          rows;
} FbState;

static FbState   s_fb       = {0};
static uint32_t  s_col      = 0;
static uint32_t  s_row      = 0;
static int       s_degraded = 0;

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

static inline void FbWritePixel(uint32_t x, uint32_t y, uint32_t rgb)
{
    uint32_t pixel = FbPixelEncode(rgb, s_fb.format);
    size_t   off   = (size_t)y * s_fb.stride + (size_t)x * 4;
    if (s_fb.shadow) {
        *(uint32_t *)(s_fb.shadow + off) = pixel;
    } else {
        *(volatile uint32_t *)(s_fb.virt + off) = pixel;
    }
}

static void FbFlushChar(uint32_t px_x, uint32_t px_y)
{
    if (!s_fb.shadow) return;
    for (uint32_t y = 0; y < FONT_H; y++) {
        size_t row_off = (size_t)(px_y + y) * s_fb.stride + (size_t)px_x * 4;
        const  uint32_t   *src = (const uint32_t *)(s_fb.shadow + row_off);
        volatile uint32_t *dst = (volatile uint32_t *)(s_fb.virt  + row_off);
        for (uint32_t x = 0; x < FONT_W; x++)
            dst[x] = src[x];
    }
}

static void FbFlush(uint32_t px_y, uint32_t line_count)
{
    if (!s_fb.shadow) return;
    if (px_y >= s_fb.height) return;
    if (px_y + line_count > s_fb.height)
        line_count = s_fb.height - px_y;
    size_t offset = (size_t)px_y * s_fb.stride;
    size_t bytes  = (size_t)line_count * s_fb.stride;
    FbBlitNT(s_fb.virt + offset, s_fb.shadow + offset, bytes);
}

static void FbDrawChar(uint32_t col, uint32_t row, char ch, uint8_t attr)
{
    if (col >= s_fb.cols || row >= s_fb.rows) return;

    uint32_t fg_rgb = vga_palette[attr & 0x0F];
    uint32_t bg_rgb = vga_palette[(attr >> 4) & 0x0F];

    uint32_t px = col * FONT_W;
    uint32_t py = row * FONT_H;

    const unsigned char *glyph = vga_font_8x16[(unsigned char)ch];
    for (int y = 0; y < FONT_H; y++) {
        uint8_t bits = glyph[y];
        for (int x = 0; x < FONT_W; x++) {
            FbWritePixel(px + (uint32_t)x, py + (uint32_t)y,
                         (bits & (0x80u >> x)) ? fg_rgb : bg_rgb);
        }
    }
    FbFlushChar(px, py);
}

static void FbScrollUp(void)
{
    size_t row_bytes = (size_t)FONT_H * s_fb.stride;
    size_t move_sz   = (size_t)(s_fb.rows - 1) * row_bytes;

    if (s_fb.shadow) {
        FbMemMove(s_fb.shadow, s_fb.shadow + row_bytes, move_sz);
        FbMemSet0(s_fb.shadow + move_sz, row_bytes);
        FbBlitNT(s_fb.virt, s_fb.shadow, (size_t)s_fb.height * s_fb.stride);
    } else {
        uint8_t *base = (uint8_t *)s_fb.virt;
        FbMemMove(base, base + row_bytes, move_sz);
        FbMemSet0(base + move_sz, row_bytes);
    }
}

static void FbClearScreen(void)
{
    if (s_fb.shadow) {
        FbMemSet0(s_fb.shadow, (size_t)s_fb.height * s_fb.stride);
        FbFlush(0, s_fb.height);
    } else {
        FbMemSet0((void *)s_fb.virt, (size_t)s_fb.height * s_fb.stride);
    }
    s_col = 0;
    s_row = 0;
}

static void FbClearLine(int line)
{
    if (line < 0 || (uint32_t)line >= s_fb.rows) return;

    uint32_t row    = (uint32_t)line;
    uint32_t bg_rgb = vga_palette[VIDEO_BG(VIDEO_ATTR_DEFAULT)];
    uint32_t py     = row * FONT_H;

    if (bg_rgb == 0) {
        if (s_fb.shadow) {
            FbMemSet0(s_fb.shadow + (size_t)py * s_fb.stride,
                      (size_t)FONT_H * s_fb.stride);
            FbFlush(py, FONT_H);
        } else {
            FbMemSet0((void *)(s_fb.virt + (size_t)py * s_fb.stride),
                      (size_t)FONT_H * s_fb.stride);
        }
    } else {
        uint32_t encoded = FbPixelEncode(bg_rgb, s_fb.format);
        for (uint32_t y = py; y < py + FONT_H; y++) {
            uint32_t *row_ptr;
            if (s_fb.shadow) {
                row_ptr = (uint32_t *)(s_fb.shadow + (size_t)y * s_fb.stride);
            } else {
                row_ptr = (uint32_t *)(s_fb.virt + (size_t)y * s_fb.stride);
            }
            for (uint32_t x = 0; x < s_fb.width; x++)
                row_ptr[x] = encoded;
        }
        if (s_fb.shadow) FbFlush(py, FONT_H);
    }
}

static void FbClearToEol(uint8_t attr)
{
    uint32_t save_col = s_col;
    for (uint32_t c = s_col; c < s_fb.cols; c++)
        FbDrawChar(c, s_row, ' ', attr);
    s_col = save_col;
}

static void FbPrintChar(char c, uint8_t attr)
{
    if (c == '\n') {
        s_col = 0;
        s_row++;
        if (s_row >= s_fb.rows) {
            FbScrollUp();
            s_row = s_fb.rows - 1;
        }
        return;
    }
    if (c == '\r') {
        s_col = 0;
        return;
    }
    if (c == '\b') {
        if (s_col > 0) {
            s_col--;
            FbDrawChar(s_col, s_row, ' ', attr);
        }
        return;
    }
    if (c == '\t') {
        s_col = (s_col + 8u) & ~7u;
        if (s_col >= s_fb.cols) {
            s_col = 0;
            s_row++;
            if (s_row >= s_fb.rows) {
                FbScrollUp();
                s_row = s_fb.rows - 1;
            }
        }
        return;
    }

    FbDrawChar(s_col, s_row, c, attr);
    s_col++;
    if (s_col >= s_fb.cols) {
        s_col = 0;
        s_row++;
        if (s_row >= s_fb.rows) {
            FbScrollUp();
            s_row = s_fb.rows - 1;
        }
    }
}

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
static void     FbChangeBackground(uint8_t bg) { (void)bg; /* full redraw not needed */ }
static uint16_t FbGetCols(void)       { return (uint16_t)s_fb.cols; }
static uint16_t FbGetRows(void)       { return (uint16_t)s_fb.rows; }

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
    s_col = 0;
    s_row = 0;

    s_fb.shadow = (uint8_t *)vmalloc(fb_size);
    if (!s_fb.shadow) {
        s_degraded = 1;
        debug_printf("[VIDEO/GOP] shadow alloc failed (%zu B) — degraded mode\n", fb_size);
    }
    return 1;
}

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
};
