
#include "hw_vga.h"
#include "../canvas.h"
#include "vmm.h"
#include "io.h"
#include "klib.h"

typedef struct {
    DisplayBackend base;
    unsigned char *vram;
} HwVgaState;

static HwVgaState s_vga;


static inline void crtc_write(uint8_t reg, uint8_t value)
{
    outb(HW_VGA_CRTC_ADDR_PORT, reg);
    outb(HW_VGA_CRTC_DATA_PORT, value);
}

static void cursor_to(uint32_t col, uint32_t row)
{
    uint16_t pos = (uint16_t)(row * HW_VGA_COLS + col);
    crtc_write(HW_VGA_CURSOR_LOW_REG,  (uint8_t)(pos & 0xFFu));
    crtc_write(HW_VGA_CURSOR_HIGH_REG, (uint8_t)((pos >> 8) & 0xFFu));
}


static inline unsigned char *cell_ptr(uint32_t row, uint32_t col)
{
    return s_vga.vram + (size_t)row * HW_VGA_LINE_BYTES + (size_t)col * HW_VGA_BYTES_PER_CELL;
}

static void op_DrawCells(DisplayBackend *be,
                         uint32_t row, uint32_t col_lo, uint32_t col_hi,
                         const TextCell *cells_row)
{
    if (row >= be->rows) return;
    if (col_hi > be->cols) col_hi = be->cols;
    unsigned char *p = cell_ptr(row, col_lo);

    uint32_t memo_fg = 0, memo_bg = 0;
    uint8_t  memo_attr = 0;
    bool     memo_valid = false;
    for (uint32_t c = col_lo; c < col_hi; c++) {
        if (!memo_valid || cells_row[c].fg != memo_fg || cells_row[c].bg != memo_bg) {
            memo_fg    = cells_row[c].fg;
            memo_bg    = cells_row[c].bg;
            memo_attr  = BoxColorPairToAttr(memo_fg, memo_bg);
            memo_valid = true;
        }
        *p++ = (unsigned char)cells_row[c].ch;
        *p++ = memo_attr;
    }
}

static void op_Scroll(DisplayBackend *be, uint32_t dy)
{
    if (!dy) return;
    if (dy >= be->rows) {
        for (size_t i = 0; i < HW_VGA_BUF_SIZE; i += 2) {
            s_vga.vram[i]     = ' ';
            s_vga.vram[i + 1] = VIDEO_ATTR_DEFAULT;
        }
        return;
    }
    size_t move_sz = (size_t)(be->rows - dy) * HW_VGA_LINE_BYTES;
    memmove(s_vga.vram, s_vga.vram + (size_t)dy * HW_VGA_LINE_BYTES, move_sz);
    for (size_t off = move_sz; off < HW_VGA_BUF_SIZE; off += 2) {
        s_vga.vram[off]     = ' ';
        s_vga.vram[off + 1] = VIDEO_ATTR_DEFAULT;
    }
}

static void op_FillRow(DisplayBackend *be, uint32_t row, uint32_t fg, uint32_t bg)
{
    if (row >= be->rows) return;
    uint8_t attr = BoxColorPairToAttr(fg, bg);
    unsigned char *p = cell_ptr(row, 0);
    for (uint32_t c = 0; c < HW_VGA_COLS; c++) {
        *p++ = ' ';
        *p++ = attr;
    }
}

static void op_Present(DisplayBackend *be, uint32_t row_lo, uint32_t row_hi)
{
    (void)be; (void)row_lo; (void)row_hi;
}

static void op_DrawCaret(DisplayBackend *be, uint32_t col, uint32_t row, uint32_t fg)
{
    (void)fg;
    if (col >= be->cols || row >= be->rows) return;
    cursor_to(col, row);
}

static void op_ActivatePullMap(DisplayBackend *be)
{
    (void)be;
    s_vga.vram = (unsigned char *)vmm_phys_to_virt(HW_VGA_BUF_ADDR);
    debug_printf("[VGA] Text buffer rebased to Pull Map: %p\n", s_vga.vram);
}


static void hw_vga_blink_off(void)
{
    (void)inb(0x3DA);
    outb(0x3C0, 0x10 | 0x20);
    uint8_t mode = inb(0x3C1);

    (void)inb(0x3DA);
    outb(0x3C0, 0x10 | 0x20);
    outb(0x3C0, (uint8_t)(mode & ~0x08u));
}

DisplayBackend *HwVgaBackendInit(void)
{
    s_vga.vram = (unsigned char *)HW_VGA_BUF_ADDR;

    hw_vga_blink_off();

    s_vga.base.caps  = 0;
    s_vga.base.cols  = HW_VGA_COLS;
    s_vga.base.rows  = HW_VGA_ROWS;
    s_vga.base.DrawCells       = op_DrawCells;
    s_vga.base.Scroll          = op_Scroll;
    s_vga.base.FillRow         = op_FillRow;
    s_vga.base.Present         = op_Present;
    s_vga.base.DrawCaret       = op_DrawCaret;
    s_vga.base.ActivatePullMap = op_ActivatePullMap;

    return &s_vga.base;
}