#include "video.h"
#include "ops.h"
#include "hw_vga/hw_vga.h"
#include "fb_gop/fb_gop.h"
#include "vmm.h"
#include "serial.h"

static const DisplayOps *g_ops           = (void*)0;
static DisplayMode        g_display_mode  = DISPLAY_VGA_TEXT;
static uint8_t            g_current_color = VIDEO_ATTR_DEFAULT;

extern const DisplayOps hw_vga_ops;
extern const DisplayOps fb_gop_ops;

DisplayMode VideoGetMode(void)       { return g_display_mode; }
uint8_t     VideoGetColor(void)      { return g_current_color; }
void        VideoSetColor(uint8_t c) { g_current_color = c; }
void        VideoResetColor(void)    { g_current_color = VIDEO_ATTR_DEFAULT; }

void VideoInit(void)
{
    g_ops           = &hw_vga_ops;
    g_display_mode  = DISPLAY_VGA_TEXT;
    g_current_color = VIDEO_ATTR_DEFAULT;
    VideoClearScreen();
    VideoSetCursor(0, 0);
}

void VideoActivatePullMap(void)
{
    HwVgaActivatePullMap();
}

void VideoInitFramebuffer(uint64_t phys_addr, uint32_t width, uint32_t height,
                          uint32_t stride, uint32_t format)
{
    if (!phys_addr || !width || !height || !stride) return;
    if (!FbGopInit(phys_addr, width, height, stride, format)) {
        debug_printf("[VIDEO] GOP framebuffer init failed — staying in text mode\n");
        return;
    }
    g_ops          = &fb_gop_ops;
    g_display_mode = DISPLAY_GOP_FB;
    VideoClearScreen();
}

void VideoPrintChar(char ch, uint8_t attr)
{
    g_ops->PrintChar(ch, attr);
}

void VideoPrint(const char *str)
{
    while (*str)
        g_ops->PrintChar(*str++, g_current_color);
    g_ops->UpdateCursor();
}

void VideoPrintNewline(void)
{
    g_ops->PrintChar('\n', g_current_color);
    g_ops->UpdateCursor();
}

void VideoClearScreen(void)            { g_ops->ClearScreen(); }
void VideoClearLine(int line)          { g_ops->ClearLine(line); }
void VideoClearToEol(void)             { g_ops->ClearToEol(g_current_color); }
void VideoScrollUp(void)               { g_ops->ScrollUp(); }
void VideoChangeBackground(uint8_t bg) { g_ops->ChangeBackground(bg); }

void VideoSetCursor(int x, int y) { g_ops->SetCursor(x, y); }
int  VideoGetCursorX(void)        { return g_ops->GetCursorX(); }
int  VideoGetCursorY(void)        { return g_ops->GetCursorY(); }
void VideoUpdateCursor(void)      { g_ops->UpdateCursor(); }

int VideoGetCols(void) { return (int)g_ops->GetCols(); }
int VideoGetRows(void) { return (int)g_ops->GetRows(); }

void VideoPrintError(const char *str)
{
    while (*str) g_ops->PrintChar(*str++, VIDEO_ATTR_ERROR);
    g_ops->UpdateCursor();
}

void VideoPrintSuccess(const char *str)
{
    while (*str) g_ops->PrintChar(*str++, VIDEO_ATTR_SUCCESS);
    g_ops->UpdateCursor();
}

void VideoPrintHint(const char *str)
{
    while (*str) g_ops->PrintChar(*str++, VIDEO_ATTR_HINT);
    g_ops->UpdateCursor();
}
