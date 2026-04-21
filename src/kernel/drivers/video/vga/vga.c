#include "vga.h"
#include "vga_font.h"
#include "vmm.h"
#include "io.h"
#include "klib.h"
#include "serial.h"

/* =========================================================================
 * VGA text-mode state
 * ========================================================================= */

unsigned char *vga = (unsigned char *)VGA;

void vga_activate_pull_map(void) {
    vga = (unsigned char *)vmm_phys_to_virt(VGA_TEXT_BUFFER_ADDR);
    debug_printf("[VGA] Text buffer rebased to Pull Map: %p\n", vga);
}

uint8_t vga_attr_backup[VGA_WIDTH * VGA_HEIGHT];

uint8_t vga_current_color = TEXT_ATTR_DEFAULT;

static unsigned int current_loc = 0;
static unsigned int last_loc    = 0;

static unsigned int line_size    = VGA_WIDTH * BYTES_FOR_EACH_ELEMENT;

/* =========================================================================
 * GOP framebuffer state
 * ========================================================================= */

DisplayMode g_display_mode = DISPLAY_VGA_TEXT;

/*
 * FbParams — all framebuffer parameters plus the pixel-level shadow buffer.
 *
 * shadow: a copy of the framebuffer contents in regular (write-back) RAM.
 *   - All drawing operations write here first (fast cached writes).
 *   - FbFlushChar / FbFlush copy shadow → MMIO framebuffer (sequential
 *     32-bit stores, minimising uncacheable MMIO traffic and eliminating
 *     slow MMIO reads that made memmove on the raw framebuffer hang).
 *   - NULL when vmalloc fails; fallback path writes directly to MMIO.
 */
typedef struct {
    volatile uint8_t *virt;   /* kernel virtual base of the MMIO framebuffer */
    uint8_t          *shadow; /* pixel copy in regular RAM (vmalloc, may be NULL) */
    uint32_t          width;  /* pixels per row                               */
    uint32_t          height; /* pixel rows                                   */
    uint32_t          stride; /* bytes per scan line                          */
    uint32_t          format; /* 0=RGB, 1=BGR                                 */
    uint32_t          cols;   /* text columns = width  / FONT_W               */
    uint32_t          rows;   /* text rows    = height / FONT_H               */
} FbParams;

static FbParams  g_fb     = { 0 };
static uint32_t  g_fb_col = 0;
static uint32_t  g_fb_row = 0;

/* Standard CGA/VGA 16-colour palette in 24-bit RGB. */
static const uint32_t vga_palette[16] = {
    0x000000, /* 0  BLACK         */
    0x0000AA, /* 1  BLUE          */
    0x00AA00, /* 2  GREEN         */
    0x00AAAA, /* 3  CYAN          */
    0xAA0000, /* 4  RED           */
    0xAA00AA, /* 5  MAGENTA       */
    0xAA5500, /* 6  BROWN         */
    0xAAAAAA, /* 7  LIGHT_GRAY    */
    0x555555, /* 8  DARK_GRAY     */
    0x5555FF, /* 9  LIGHT_BLUE    */
    0x55FF55, /* A  LIGHT_GREEN   */
    0x55FFFF, /* B  LIGHT_CYAN    */
    0xFF5555, /* C  LIGHT_RED     */
    0xFF55FF, /* D  LIGHT_MAGENTA */
    0xFFFF55, /* E  YELLOW        */
    0xFFFFFF, /* F  WHITE         */
};

/* =========================================================================
 * Fast memory primitives for framebuffer operations
 *
 * klib memmove/memset are byte-by-byte — catastrophically slow for multi-MB
 * shadow buffers.  These replacements use x86-64 string instructions and
 * SSE2 non-temporal stores that are mandatory on x86-64.
 * ========================================================================= */

/*
 * FbMemMove — forward memory copy using rep movsq (8 bytes/iteration).
 * dst MUST be <= src (forward copy — safe for scroll-up where dst < src).
 * n is byte count; any tail < 8 bytes is handled with rep movsb.
 */
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

