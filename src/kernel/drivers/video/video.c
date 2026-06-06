/*
 * video.c — public Video* API surface.
 *
 * Translates the long-standing Video* names (called from kprintf, klib_print,
 * hardware_ops, panic/banner code, etc.) into Canvas operations.  Module-
 * local state is limited to:
 *   • g_current_color — the "current attribute" that callers set via
 *     VideoSetColor and observe via VideoGetColor.  Lives here because the
 *     Canvas accepts an explicit per-call attr; the running default is a
 *     concern of the API surface, not the engine.
 *   • g_display_mode — coarse-grained enum so callers (main.c boot log)
 *     can report whether GOP or VGA-text is active.
 *
 * The backend switch (VGA text → GOP under UEFI) goes through
 * CanvasReplaceBackend so cells survive the transition.
 */

#include "video.h"
#include "canvas.h"
#include "hw_vga/hw_vga.h"
#include "fb_gop/fb_gop.h"
#include "klib.h"

static DisplayMode g_display_mode  = DISPLAY_VGA_TEXT;
static uint8_t     g_current_color = VIDEO_ATTR_DEFAULT;

/* =========================================================================
 *  Lifecycle
 * ========================================================================= */

void VideoInit(void)
{
    DisplayBackend *be = HwVgaBackendInit();
    if (!be) return;
    CanvasInit(be);
    g_display_mode  = DISPLAY_VGA_TEXT;
    g_current_color = VIDEO_ATTR_DEFAULT;
    CanvasSetCursor(0, 0);
    CanvasUpdateCursor();
}

void VideoActivatePullMap(void)
{
    CanvasActivatePullMap();
}

void VideoInitFramebuffer(uint64_t phys_addr, uint32_t width, uint32_t height,
                          uint32_t stride, uint32_t format)
{
    if (!phys_addr || !width || !height || !stride) return;

    DisplayBackend *be = FbGopBackendInit(phys_addr, width, height, stride, format);
    if (!be) {
        debug_printf("[VIDEO] GOP framebuffer init failed — staying in text mode\n");
        return;
    }
    CanvasReplaceBackend(be);
    g_display_mode = DISPLAY_GOP_FB;
}

void VideoNotifyReady(void)
{
    CanvasNotifyReady();
}

/* =========================================================================
 *  Mode + color
 * ========================================================================= */

DisplayMode VideoGetMode(void)        { return g_display_mode; }
uint8_t     VideoGetColor(void)       { return g_current_color; }
void        VideoSetColor(uint8_t c)  { g_current_color = c; }
void        VideoResetColor(void)     { g_current_color = VIDEO_ATTR_DEFAULT; }

/* =========================================================================
 *  Print path
 * ========================================================================= */

void VideoPrintChar(char ch, uint8_t attr)
{
    CanvasPrintChar(ch, attr);
}

void VideoPrint(const char *str)
{
    if (!str) return;
    CanvasBatchBegin();
    while (*str) CanvasPrintChar(*str++, g_current_color);
    CanvasBatchEnd();
}

void VideoPrintNewline(void)
{
    CanvasPrintChar('\n', g_current_color);
}

static void video_print_attr(const char *str, uint8_t attr)
{
    if (!str) return;
    CanvasBatchBegin();
    while (*str) CanvasPrintChar(*str++, attr);
    CanvasBatchEnd();
}

void VideoPrintError(const char *str)   { video_print_attr(str, VIDEO_ATTR_ERROR);   }
void VideoPrintSuccess(const char *str) { video_print_attr(str, VIDEO_ATTR_SUCCESS); }
void VideoPrintHint(const char *str)    { video_print_attr(str, VIDEO_ATTR_HINT);    }

/* =========================================================================
 *  Screen ops
 * ========================================================================= */

void VideoClearScreen(void)             { CanvasClearScreen(); }
void VideoClearLine(int line)           { CanvasClearLine(line); }
void VideoClearToEol(void)              { CanvasClearToEol(g_current_color); }
void VideoScrollUp(void)                { CanvasScrollUp(); }
void VideoChangeBackground(uint8_t bg)  { CanvasChangeBackground(bg); }

/* =========================================================================
 *  Cursor
 * ========================================================================= */

void VideoSetCursor(int x, int y)       { CanvasSetCursor(x, y); }
int  VideoGetCursorX(void)              { return CanvasGetCursorX(); }
int  VideoGetCursorY(void)              { return CanvasGetCursorY(); }
void VideoUpdateCursor(void)            { CanvasUpdateCursor(); }

int  VideoGetCols(void)                 { return CanvasGetCols(); }
int  VideoGetRows(void)                 { return CanvasGetRows(); }

/* =========================================================================
 *  Batch passthrough
 * ========================================================================= */

void VideoBatchBegin(void)              { CanvasBatchBegin(); }
void VideoBatchEnd(void)                { CanvasBatchEnd(); }
