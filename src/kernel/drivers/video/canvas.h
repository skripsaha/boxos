
#ifndef CANVAS_H
#define CANVAS_H

#include "ktypes.h"
#include "video_colors.h"
#include "boxos_color.h"

#include "text_cell.h"

typedef enum {
    DISP_CAP_HW_SCROLL = (1u << 0),
    DISP_CAP_HW_BITBLT = (1u << 1),
    DISP_CAP_PAGE_FLIP = (1u << 2),
    DISP_CAP_VSYNC     = (1u << 3),
    DISP_CAP_DPMS      = (1u << 4),
} DispCaps;

struct DisplayBackend;
typedef struct DisplayBackend DisplayBackend;

struct DisplayBackend {
    uint32_t caps;
    uint32_t cols;
    uint32_t rows;

    void (*DrawCells)(DisplayBackend *be,
                      uint32_t row, uint32_t col_lo, uint32_t col_hi,
                      const TextCell *cells_row);

    void (*Scroll)(DisplayBackend *be, uint32_t dy);

    void (*FillRow)(DisplayBackend *be, uint32_t row, uint32_t fg, uint32_t bg);

    void (*Present)(DisplayBackend *be, uint32_t row_lo, uint32_t row_hi);

    void (*DrawCaret)(DisplayBackend *be,
                      uint32_t col, uint32_t row, uint32_t fg);

    void (*ActivatePullMap)(DisplayBackend *be);
};

DisplayBackend *HwVgaBackendInit(void);
DisplayBackend *FbGopBackendInit(uint64_t phys, uint32_t width, uint32_t height,
                                 uint32_t stride, uint32_t format);

void CanvasInit(DisplayBackend *be);
void CanvasReplaceBackend(DisplayBackend *be);
void CanvasActivatePullMap(void);
void CanvasNotifyReady(void);

void CanvasBatchBegin(void);
void CanvasBatchEnd(void);

void CanvasForceReset(void);

void CanvasPrintChar(char c, uint32_t fg, uint32_t bg);

bool CanvasPaint(uint32_t row, uint32_t col, uint32_t height, uint32_t width,
                 const TextCell *cells);

void CanvasScrollUp(void);
void CanvasClearScreen(uint32_t fg, uint32_t bg);
void CanvasClearLine(int line, uint32_t fg, uint32_t bg);
void CanvasClearToEol(uint32_t fg, uint32_t bg);

void CanvasSetCursor(int x, int y);
int  CanvasGetCursorX(void);
int  CanvasGetCursorY(void);
void CanvasUpdateCursor(void);

int  CanvasGetCols(void);
int  CanvasGetRows(void);

#endif