/*
 * FbMemSet0 — zero-fill using rep stosq (8 bytes/iteration).
 * n is byte count; tail handled with rep stosb.
 */
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
 * FbBlitNT — blit from WB shadow RAM to WC MMIO framebuffer using SSE2
 * non-temporal stores (MOVNTDQ).
 *
 * MOVNTDQ bypasses the CPU data cache on writes and feeds the WC
 * write-combining buffers directly.  Those buffers drain as 64-byte
 * cache-line burst transactions — the fastest legal path to WC MMIO.
 * The loop is 4x unrolled (one full cache line per iteration).
 * SFENCE at the end ensures all NT stores are globally visible.
 *
 * The kernel is compiled with -mno-sse globally (no FPU/SSE in interrupt
 * context).  The target("sse2") attribute enables SSE2 only for this
 * function so MOVNTDQ and XMM clobbers are legal here without polluting
 * the rest of the kernel.
 */
__attribute__((target("sse2")))
static void FbBlitNT(volatile uint8_t *dst, const uint8_t *src, size_t bytes)
{
    size_t i = 0;

    /* 64-byte (one cache line) granularity: 4 x MOVNTDQ per iteration. */
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
    /* Tail: any bytes not covered by the 64-byte loop. */
    for (; i + 4 <= bytes; i += 4)
        *(volatile uint32_t *)((uint8_t *)dst + i) = *(const uint32_t *)(src + i);
    for (; i < bytes; i++)
        *((volatile uint8_t *)dst + i) = src[i];

    /* Fence: ensure all NT stores reach the WC buffers before we return. */
    __asm__ volatile("sfence" ::: "memory");
}

/* =========================================================================
 * Low-level pixel helpers
 * ========================================================================= */

/*
 * FbEncodePixel — apply colour-format conversion and return the 32-bit word
 * to be stored in the framebuffer / shadow buffer.
 */
static inline uint32_t FbEncodePixel(uint32_t rgb)
{
    if (g_fb.format == 1) {   /* BGR: swap R↔B */
        uint8_t r = (rgb >> 16) & 0xFF;
        uint8_t g = (rgb >>  8) & 0xFF;
        uint8_t b =  rgb        & 0xFF;
        return ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
    }
    return rgb; /* RGB and BGRX — store as-is */
}

/*
 * FbWritePixel — write one pixel to the shadow buffer (preferred) or
 * directly to the MMIO framebuffer when no shadow is available.
 */
static inline void FbWritePixel(uint32_t x, uint32_t y, uint32_t rgb)
{
    uint32_t pixel = FbEncodePixel(rgb);
    size_t   off   = (size_t)y * g_fb.stride + (size_t)x * 4;

    if (g_fb.shadow) {
        *(uint32_t *)(g_fb.shadow + off) = pixel;
    } else {
        *(volatile uint32_t *)(g_fb.virt + off) = pixel;
    }
}

/*
 * FbFlushChar — blit one character cell (FONT_W × FONT_H pixels) from the
 * shadow buffer to the MMIO framebuffer.  Uses 32-bit stores; each pixel is
 * exactly 4 bytes so the loop is always word-aligned.
 *
 * Called after every FbDrawChar to minimise MMIO write traffic: only the
 * 8×16 = 128 words that actually changed are written to the device.
 */
static void FbFlushChar(uint32_t px_x, uint32_t px_y)
{
    if (!g_fb.shadow) return;

    for (uint32_t y = 0; y < FONT_H; y++) {
        size_t row_off = (size_t)(px_y + y) * g_fb.stride + (size_t)px_x * 4;
        const  uint32_t          *src = (const uint32_t *)(g_fb.shadow + row_off);
        volatile uint32_t        *dst = (volatile uint32_t *)(g_fb.virt  + row_off);
        for (uint32_t x = 0; x < FONT_W; x++)
            dst[x] = src[x];
    }
}

/*
 * FbFlush — blit a contiguous range of horizontal pixel scan-lines from the
 * shadow buffer to the MMIO framebuffer.  Uses SSE2 NT stores via FbBlitNT.
 *
 * px_y      — first pixel row to blit (inclusive)
 * line_count — number of pixel rows
 *
 * Used by FbClearScreen / FbClearRow after bulk RAM operations to commit
 * the result to the display in one pass.
 */
