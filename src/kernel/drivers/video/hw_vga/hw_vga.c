#include "hw_vga.h"
#include "../ops.h"
#include "video_colors.h"
#include "io.h"
#include "vmm.h"
#include "serial.h"

static unsigned char *s_buf      = (unsigned char *)HW_VGA_BUF_ADDR;
static uint8_t  s_attr_backup[HW_VGA_COLS * HW_VGA_ROWS];
static unsigned s_cur_loc  = 0;
static unsigned s_last_loc = 0;

void HwVgaActivatePullMap(void)
{
    s_buf = (unsigned char *)vmm_phys_to_virt(HW_VGA_BUF_ADDR);
    debug_printf("[VGA] Text buffer rebased to Pull Map: %p\n", s_buf);
}

static void HwUpdateCursor(void);

static void HwScrollUp(void)
{
    for (unsigned i = 0; i < HW_VGA_BUF_SIZE - HW_VGA_LINE_BYTES; i += 2) {
        s_buf[i]     = s_buf[i + HW_VGA_LINE_BYTES];
        s_buf[i + 1] = s_buf[i + HW_VGA_LINE_BYTES + 1];
        s_attr_backup[i / 2] = s_attr_backup[(i + HW_VGA_LINE_BYTES) / 2];
    }
    for (unsigned i = HW_VGA_BUF_SIZE - HW_VGA_LINE_BYTES; i < HW_VGA_BUF_SIZE; i += 2) {
        s_buf[i]     = ' ';
        s_buf[i + 1] = VIDEO_ATTR_DEFAULT;
        s_attr_backup[i / 2] = VIDEO_ATTR_DEFAULT;
    }
    if (s_cur_loc >= HW_VGA_LINE_BYTES)
        s_cur_loc -= HW_VGA_LINE_BYTES;
    else
        s_cur_loc = 0;
}

static void HwClearScreen(void)
{
    for (unsigned i = 0; i < HW_VGA_BUF_SIZE; i += 2) {
        s_buf[i]     = ' ';
        s_buf[i + 1] = VIDEO_ATTR_DEFAULT;
        s_attr_backup[i / 2] = VIDEO_ATTR_DEFAULT;
    }
    s_cur_loc  = 0;
    s_last_loc = 0;
}

static void HwClearLine(int line)
{
    if (line < 0 || line >= (int)HW_VGA_ROWS) return;
    unsigned start = (unsigned)line * HW_VGA_LINE_BYTES;
    for (unsigned i = start; i < start + HW_VGA_LINE_BYTES; i += 2) {
        s_buf[i]     = ' ';
        s_buf[i + 1] = VIDEO_ATTR_DEFAULT;
        s_attr_backup[i / 2] = VIDEO_ATTR_DEFAULT;
    }
}

static void HwClearToEol(uint8_t attr)
{
    int x = (int)((s_cur_loc / 2) % HW_VGA_COLS);
    int y = (int)((s_cur_loc / 2) / HW_VGA_COLS);
    int idx = (y * (int)HW_VGA_COLS + x) * 2;
    int end = (y * (int)HW_VGA_COLS + (int)HW_VGA_COLS) * 2;
    for (; idx < end; idx += 2) {
        s_buf[idx]     = ' ';
        s_buf[idx + 1] = attr;
        s_attr_backup[idx / 2] = attr;
    }
}

static void HwSetCursor(int x, int y)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= (int)HW_VGA_COLS) x = (int)HW_VGA_COLS - 1;
    if (y >= (int)HW_VGA_ROWS) y = (int)HW_VGA_ROWS - 1;
    s_cur_loc = (unsigned)y * HW_VGA_LINE_BYTES + (unsigned)x * HW_VGA_BYTES_PER_CELL;
    HwUpdateCursor();
}

static int HwGetCursorX(void)
{
    return (int)((s_cur_loc / 2) % HW_VGA_COLS);
}

static int HwGetCursorY(void)
{
    return (int)((s_cur_loc / 2) / HW_VGA_COLS);
}

