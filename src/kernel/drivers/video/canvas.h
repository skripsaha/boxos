/*
 * canvas.h — text-console rendering engine for BoxOS.
 *
 * Canvas owns the logical text matrix (cells[][]) and a per-batch RenderPlan
 * that accumulates damage (per-row dirty bitmap + col_lo/col_hi range), a
 * coalesced scroll counter, and a deferred caret position. At commit time
 * (CanvasBatchEnd), the plan is applied to a DisplayBackend in three phases:
 *
 *   1. backend->Scroll(dy) — ONE coalesced shift covers every '\n' in the
 *      batch, regardless of how many newlines occurred. Backends advertising
 *      DISP_CAP_HW_SCROLL handle this via CRTC Start Address (VGA text) or
 *      GPU pan-display (future GPU drivers) — zero memory traffic. Backends
 *      without HW scroll do a single SW shift of their internal surface.
 *
 *   2. backend->DrawCells(row, col_lo, col_hi, cells_row) — render the
 *      tight damage range for each dirty row. Partial-row updates (cursor
 *      blink, status line refresh) flush only the affected pixel columns,
 *      not a full scanline strip.
 *
 *   3. backend->Present(row_lo, row_hi) — flush the bounding damage
 *      rectangle to the device. No-op for backends without an internal
 *      surface (VGA text mode writes go straight to VRAM in step 2).
 *
 * Re-entrant: CanvasBatchBegin/End nest via depth counter. Only the
 * outermost commit fires the actual backend pipeline. This lets the
 * Manifest dispatcher wrap a whole op stream in a single commit even
 * when each individual op already brackets itself.
 *
 * Single-canvas design: the kernel exposes one global text console.
 * Multi-display + multi-tty is a future Cabin-addressable surface model.
 */

#ifndef CANVAS_H
#define CANVAS_H

#include "ktypes.h"
#include "video_colors.h"
#include "boxos_color.h"

/* One character cell — full #RRGGBB pair per cell (the Color/Canvas
 * principle: colour is cell metadata, never an in-band byte). The GOP
 * backend renders fg/bg exactly; the VGA text backend quantises the pair
 * to its 16-colour attribute at draw time.
 *
 * The layout is shared with userspace (CanvasPaint / HW_VGA_PAINT hand a
 * rectangle of these straight across), so it lives in the one header both
 * sides include. */
#include "text_cell.h"

typedef enum {
    /* Backend implements Scroll() via a HW path (CRTC offset / pan-display).
     * Canvas can rely on Scroll(dy) being O(1) instead of O(stride*text_h). */
    DISP_CAP_HW_SCROLL = (1u << 0),
    /* Backend can perform intra-VRAM rectangle copies via a 2D engine.
     * Reserved for future GPU drivers. */
    DISP_CAP_HW_BITBLT = (1u << 1),
    /* Backend supports atomic front/back buffer flipping (vsync-aligned). */
    DISP_CAP_PAGE_FLIP = (1u << 2),
    /* Backend exposes vblank-driven callback. Reserved. */
    DISP_CAP_VSYNC     = (1u << 3),
    /* Backend can park the display panel for power management. */
    DISP_CAP_DPMS      = (1u << 4),
} DispCaps;

struct DisplayBackend;
typedef struct DisplayBackend DisplayBackend;

struct DisplayBackend {
    uint32_t caps;
    uint32_t cols;
    uint32_t rows;

    /* Render cells[row][col_lo .. col_hi) into the backend's surface
     * (shadow buffer for framebuffer backends, VRAM for text-mode). */
    void (*DrawCells)(DisplayBackend *be,
                      uint32_t row, uint32_t col_lo, uint32_t col_hi,
                      const TextCell *cells_row);

    /* Scroll the visible content up by dy rows. Backend updates its
     * internal surface; for HW backends the visible state changes
     * immediately, for SW backends Present() is still required to
     * flush the shifted shadow to VRAM. */
    void (*Scroll)(DisplayBackend *be, uint32_t dy);

    /* Fill an entire row with the given colour pair (used for clear-line
     * and post-clear-screen initialisation). */
    void (*FillRow)(DisplayBackend *be, uint32_t row, uint32_t fg, uint32_t bg);

    /* Flush damage rectangle [row_lo, row_hi) to the device. No-op for
     * backends that have already written to VRAM in DrawCells/Scroll. */
    void (*Present)(DisplayBackend *be, uint32_t row_lo, uint32_t row_hi);

    /* Optional caret rendering. NULL → caret is invisible. */
    void (*DrawCaret)(DisplayBackend *be,
                      uint32_t col, uint32_t row, uint32_t fg);

    /* Optional Pull Map rebase for backends mapped via identity early-boot
     * addresses (legacy VGA text mode at 0xB8000). */
    void (*ActivatePullMap)(DisplayBackend *be);
};

/* ───── Backend factories ───── */
DisplayBackend *HwVgaBackendInit(void);
DisplayBackend *FbGopBackendInit(uint64_t phys, uint32_t width, uint32_t height,
                                 uint32_t stride, uint32_t format);

/* ───── Canvas lifecycle ───── */
void CanvasInit(DisplayBackend *be);
void CanvasReplaceBackend(DisplayBackend *be);
void CanvasActivatePullMap(void);
void CanvasNotifyReady(void);

/* ───── Batch + rendering ───── */
void CanvasBatchBegin(void);
void CanvasBatchEnd(void);

/* Panic / fatal-path recovery — forcibly resets the batch counter so a
 * subsequent kprintf commits immediately.  Discards any pending plan. */
void CanvasForceReset(void);

void CanvasPrintChar(char c, uint32_t fg, uint32_t bg);

/* Lay a rectangle of finished cells into the matrix at (row, col), row-major,
 * `width` cells per row of the source. Nothing is interpreted: a '\n' in a
 * cell is a glyph, not a line break, and the cursor does not move — this is a
 * picture being put down, not a line being said. Returns false when the
 * rectangle does not fit the screen, so the caller can say so rather than
 * paint a smaller one and call it done. */
bool CanvasPaint(uint32_t row, uint32_t col, uint32_t height, uint32_t width,
                 const TextCell *cells);

void CanvasScrollUp(void);
void CanvasClearScreen(uint32_t fg, uint32_t bg);
void CanvasClearLine(int line, uint32_t fg, uint32_t bg);
void CanvasClearToEol(uint32_t fg, uint32_t bg);

/* ───── Cursor ───── */
void CanvasSetCursor(int x, int y);
int  CanvasGetCursorX(void);
int  CanvasGetCursorY(void);
void CanvasUpdateCursor(void);

/* ───── Geometry ───── */
int  CanvasGetCols(void);
int  CanvasGetRows(void);

#endif /* CANVAS_H */