static void FbFlush(uint32_t px_y, uint32_t line_count)
{
    if (!g_fb.shadow) return;
    if (px_y >= g_fb.height) return;
    if (px_y + line_count > g_fb.height)
        line_count = g_fb.height - px_y;

    size_t offset = (size_t)px_y * g_fb.stride;
    size_t bytes  = (size_t)line_count * g_fb.stride;

    FbBlitNT(g_fb.virt + offset, g_fb.shadow + offset, bytes);
}

/* =========================================================================
 * Character-level drawing (GOP framebuffer)
 * ========================================================================= */

/* Render one 8×16 glyph at text-grid position (col, row). */
static void FbDrawChar(uint32_t col, uint32_t row, char ch, uint8_t attr)
{
    if (col >= g_fb.cols || row >= g_fb.rows) return;

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

    /* Commit only this character's pixels to the MMIO framebuffer.
     * 128 × 32-bit stores is negligible; avoids full-row blits per char. */
    FbFlushChar(px, py);
}

/*
 * FbScrollUp — scroll the entire display up by one text row.
 *
 * With shadow buffer (normal path):
 *   1. FbMemMove on RAM shadow — rep movsq, 8 bytes/cycle, no MMIO reads.
 *   2. FbMemSet0 the last row in shadow — rep stosq, zero-fill.
 *   3. FbBlitNT: blit entire shadow to WC MMIO via SSE2 MOVNTDQ — 64 bytes
 *      per store burst, drains through write-combining buffers.
 *
 * Without shadow (fallback):
 *   Direct rep movsq / rep stosq on MMIO — slow but correct.
 */
static void FbScrollUp(void)
{
    size_t row_bytes = (size_t)FONT_H * g_fb.stride;
    size_t move_sz   = (size_t)(g_fb.rows - 1) * row_bytes;

    if (g_fb.shadow) {
        /* Fast forward copy: dst < src so forward rep movsq is safe. */
        FbMemMove(g_fb.shadow, g_fb.shadow + row_bytes, move_sz);
        /* Zero-fill the new blank bottom row. */
        FbMemSet0(g_fb.shadow + move_sz, row_bytes);
        /* Blit entire updated shadow to WC MMIO via SSE2 NT stores. */
        FbBlitNT(g_fb.virt, g_fb.shadow, (size_t)g_fb.height * g_fb.stride);
    } else {
        /* Fallback: no shadow — direct MMIO ops (slow but correct). */
        uint8_t *base = (uint8_t *)g_fb.virt;
        FbMemMove(base, base + row_bytes, move_sz);
        FbMemSet0(base + move_sz, row_bytes);
    }
}

/* Clear the full framebuffer and home the cursor. */
static void FbClearScreen(void)
{
    if (g_fb.shadow) {
        FbMemSet0(g_fb.shadow, (size_t)g_fb.height * g_fb.stride);
        FbFlush(0, g_fb.height);
    } else {
        FbMemSet0((void *)g_fb.virt, (size_t)g_fb.height * g_fb.stride);
    }
    g_fb_col = 0;
    g_fb_row = 0;
}

/* Clear one text row (fill with background of current colour). */
static void FbClearRow(uint32_t row)
{
    if (row >= g_fb.rows) return;

    uint8_t  bg     = (vga_current_color >> 4) & 0x0F;
    uint32_t bg_rgb = vga_palette[bg];
    uint32_t py     = row * FONT_H;

    if (bg_rgb == 0) {
        /* Background is black — fast path: zero-fill. */
        if (g_fb.shadow) {
            FbMemSet0(g_fb.shadow + (size_t)py * g_fb.stride,
                      (size_t)FONT_H * g_fb.stride);
            FbFlush(py, FONT_H);
        } else {
            FbMemSet0((void *)(g_fb.virt + (size_t)py * g_fb.stride),
                      (size_t)FONT_H * g_fb.stride);
        }
    } else {
        /* Non-black background — fill pixel by pixel into shadow, then flush. */
        uint32_t encoded = FbEncodePixel(bg_rgb);
        for (uint32_t y = py; y < py + FONT_H; y++) {
            uint32_t *row_ptr;
            if (g_fb.shadow) {
                row_ptr = (uint32_t *)(g_fb.shadow + (size_t)y * g_fb.stride);
            } else {
                row_ptr = (uint32_t *)(g_fb.virt + (size_t)y * g_fb.stride);
            }
            for (uint32_t x = 0; x < g_fb.width; x++)
                row_ptr[x] = encoded;
        }
        if (g_fb.shadow) FbFlush(py, FONT_H);
    }
}

