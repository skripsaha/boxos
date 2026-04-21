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

static unsigned int current_line = 0;
static unsigned int line_size    = VGA_WIDTH * BYTES_FOR_EACH_ELEMENT;

/* =========================================================================
 * GOP framebuffer state
 * ========================================================================= */

DisplayMode g_display_mode = DISPLAY_VGA_TEXT;

typedef struct {
    volatile uint8_t *virt;   /* virtual base of the mapped framebuffer  */
    uint32_t          width;  /* pixels per row                           */
    uint32_t          height; /* pixel rows                               */
    uint32_t          stride; /* bytes per scan line                      */
    uint32_t          format; /* 0=RGB, 1=BGR                             */
    uint32_t          cols;   /* text columns = width  / FONT_W           */
    uint32_t          rows;   /* text rows    = height / FONT_H           */
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

/* Write one 32-bpp pixel, honouring the framebuffer colour format. */
static inline void FbWritePixel(uint32_t x, uint32_t y, uint32_t rgb)
{
    volatile uint32_t *px = (volatile uint32_t *)
        (g_fb.virt + (size_t)y * g_fb.stride + (size_t)x * 4);

    if (g_fb.format == 1) {   /* BGR: swap R↔B */
        uint8_t r = (rgb >> 16) & 0xFF;
        uint8_t g = (rgb >>  8) & 0xFF;
        uint8_t b =  rgb        & 0xFF;
        *px = ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
    } else {                  /* RGB (and BGRX / other) */
        *px = rgb;
    }
}

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
}

/* Scroll the entire framebuffer up by one text row. */
static void FbScrollUp(void)
{
    uint8_t *base     = (uint8_t *)g_fb.virt;
    size_t   row_px   = (size_t)FONT_H * g_fb.stride;
    size_t   move_sz  = (size_t)(g_fb.rows - 1) * row_px;

    memmove(base, base + row_px, move_sz);
    memset(base + move_sz, 0, row_px);
}

/* Clear the full framebuffer and home the cursor. */
static void FbClearScreen(void)
{
    memset((void *)g_fb.virt, 0, (size_t)g_fb.height * g_fb.stride);
    g_fb_col = 0;
    g_fb_row = 0;
}

/* Clear one text row (fill with background of current colour). */
static void FbClearRow(uint32_t row)
{
    if (row >= g_fb.rows) return;
    uint8_t bg = (vga_current_color >> 4) & 0x0F;
    uint32_t bg_rgb = vga_palette[bg];
    uint32_t py = row * FONT_H;

    if (bg_rgb == 0) {
        uint8_t *base = (uint8_t *)g_fb.virt + (size_t)py * g_fb.stride;
        memset(base, 0, (size_t)FONT_H * g_fb.stride);
    } else {
        for (uint32_t y = py; y < py + FONT_H; y++) {
            for (uint32_t x = 0; x < g_fb.width; x++) {
                FbWritePixel(x, y, bg_rgb);
            }
        }
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
    volatile void *virt = vmm_map_mmio((uintptr_t)phys_addr, fb_size,
                                       VMM_FLAGS_KERNEL_RW | VMM_FLAG_CACHE_DISABLE);
    if (!virt) {
        debug_printf("[VGA] vga_init_framebuffer: vmm_map_mmio failed\n");
        return;
    }

    g_fb.virt   = (volatile uint8_t *)virt;
    g_fb.width  = width;
    g_fb.height = height;
    g_fb.stride = stride;
    g_fb.format = format;
    g_fb.cols   = width  / FONT_W;
    g_fb.rows   = height / FONT_H;

    g_fb_col = 0;
    g_fb_row = 0;

    /* Switch mode before clearing so that subsequent kprintf goes to the FB. */
    g_display_mode = DISPLAY_GOP_FB;
    FbClearScreen();
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
