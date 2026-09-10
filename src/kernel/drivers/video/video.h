#ifndef VIDEO_H
#define VIDEO_H

#include "ktypes.h"
#include "video_colors.h"
#include "boxos_color.h"
#include "text_cell.h"

typedef enum {
    DISPLAY_VGA_TEXT = 0,
    DISPLAY_GOP_FB   = 1,
} DisplayMode;

void VideoInit(void);
void VideoActivatePullMap(void);
void VideoInitFramebuffer(uint64_t phys_addr, uint32_t width, uint32_t height,
                          uint32_t stride, uint32_t format);
/* Publish the `display:ready` Touch event.  Call once Touch is up
 * (after guide_init).  Re-publish is idempotent. */
void VideoNotifyReady(void);

/* ───── Colour state — one full #RRGGBB pair ─────
 * VideoSetColor(attr) is the EXACT attribute converter for the kernel's own
 * %[E]/%[S]/… paths: palette-index → palette RGB, no rounding anywhere, so
 * the VGA backend's draw-time quantisation reproduces the identical
 * attribute byte. User-supplied colours arrive through the Rgb entry
 * points and keep all 24 bits. */
void VideoSetColor(uint8_t attr);
void VideoSetColorRgb(uint32_t fg, uint32_t bg);
void VideoGetColorRgb(uint32_t *fg, uint32_t *bg);

/* ───── Print path ───── */
void VideoPrintCharRgb(char ch, uint32_t fg, uint32_t bg);
void VideoPrintCharCur(char ch);           /* current colour pair */
void VideoPrint(const char *str);          /* current colour pair */
void VideoPrintNewline(void);

/* ───── Screen ops ───── */
/* Put a finished rectangle of cells on the screen. See CanvasPaint: nothing
 * is interpreted and the cursor stays where it was. False = it does not fit. */
bool VideoPaintCells(uint32_t row, uint32_t col, uint32_t height, uint32_t width,
                     const TextCell *cells);
void VideoClearScreenRgb(uint32_t fg, uint32_t bg);
void VideoClearLineRgb(int line, uint32_t fg, uint32_t bg);
void VideoClearToEol(void);                /* current colour pair */
void VideoScrollUp(void);

/* ───── Cursor ───── */
void VideoSetCursor(int x, int y);
int  VideoGetCursorX(void);
int  VideoGetCursorY(void);
void VideoUpdateCursor(void);

/* ───── Batch passthrough ───── */
void VideoBatchBegin(void);
void VideoBatchEnd(void);

int         VideoGetCols(void);
int         VideoGetRows(void);
DisplayMode VideoGetMode(void);

#endif /* VIDEO_H */
