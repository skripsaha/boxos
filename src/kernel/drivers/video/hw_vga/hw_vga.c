/*
 * hw_vga.c — legacy 80×25 VGA text-mode backend.
 *
 * Writes char+attr pairs straight to VRAM at 0xB8000 (rebased onto the
 * Pull Map after vmm_init).  No shadow buffer — the entire visible
 * surface is 4000 bytes and a SW memmove on scroll costs sub-microsecond
 * on any post-Pentium CPU.
 *
 * Caret: real CRTC hardware cursor (registers 0x0E/0x0F).  Visible
 * immediately, no shadow flush needed.
 *
 * Why no CRTC HW scroll (Start Address bump): the CRTC Start Address
 * register pair is *writable* on essentially every PC, but
 * panning-via-Start-Address only behaves uniformly on the original IBM
 * VGA hardware path.  Several emulators (Bochs default text mode in
 * particular) accept the writes without actually moving the display
 * origin, leaving the visible window pinned while we paint into ring-
 * buffer positions past the visible 4000 bytes — the user sees what
 * looks like a hang.  Since the SW memmove is sub-microsecond there is
 * no observable upside in chasing this corner.  The DisplayBackend caps
 * slot stays available for real GPU drivers (i915 pan-display) where
 * HW scroll IS a real win and IS reliably specified.
 */

#include "hw_vga.h"
#include "../canvas.h"
#include "vmm.h"
#include "io.h"
#include "klib.h"

typedef struct {
    DisplayBackend base;
    unsigned char *vram;          /* identity-mapped at boot, Pull Map after vmm_init */
} HwVgaState;

static HwVgaState s_vga;

/* =========================================================================
 *  CRTC helpers
 * ========================================================================= */

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

/* =========================================================================
 *  Backend vtable
 * ========================================================================= */

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

    /* Draw-time quantisation: the cell carries #RRGGBB, the hardware takes
     * a 4+4-bit attribute. Runs share a colour, so memoise the last pair —
     * one nearest-palette search per run, not per cell. Palette-exact pairs
     * (everything the kernel itself prints) round-trip bit-for-bit. */
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
    /* All writes hit VRAM in DrawCells/Scroll/FillRow.  Nothing to flush. */
    (void)be; (void)row_lo; (void)row_hi;
}

static void op_DrawCaret(DisplayBackend *be, uint32_t col, uint32_t row, uint32_t fg)
{
    (void)fg;                      /* real CRTC cursor — colourless */
    if (col >= be->cols || row >= be->rows) return;
    cursor_to(col, row);
}

static void op_ActivatePullMap(DisplayBackend *be)
{
    (void)be;
    s_vga.vram = (unsigned char *)vmm_phys_to_virt(HW_VGA_BUF_ADDR);
    debug_printf("[VGA] Text buffer rebased to Pull Map: %p\n", s_vga.vram);
}

/* =========================================================================
 *  Initialisation
 * ========================================================================= */

/* ‼ THE HIGH BIT OF A BACKGROUND IS A COLOUR HERE, NOT A FLASH.
 *
 * An attribute byte is (bg << 4) | fg, and in the mode the firmware leaves
 * behind, bit 7 — the top bit of the background nibble — does not select a
 * colour: it makes the cell BLINK. Eight of the sixteen backgrounds were
 * therefore unusable, and nothing said so. What it cost was measured the day
 * a program first painted with them: lavalamp's default liquid (#ff3a6b)
 * projects to light red, so the body of every blob flashed at 2 Hz instead of
 * glowing, and draft's title bar (dark grey) flashed along with it. A picture
 * that blinks is not a picture with a bug in its colours — it is a picture
 * nobody can look at.
 *
 * Clearing bit 3 of Attribute Controller index 0x10 hands those eight back as
 * what they look like they are. Nothing is lost: no part of this system asks
 * for blinking text, and a cell that wants attention has twenty-four bits of
 * colour to ask with.
 *
 * ‼ THE FLIP-FLOP IS RESET TWICE, AND THE FIRST ATTEMPT HERE RESET IT ONCE.
 * Port 0x3C0 is index and data through one address, told apart by a flip-flop
 * that a read of 0x3DA puts into INDEX state and that every write to 0x3C0
 * toggles. Reading the value at 0x3C1 leaves the flip-flop in DATA state, so a
 * sequence that writes the index again straight afterwards writes it as DATA —
 * and the write meant for the data then lands as an INDEX, with bit 5 (PAS)
 * clear. Clearing PAS disconnects the palette and the screen goes BLACK and
 * stays black. Measured exactly that way: the console went dark on the first
 * BIOS boot after this function was added. Hence the second read of 0x3DA
 * before the write pair — and the index carrying 0x20 on the way in, which is
 * what puts PAS back.
 *
 * Interrupts are not disabled: this runs once, on the BSP, before any other
 * core exists, and an interrupt that wrote 0x3C0 would already have broken
 * every other attribute write in the kernel. */
static void hw_vga_blink_off(void)
{
    (void)inb(0x3DA);                 /* flip-flop -> index */
    outb(0x3C0, 0x10 | 0x20);         /* index 0x10, palette access kept on */
    uint8_t mode = inb(0x3C1);        /* flip-flop is left in DATA state here */

    (void)inb(0x3DA);                 /* -> index again, for the write pair */
    outb(0x3C0, 0x10 | 0x20);         /* index                              */
    outb(0x3C0, (uint8_t)(mode & ~0x08u));   /* data: blink enable cleared  */
}

DisplayBackend *HwVgaBackendInit(void)
{
    s_vga.vram = (unsigned char *)HW_VGA_BUF_ADDR;   /* identity at boot */

    hw_vga_blink_off();

    s_vga.base.caps  = 0;                            /* HW_SCROLL slot reserved for GPU drivers */
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