/* Output one character through the GOP framebuffer backend. */
static void FbPrintChar(char ch, uint8_t attr)
{
    if (ch == '\n') {
        g_fb_col = 0;
        g_fb_row++;
        if (g_fb_row >= g_fb.rows) {
            FbScrollUp();
            g_fb_row = g_fb.rows - 1;
        }
        return;
    }
    if (ch == '\r') {
        g_fb_col = 0;
        return;
    }
    if (ch == '\b') {
        if (g_fb_col > 0) {
            g_fb_col--;
            FbDrawChar(g_fb_col, g_fb_row, ' ', attr);
        }
        return;
    }
    if (ch == '\t') {
        g_fb_col = (g_fb_col + 8u) & ~7u;
        if (g_fb_col >= g_fb.cols) {
            g_fb_col = 0;
            g_fb_row++;
            if (g_fb_row >= g_fb.rows) {
                FbScrollUp();
                g_fb_row = g_fb.rows - 1;
            }
        }
        return;
    }

    FbDrawChar(g_fb_col, g_fb_row, ch, attr);
    g_fb_col++;
    if (g_fb_col >= g_fb.cols) {
        g_fb_col = 0;
        g_fb_row++;
        if (g_fb_row >= g_fb.rows) {
            FbScrollUp();
            g_fb_row = g_fb.rows - 1;
        }
    }
}

/* =========================================================================
 * Public initialisation
 * ========================================================================= */

void vga_set_color(uint8_t color) { vga_current_color = color; }
uint8_t vga_get_color(void)       { return vga_current_color;  }
void vga_reset_color(void)        { vga_current_color = TEXT_ATTR_DEFAULT; }

void vga_init(void)
{
    vga_clear_screen();
    vga_set_cursor_position(0, 0);
}

void vga_init_framebuffer(uint64_t phys_addr, uint32_t width, uint32_t height,
                          uint32_t stride, uint32_t format)
{
    if (!phys_addr || !width || !height || !stride) return;

    size_t fb_size = (size_t)height * stride;

    /*
     * Map the framebuffer with Write Combining (WC) caching via vmm_map_framebuffer.
     * WC allows the CPU to coalesce sequential pixel writes into cache-line bursts
     * before flushing to the bus — 10–50× faster than the UC mapping that
     * vmm_map_mmio would produce, making FbFlush during scroll fast enough to
     * be imperceptible to the user.
     */
    volatile void *virt = vmm_map_framebuffer((uintptr_t)phys_addr, fb_size);
    if (!virt) {
        debug_printf("[VGA] vga_init_framebuffer: vmm_map_framebuffer failed\n");
        return;
    }

    g_fb.virt   = (volatile uint8_t *)virt;
    g_fb.width  = width;
    g_fb.height = height;
    g_fb.stride = stride;
    g_fb.format = format;
    g_fb.cols   = width  / FONT_W;
    g_fb.rows   = height / FONT_H;

    /*
     * Allocate pixel shadow buffer in regular (write-back) RAM.
     * All drawing operates on this buffer; FbFlushChar/FbFlush commit
     * changes to the UC MMIO framebuffer using sequential 32-bit stores,
     * which is orders of magnitude faster than byte-by-byte memmove on
     * raw MMIO (which would hang on large displays).
     *
     * vmalloc is used (not kmalloc) since fb_size can exceed the kernel
     * heap's contiguous allocation limit (e.g. 8 MB for 1920×1080).
     */
    g_fb.shadow = (uint8_t *)vmalloc(fb_size);
    if (!g_fb.shadow) {
        debug_printf("[VGA] vga_init_framebuffer: shadow alloc failed "
                     "(%zu bytes) — fallback to direct MMIO writes\n", fb_size);
    }

    g_fb_col = 0;
    g_fb_row = 0;

    /* Switch mode before clearing so that subsequent kprintf goes to the FB. */
    g_display_mode = DISPLAY_GOP_FB;
    FbClearScreen();
}

