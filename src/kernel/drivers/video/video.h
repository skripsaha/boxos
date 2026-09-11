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
void VideoNotifyReady(void);

void VideoSetColor(uint8_t attr);
void VideoSetColorRgb(uint32_t fg, uint32_t bg);
void VideoGetColorRgb(uint32_t *fg, uint32_t *bg);

void VideoPrintCharRgb(char ch, uint32_t fg, uint32_t bg);
void VideoPrintCharCur(char ch);
void VideoPrint(const char *str);
void VideoPrintNewline(void);

bool VideoPaintCells(uint32_t row, uint32_t col, uint32_t height, uint32_t width,
                     const TextCell *cells);
void VideoClearScreenRgb(uint32_t fg, uint32_t bg);
void VideoClearLineRgb(int line, uint32_t fg, uint32_t bg);
void VideoClearToEol(void);
void VideoScrollUp(void);

void VideoSetCursor(int x, int y);
int  VideoGetCursorX(void);
int  VideoGetCursorY(void);
void VideoUpdateCursor(void);

void VideoBatchBegin(void);
void VideoBatchEnd(void);

int         VideoGetCols(void);
int         VideoGetRows(void);
DisplayMode VideoGetMode(void);

#endif