static void HwUpdateCursor(void)
{
    if (s_last_loc < HW_VGA_BUF_SIZE) {
        uint16_t last_pos = s_last_loc / 2;
        s_buf[s_last_loc + 1] = s_attr_backup[last_pos];
    }
    s_last_loc = s_cur_loc;

    uint16_t pos = s_cur_loc / 2;
    outb(HW_VGA_CRTC_ADDR_PORT, HW_VGA_CURSOR_LOW_REG);
    outb(HW_VGA_CRTC_DATA_PORT, (uint8_t)(pos & 0xFF));
    outb(HW_VGA_CRTC_ADDR_PORT, HW_VGA_CURSOR_HIGH_REG);
    outb(HW_VGA_CRTC_DATA_PORT, (uint8_t)((pos >> 8) & 0xFF));
}

static void HwPrintChar(char c, uint8_t attr)
{
    if (c == '\n') {
        int y = (int)((s_cur_loc / 2) / HW_VGA_COLS);
        if (y + 1 >= (int)HW_VGA_ROWS) {
            HwScrollUp();
            s_cur_loc = ((int)HW_VGA_ROWS - 1) * HW_VGA_LINE_BYTES;
        } else {
            s_cur_loc = (unsigned)(y + 1) * HW_VGA_LINE_BYTES;
        }
        return;
    }
    if (c == '\r') {
        int y = (int)((s_cur_loc / 2) / HW_VGA_COLS);
        s_cur_loc = (unsigned)y * HW_VGA_LINE_BYTES;
        return;
    }
    if (c == '\b') {
        if (s_cur_loc >= HW_VGA_BYTES_PER_CELL) {
            s_cur_loc -= HW_VGA_BYTES_PER_CELL;
            s_buf[s_cur_loc]     = ' ';
            s_buf[s_cur_loc + 1] = attr;
            s_attr_backup[s_cur_loc / 2] = attr;
        }
        return;
    }
    if (c == '\t') {
        int x = (int)((s_cur_loc / 2) % HW_VGA_COLS);
        int y = (int)((s_cur_loc / 2) / HW_VGA_COLS);
        int new_x = (x + 8) & ~7;
        if (new_x >= (int)HW_VGA_COLS) {
            new_x = 0;
            y++;
            if (y >= (int)HW_VGA_ROWS) {
                HwScrollUp();
                y = (int)HW_VGA_ROWS - 1;
            }
        }
        s_cur_loc = (unsigned)y * HW_VGA_LINE_BYTES + (unsigned)new_x * HW_VGA_BYTES_PER_CELL;
        return;
    }

    if (s_cur_loc >= HW_VGA_BUF_SIZE - 2)
        HwScrollUp();

    s_buf[s_cur_loc]     = (unsigned char)c;
    s_buf[s_cur_loc + 1] = attr;
    s_attr_backup[s_cur_loc / 2] = attr;
    s_cur_loc += HW_VGA_BYTES_PER_CELL;
}

static void HwChangeBackground(uint8_t bg)
{
    bg = (bg & 0x0F) << 4;
    for (unsigned i = 1; i < HW_VGA_BUF_SIZE; i += 2) {
        unsigned char cur = s_buf[i];
        s_buf[i] = (cur & 0x0F) | bg;
        s_attr_backup[i / 2] = s_buf[i];
    }
}

static uint16_t HwGetCols(void) { return HW_VGA_COLS; }
static uint16_t HwGetRows(void) { return HW_VGA_ROWS; }

/* VGA text mode: all writes are immediate MMIO — batch is a no-op */
static void HwBatchBegin(void) { }
static void HwBatchEnd(void)   { }

const DisplayOps hw_vga_ops = {
    .PrintChar        = HwPrintChar,
    .ScrollUp         = HwScrollUp,
    .ClearScreen      = HwClearScreen,
    .ClearLine        = HwClearLine,
    .ClearToEol       = HwClearToEol,
    .SetCursor        = HwSetCursor,
    .GetCursorX       = HwGetCursorX,
    .GetCursorY       = HwGetCursorY,
    .UpdateCursor     = HwUpdateCursor,
    .ChangeBackground = HwChangeBackground,
    .GetCols          = HwGetCols,
    .GetRows          = HwGetRows,
    .BatchBegin       = HwBatchBegin,
    .BatchEnd         = HwBatchEnd,
};
