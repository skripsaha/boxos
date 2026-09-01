/*
 * video.c — public Video* API surface.
 *
 * Translates the long-standing Video* names (called from kprintf, klib_print,
 * hardware_ops, panic/banner code, etc.) into Canvas operations.  Module-
 * local state is limited to:
 *   • g_cur_fg / g_cur_bg — the current colour pair, full #RRGGBB.  Lives
 *     here because the Canvas accepts an explicit per-call pair; the running
 *     default is a concern of the API surface, not the engine.  The kernel's
 *     own attribute paths enter through VideoSetColor, which converts a
 *     4+4-bit attribute to its exact palette RGB — the VGA backend's
 *     draw-time quantisation then reproduces the identical attribute byte.
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

static DisplayMode g_display_mode = DISPLAY_VGA_TEXT;
static uint32_t    g_cur_fg;      /* set in VideoInit */
static uint32_t    g_cur_bg;

/* =========================================================================
 *  Lifecycle
 * ========================================================================= */

void VideoInit(void)
{
    DisplayBackend *be = HwVgaBackendInit();
    if (!be) return;
    CanvasInit(be);
    g_display_mode = DISPLAY_VGA_TEXT;
    g_cur_fg = BoxAttrFgRgb(VIDEO_ATTR_DEFAULT);
    g_cur_bg = BoxAttrBgRgb(VIDEO_ATTR_DEFAULT);
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
 *  Mode + colour
 * ========================================================================= */

DisplayMode VideoGetMode(void) { return g_display_mode; }

void VideoSetColor(uint8_t attr)
{
    g_cur_fg = BoxAttrFgRgb(attr);
    g_cur_bg = BoxAttrBgRgb(attr);
}

void VideoSetColorRgb(uint32_t fg, uint32_t bg)
{
    g_cur_fg = BoxColorResolveFg(fg);
    g_cur_bg = BoxColorResolveBg(bg);
}

void VideoGetColorRgb(uint32_t *fg, uint32_t *bg)
{
    if (fg) *fg = g_cur_fg;
    if (bg) *bg = g_cur_bg;
}

/* =========================================================================
 *  Print path
 * ========================================================================= */

void VideoPrintCharRgb(char ch, uint32_t fg, uint32_t bg)
{
    CanvasPrintChar(ch, BoxColorResolveFg(fg), BoxColorResolveBg(bg));
}

void VideoPrintCharCur(char ch)
{
    CanvasPrintChar(ch, g_cur_fg, g_cur_bg);
}

void VideoPrint(const char *str)
{
    if (!str) return;
    CanvasBatchBegin();
    while (*str) CanvasPrintChar(*str++, g_cur_fg, g_cur_bg);
    CanvasBatchEnd();
}

void VideoPrintNewline(void)
{
    CanvasPrintChar('\n', g_cur_fg, g_cur_bg);
}

/* =========================================================================
 *  Screen ops
 * ========================================================================= */

void VideoClearScreenRgb(uint32_t fg, uint32_t bg)
{
    CanvasClearScreen(BoxColorResolveFg(fg), BoxColorResolveBg(bg));
}

void VideoClearLineRgb(int line, uint32_t fg, uint32_t bg)
{
    CanvasClearLine(line, BoxColorResolveFg(fg), BoxColorResolveBg(bg));
}

void VideoClearToEol(void)
{
    CanvasClearToEol(g_cur_fg, g_cur_bg);
}

void VideoScrollUp(void)
{
    CanvasScrollUp();
}

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