/* =========================================================================
 * Public dimension helpers
 * ========================================================================= */

/*
 * vga_get_display_cols / vga_get_display_rows
 *
 * Return the actual text-grid dimensions of the current display mode:
 *   GOP mode  — derived from the GOP framebuffer resolution.
 *   Text mode — fixed 80 × 25 VGA hardware constants.
 *
 * Used by the hardware deck to report accurate screen geometry to userspace
 * instead of the hardcoded 80 × 25 that was always returned before.
 */
int vga_get_display_cols(void)
{
    if (g_display_mode == DISPLAY_GOP_FB && g_fb.cols > 0)
        return (int)g_fb.cols;
    return VGA_WIDTH;
}

int vga_get_display_rows(void)
{
    if (g_display_mode == DISPLAY_GOP_FB && g_fb.rows > 0)
        return (int)g_fb.rows;
    return VGA_HEIGHT;
}

/* =========================================================================
 * Public VGA API — dispatch on g_display_mode
 * ========================================================================= */

void vga_print_char(char ch, const unsigned char attr)
{
    if (g_display_mode == DISPLAY_GOP_FB) {
        FbPrintChar(ch, attr);
        return;
    }

    if (current_loc >= VGA_SIZE - 2) vga_scroll_up();

    vga[current_loc]     = (unsigned char)ch;
    vga[current_loc + 1] = attr;
    vga_attr_backup[current_loc / 2] = attr;

    current_loc += 2;
    vga_update_cursor();
}

void vga_print(const char *str)
{
    while (*str) {
        if (*str == '\n') {
            vga_print_newline();
            str++;
            continue;
        }
        vga_print_char(*str, vga_current_color);
        str++;
    }
    if (g_display_mode == DISPLAY_VGA_TEXT)
        vga_update_cursor();
}

void vga_print_newline(void)
{
    if (g_display_mode == DISPLAY_GOP_FB) {
        FbPrintChar('\n', vga_current_color);
        return;
    }

    int y = vga_get_cursor_position_y();
    if (y + 1 >= VGA_HEIGHT) {
        vga_scroll_up();
        current_loc = (VGA_HEIGHT - 1) * line_size;
    } else {
        current_loc = (y + 1) * line_size;
    }
}

void vga_clear_screen(void)
{
    if (g_display_mode == DISPLAY_GOP_FB) {
        FbClearScreen();
        return;
    }

    for (int i = 0; i < VGA_SIZE; i += 2) {
        vga[i]     = ' ';
        vga[i + 1] = TEXT_ATTR_DEFAULT;
    }
    current_loc = 0;
}

void vga_clear_line(int line)
{
    if (g_display_mode == DISPLAY_GOP_FB) {
        FbClearRow((uint32_t)line);
        return;
    }

    if (line < 0 || line >= VGA_HEIGHT) return;
    unsigned int line_start = (unsigned int)line * line_size;
    for (unsigned int i = line_start; i < line_start + line_size; i += 2) {
        vga[i]     = ' ';
        vga[i + 1] = TEXT_ATTR_DEFAULT;
    }
}

void vga_clear_to_eol(void)
{
    if (g_display_mode == DISPLAY_GOP_FB) {
        uint32_t save_col = g_fb_col;
        for (uint32_t c = g_fb_col; c < g_fb.cols; c++)
            FbDrawChar(c, g_fb_row, ' ', vga_current_color);
        g_fb_col = save_col;
        return;
    }

    int x = vga_get_cursor_position_x();
    int y = vga_get_cursor_position_y();
    if (x < 0 || x >= VGA_WIDTH || y < 0 || y >= VGA_HEIGHT) {
        vga_print_error("vga_clear_to_eol: Invalid cursor pos");
        return;
    }
    int idx = (y * VGA_WIDTH + x) * 2;
    int end = (y * VGA_WIDTH + VGA_WIDTH) * 2;
    for (; idx < end; idx += 2) {
        vga[idx]     = ' ';
        vga[idx + 1] = TEXT_ATTR_DEFAULT;
    }
}

