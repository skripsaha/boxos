#ifndef VIDEO_H
#define VIDEO_H

#include "ktypes.h"
#include "video_colors.h"

typedef enum {
    DISPLAY_VGA_TEXT = 0,
    DISPLAY_GOP_FB   = 1,
} DisplayMode;

void VideoInit(void);
void VideoActivatePullMap(void);
void VideoInitFramebuffer(uint64_t phys_addr, uint32_t width, uint32_t height,
                          uint32_t stride, uint32_t format);

void VideoPrintChar(char ch, uint8_t attr);
void VideoPrint(const char *str);
void VideoPrintNewline(void);
void VideoPrintError(const char *str);
void VideoPrintSuccess(const char *str);
void VideoPrintHint(const char *str);

void VideoClearScreen(void);
void VideoClearLine(int line);
void VideoClearToEol(void);
void VideoScrollUp(void);
void VideoChangeBackground(uint8_t new_bg);

void VideoSetCursor(int x, int y);
int  VideoGetCursorX(void);
int  VideoGetCursorY(void);
void VideoUpdateCursor(void);

void    VideoSetColor(uint8_t color);
uint8_t VideoGetColor(void);
void    VideoResetColor(void);

int         VideoGetCols(void);
int         VideoGetRows(void);
DisplayMode VideoGetMode(void);

#endif /* VIDEO_H */
