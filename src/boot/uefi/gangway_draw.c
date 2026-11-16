/*
 * Gangway — what the loader draws on the screen itself: text, in the cells of
 * the imprint (boxos_imprint.h), the system's characters drawn ahead of time
 * from its font. The kernel's Emergency Screen presses them the same way
 * (boxos_imprint_press.h), and the display draws from the same font: a
 * character looks the one way from the loader on. The firmware's console is
 * not asked — its font differs from board to board and reads no UTF-8.
 *
 * The screen is the framebuffer the journey chose for the kernel (DrawUse,
 * after QueryGopFramebuffer), written straight, so it is drawn on after
 * ExitBootServices as before it; until then it is the firmware's mode as it
 * is (GopFramebufferNow), asked for when there is something to draw, and a
 * stop before the screen step leaves that mode as the firmware set it. The
 * cells are the size the Emergency Screen draws a screen as tall in
 * (BoxPressSize), from the top-left corner.
 *
 * A page is drawn whole every time — every cell of every row, the rest of a
 * line in the colours of the piece that ends it — so the panel redrawn on a
 * key changes what changed and nothing flickers; the screen is cleared once,
 * the first time it is drawn on, for the strip past the last whole cell.
 */

#include "gangway.h"
#include "boxos_imprint_data.h"
#include "boxos_imprint_press.h"
#include "boxos_utf8.h"
#include "boxos_color.h"
#include "video_colors.h"

typedef struct {
    FbInfo   fb;
    int      chosen;   /* DrawUse named it: the firmware is not asked again */
    int      drawn;    /* cleared and drawn on since it was taken */
    int      inked;    /* `inks` are attr `inks_attr`'s, in this screen's format */
    uint8_t  inks_attr;
    uint32_t inks[BOXOS_IMPRINT_INKED + 1u];
    uint32_t size;     /* the imprint's size the cells are */
    uint32_t cell_w;
    uint32_t cell_h;
    uint32_t cols;
    uint32_t rows;
} DrawScreen;

static DrawScreen g_screen;

/* Where the next cell goes: row `skip` of the text onto screen row `top`, and
 * `height` rows of it; the rows above and below are counted and not drawn,
 * and a pen that does not draw only counts. A line that fills its row moves
 * on when another cell comes, so a '\n' right after it ends that row rather
 * than leaving an empty one below. */
typedef struct {
    uint32_t col;
    uint32_t row;
    uint32_t skip;
    uint32_t top;
    uint32_t height;
    int      draw;
} DrawPen;

static void ScreenTake(const FbInfo *fb)
{
    MemCopy(&g_screen.fb, fb, sizeof(*fb));
    g_screen.drawn = 0;
    g_screen.inked = 0;
    g_screen.cols  = 0;
    g_screen.rows  = 0;
    if (!fb->addr) return;
    g_screen.size   = BoxPressSize(&BoxSystemImprint, fb->height);
    g_screen.cell_w = BoxSystemImprint.Sizes[g_screen.size].Width;
    g_screen.cell_h = BoxSystemImprint.Sizes[g_screen.size].Height;
    g_screen.cols   = fb->width / g_screen.cell_w;
    g_screen.rows   = fb->height / g_screen.cell_h;
}

void DrawUse(const FbInfo *fb)
{
    ScreenTake(fb);
    g_screen.chosen = 1;
}

/* Whether there is a screen to draw on — the one the journey chose, or until
 * it has chosen, the firmware's as it is now. */
static int ScreenReady(void)
{
    if (!g_screen.chosen) {
        FbInfo now = GopFramebufferNow();
        ScreenTake(&now);
    }
    return g_screen.cols != 0 && g_screen.rows != 0;
}

static void ScreenClear(void)
{
    uint32_t paper = BoxPixelEncode(BoxAttrBgRgb(VIDEO_ATTR_DEFAULT), g_screen.fb.format);
    volatile uint8_t *line = (volatile uint8_t *)(uintptr_t)g_screen.fb.addr;
    for (uint32_t y = 0; y < g_screen.fb.height; y++, line += g_screen.fb.stride) {
        volatile uint32_t *px = (volatile uint32_t *)line;
        for (uint32_t x = 0; x < g_screen.fb.width; x++) px[x] = paper;
    }
}