void vga_print_error(const char *str)
{
    while (*str) {
        if (*str == '\n') { vga_print_newline(); str++; continue; }
        vga_print_char(*str, TEXT_ATTR_ERROR);
        str++;
    }
    if (g_display_mode == DISPLAY_VGA_TEXT)
        vga_update_cursor();
}

void vga_print_success(const char *str)
{
    while (*str) {
        if (*str == '\n') { vga_print_newline(); str++; continue; }
        vga_print_char(*str, TEXT_ATTR_SUCCESS);
        str++;
    }
    if (g_display_mode == DISPLAY_VGA_TEXT)
        vga_update_cursor();
}

void vga_print_hint(const char *str)
{
    while (*str) {
        if (*str == '\n') { vga_print_newline(); str++; continue; }
        vga_print_char(*str, TEXT_ATTR_HINT);
        str++;
    }
    if (g_display_mode == DISPLAY_VGA_TEXT)
        vga_update_cursor();
}

void vga_scroll_up(void)
{
    if (g_display_mode == DISPLAY_GOP_FB) {
        FbScrollUp();
        return;
    }

    for (unsigned int i = 0; i < VGA_SIZE - line_size; i += 2) {
        vga[i]     = vga[i + line_size];
        vga[i + 1] = vga[i + line_size + 1];
        vga_attr_backup[i / 2] = vga_attr_backup[(i + line_size) / 2];
    }
    for (unsigned int i = VGA_SIZE - line_size; i < VGA_SIZE; i += 2) {
        vga[i]     = ' ';
        vga[i + 1] = TEXT_ATTR_DEFAULT;
        vga_attr_backup[i / 2] = TEXT_ATTR_DEFAULT;
    }
    if (current_loc >= line_size)
        current_loc -= line_size;
    else
        current_loc = 0;
}

void vga_change_background(unsigned char new_bg_color)
{
    if (g_display_mode == DISPLAY_GOP_FB) return; /* no-op in framebuffer mode */

    new_bg_color &= 0xF0;
    for (unsigned int i = 1; i < VGA_SIZE; i += 2) {
        unsigned char current_attr = vga[i];
        vga[i] = (current_attr & 0x0F) | new_bg_color;
        vga_attr_backup[i / 2] = vga[i];
    }
}

void vga_update_cursor(void)
{
    if (g_display_mode == DISPLAY_GOP_FB) return; /* no hardware cursor in FB mode */

    if (last_loc < VGA_SIZE) {
        uint16_t last_pos = last_loc / 2;
        vga[last_loc + 1] = vga_attr_backup[last_pos];
    }
    last_loc = current_loc;

    uint16_t pos = current_loc / 2;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

void vga_set_cursor_position(int x, int y)
{
    if (g_display_mode == DISPLAY_GOP_FB) {
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        g_fb_col = (uint32_t)x < g_fb.cols ? (uint32_t)x : g_fb.cols - 1;
        g_fb_row = (uint32_t)y < g_fb.rows ? (uint32_t)y : g_fb.rows - 1;
        return;
    }

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= VGA_WIDTH)  x = VGA_WIDTH  - 1;
    if (y >= VGA_HEIGHT) y = VGA_HEIGHT - 1;
    current_loc = (unsigned int)y * line_size + (unsigned int)x * BYTES_FOR_EACH_ELEMENT;
    vga_update_cursor();
}

int vga_get_cursor_position_x(void)
{
    if (g_display_mode == DISPLAY_GOP_FB) return (int)g_fb_col;
    return (int)((current_loc / 2) % VGA_WIDTH);
}

int vga_get_cursor_position_y(void)
{
    if (g_display_mode == DISPLAY_GOP_FB) return (int)g_fb_row;
    return (int)((current_loc / 2) / VGA_WIDTH);
}