/* The inks of the last colours asked for are kept: a page is a few colours
 * over thousands of cells. */
static void PutCell(uint32_t col, uint32_t row, uint32_t character, uint8_t attr)
{
    if (!g_screen.inked || g_screen.inks_attr != attr) {
        BoxPressInks(BoxAttrBgRgb(attr), BoxAttrFgRgb(attr), g_screen.fb.format, g_screen.inks);
        g_screen.inks_attr = attr;
        g_screen.inked     = 1;
    }
    volatile uint8_t *at = (volatile uint8_t *)(uintptr_t)g_screen.fb.addr
                         + (uint64_t)row * g_screen.cell_h * g_screen.fb.stride
                         + (uint64_t)col * g_screen.cell_w * 4u;
    BoxPressGlyph(&BoxSystemImprint, g_screen.size, BoxImprintFind(&BoxSystemImprint, character), at,
                  g_screen.fb.stride, g_screen.inks);
}

static int PenShows(const DrawPen *pen)
{
    return pen->draw && pen->row >= pen->skip && pen->row - pen->skip < pen->height;
}

/* The rest of the pen's row, from its column on, blank in `attr`. */
static void PenRowEnd(const DrawPen *pen, uint8_t attr)
{
    if (!PenShows(pen)) return;
    for (uint32_t col = pen->col; col < g_screen.cols; col++) PutCell(col, pen->top + pen->row - pen->skip, ' ', attr);
}

static void PenText(DrawPen *pen, const DrawPiece *piece)
{
    for (uint32_t at = 0; at < piece->bytes;) {
        if (piece->text[at] == '\n') {
            PenRowEnd(pen, piece->attr);
            pen->col = 0;
            pen->row++;
            at++;
            continue;
        }
        uint32_t character;
        at += (uint32_t)BoxUtf8Next(piece->text + at, piece->bytes - at, &character);
        if (pen->col == g_screen.cols) {
            pen->col = 0;
            pen->row++;
        }
        if (PenShows(pen)) PutCell(pen->col, pen->top + pen->row - pen->skip, character, piece->attr);
        pen->col++;
    }
}

uint32_t DrawRows(void)
{
    return ScreenReady() ? g_screen.rows : 0;
}

uint32_t DrawCols(void)
{
    return ScreenReady() ? g_screen.cols : 0;
}

uint32_t DrawCount(const DrawPiece *pieces, uint32_t count)
{
    if (!ScreenReady()) return 0;
    DrawPen pen = {0, 0, 0, 0, 0, 0};
    for (uint32_t i = 0; i < count; i++) PenText(&pen, &pieces[i]);
    return pen.row + (pen.col ? 1u : 0u);
}

uint32_t DrawPage(const DrawPiece *pieces, uint32_t count, uint32_t skip, uint32_t top, uint32_t height)
{
    if (!ScreenReady() || top >= g_screen.rows) return 0;
    if (height > g_screen.rows - top) height = g_screen.rows - top;
    if (!g_screen.drawn) {
        ScreenClear();
        g_screen.drawn = 1;
    }

    DrawPen pen = {0, 0, skip, top, height, 1};
    for (uint32_t i = 0; i < count; i++) PenText(&pen, &pieces[i]);
    uint32_t filled = pen.row + (pen.col ? 1u : 0u);
    for (; pen.row < skip + height; pen.row++, pen.col = 0) PenRowEnd(&pen, VIDEO_ATTR_DEFAULT);

    /* Whatever the firmware mapped the framebuffer as, nothing drawn stays in
     * a write-combining buffer — behind the halt that follows a stop, or the
     * wait for the next key. */
    __asm__ volatile("sfence" ::: "memory");
    return filled;
}

/* Counted first, with a pen that only counts: the rows the pieces fill past
 * the screen's are the ones cut from the front. */
int DrawEnd(const DrawPiece *pieces, uint32_t count)
{
    if (!ScreenReady()) return 0;
    uint32_t filled = DrawCount(pieces, count);
    DrawPage(pieces, count, filled > g_screen.rows ? filled - g_screen.rows : 0, 0, g_screen.rows);
    return 1;
}
