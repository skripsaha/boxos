/*
 * emergency_screen.c — the log ring's tail on the glass. See emergency_screen.h.
 *
 * One state machine reads the ring a byte at a time: the pen. It turns bytes
 * into cells — '\n' '\r' '\b' '\t' as a console does, UTF-8 folded at the font
 * (boxos_glyph_fold.h), any other byte one cell — and either draws them or
 * only counts where they would go. Counting is how the tail is found: a scroll
 * is never a copy of what is on the glass (a read of write-combining memory
 * costs a bus round trip a word), it is the ring's last lines drawn afresh.
 *
 * The jump. When a line ends below the last row, the glass is drawn again from
 * the tail with half its rows, and the next half a screen of lines goes down
 * below them without a redraw. A full redraw per line would put a whole
 * framebuffer of writes behind every line the boot says.
 *
 * ‼ NO DRAW ENDS WITHOUT AN SFENCE. Pixels go through the kernel's
 * write-combining mapping and sit in the core's WC buffers until something
 * drains them; a panic ends in `cli; hlt`, which drains nothing, and the last
 * lines of its account would never reach the glass.
 *
 * ‼ AND EVERY ROW ASKS WHETHER A PANIC HAS BEEN CLAIMED. The panic's NMIs are
 * not waited for (klib_panic.c), so a core can still be in a draw here while
 * the owner takes the glass; it stops at the end of the row it is on rather
 * than drawing on over the account.
 */

#include "emergency_screen.h"
#include "screen.h"           /* SCREEN_TEXT_PHYS */
#include "klib.h"
#include "klib_logring.h"
#include "klib_panic.h"       /* PanicStandIfClaimedElsewhere */
#include "vmm.h"              /* vmm_phys_to_virt */
#include "boxos_screen.h"
#include "boxos_font.h"
#include "boxos_pixel.h"
#include "boxos_color.h"
#include "boxos_glyph_fold.h"
#include "video_colors.h"

/* Where the next cell goes, and a character the ring has begun but not
 * finished. A multi-byte character takes its lead byte's tone. */
typedef struct {
    uint32_t col;
    uint32_t row;
    uint8_t  seq[4];
    uint8_t  have;
    uint8_t  need;
    uint8_t  tone;
} EmergencyPen;

typedef struct {
    uint32_t          kind;      /* SCREEN_KIND_* */
    volatile uint8_t *pixels;    /* PIXELS: the kernel's WC mapping */
    uint32_t          width;
    uint32_t          height;
    uint32_t          stride;
    uint32_t          format;
    uint32_t          cols;
    uint32_t          rows;
    bool              shown;     /* the glass is ours to draw on */
    uint64_t          drawn;     /* the ring position drawn up to */
    EmergencyPen      pen;
} EmergencyGlass;

static EmergencyGlass g_glass;

/* The tone of a line, in the colours this screen draws it in. */
static const uint8_t g_tone_attr[LOG_TONE_COUNT] = {
    [LOG_TONE_PLAIN]   = VIDEO_ATTR_DEFAULT,
    [LOG_TONE_ERROR]   = VIDEO_ATTR_ERROR,
    [LOG_TONE_SUCCESS] = VIDEO_ATTR_SUCCESS,
    [LOG_TONE_WARNING] = VIDEO_ATTR_WARNING,
    [LOG_TONE_HINT]    = VIDEO_ATTR_HINT,
};

static uint8_t ToneAttr(uint8_t tone)
{
    return tone < LOG_TONE_COUNT ? g_tone_attr[tone] : VIDEO_ATTR_DEFAULT;
}

static void Fence(void)
{
    __asm__ volatile("sfence" ::: "memory");
}

/* =========================================================================
 *  The glass
 * ========================================================================= */

/* The text page through whichever mapping is live now — identity before
 * vmm_init, the Pull Map after. Asked every time and never kept, so no draw
 * can go through an address that has just stopped existing. */
static volatile uint8_t *TextPage(void)
{
    return (volatile uint8_t *)vmm_phys_to_virt((uintptr_t)SCREEN_TEXT_PHYS);
}

static void PutCell(uint32_t col, uint32_t row, uint8_t ch, uint8_t tone)
{
    if (col >= g_glass.cols || row >= g_glass.rows) return;
    uint8_t attr = ToneAttr(tone);

    if (g_glass.kind == SCREEN_KIND_TEXT) {
        volatile uint8_t *cell = TextPage() + ((size_t)row * g_glass.cols + col) * 2u;
        cell[0] = ch;
        cell[1] = attr;
        return;
    }

    uint32_t fg = BoxPixelEncode(BoxAttrFgRgb(attr), g_glass.format);
    uint32_t bg = BoxPixelEncode(BoxAttrBgRgb(attr), g_glass.format);
    const unsigned char *glyph = BoxFontGlyphs[ch];
    volatile uint8_t *line = g_glass.pixels
                           + (size_t)row * BOXOS_FONT_HEIGHT * g_glass.stride
                           + (size_t)col * BOXOS_FONT_WIDTH * 4u;
    for (uint32_t y = 0; y < BOXOS_FONT_HEIGHT; y++, line += g_glass.stride) {
        volatile uint32_t *px = (volatile uint32_t *)line;
        uint8_t bits = glyph[y];
        for (uint32_t x = 0; x < BOXOS_FONT_WIDTH; x++)
            px[x] = (bits & (0x80u >> x)) ? fg : bg;
    }
}

static void ClearGlass(void)
{
    if (g_glass.kind == SCREEN_KIND_TEXT) {
        volatile uint8_t *page = TextPage();
        for (uint32_t row = 0; row < g_glass.rows; row++) {
            PanicStandIfClaimedElsewhere();
            for (uint32_t col = 0; col < g_glass.cols; col++) {
                page[((size_t)row * g_glass.cols + col) * 2u]      = ' ';
                page[((size_t)row * g_glass.cols + col) * 2u + 1u] = VIDEO_ATTR_DEFAULT;
            }
        }
        return;
    }

    uint32_t bg = BoxPixelEncode(BoxAttrBgRgb(VIDEO_ATTR_DEFAULT), g_glass.format);
    volatile uint8_t *line = g_glass.pixels;
    for (uint32_t y = 0; y < g_glass.height; y++, line += g_glass.stride) {
        if ((y % BOXOS_FONT_HEIGHT) == 0) PanicStandIfClaimedElsewhere();
        volatile uint32_t *px = (volatile uint32_t *)line;
        for (uint32_t x = 0; x < g_glass.width; x++) px[x] = bg;
    }
}

/* =========================================================================
 *  The pen — bytes into cells, drawn or only counted
 * ========================================================================= */

/* Bytes the lead byte promises, itself included; 0 = not a lead byte
 * (0x80-0xBF stray continuation, 0xC0/0xC1 overlong, 0xF5+ past U+10FFFF). */
static uint8_t Utf8Promise(uint8_t b)
{
    if (b >= 0xC2u && b <= 0xDFu) return 2;
    if (b >= 0xE0u && b <= 0xEFu) return 3;
    if (b >= 0xF0u && b <= 0xF4u) return 4;
    return 0;
}

/* A line ends. Below the last row nothing is drawn; the caller draws the tail
 * again. A drawing pen asks about a panic once a row. */
static void PenNewline(EmergencyPen *pen, bool draw)
{
    pen->col = 0;
    pen->row++;
    if (draw) PanicStandIfClaimedElsewhere();
}

static void PenCell(EmergencyPen *pen, uint8_t ch, uint8_t tone, bool draw)
{
    if (draw) PutCell(pen->col, pen->row, ch, tone);
    if (++pen->col >= g_glass.cols) PenNewline(pen, draw);
}

static void PenAscii(EmergencyPen *pen, uint8_t b, uint8_t tone, bool draw)
{
    switch (b) {
    case '\n':
        PenNewline(pen, draw);
        return;
    case '\r':
        pen->col = 0;
        return;
    case '\b':
        if (pen->col > 0) {
            pen->col--;
            if (draw) PutCell(pen->col, pen->row, ' ', tone);
        }
        return;
    case '\t': {
        uint32_t target = (pen->col + 8u) & ~7u;
        if (target >= g_glass.cols) PenNewline(pen, draw);
        else                        pen->col = target;
        return;
    }
    default:
        PenCell(pen, b, tone, draw);
        return;
    }
}

/* What was held of a character that never finished goes down as its own raw
 * cells, in the tone it was begun in; the byte that broke it starts over. */
static void PenSpill(EmergencyPen *pen, bool draw)
{
    uint8_t have = pen->have;
    pen->have = 0;
    pen->need = 0;
    for (uint8_t i = 0; i < have; i++)
        PenCell(pen, pen->seq[i], pen->tone, draw);
}

static void PenByte(EmergencyPen *pen, uint8_t b, uint8_t tone, bool draw)
{
    if (pen->need) {
        if ((b & 0xC0u) == 0x80u) {
            pen->seq[pen->have++] = b;
            if (pen->have < pen->need) return;

            uint32_t cp = 0;
            if (utf8_decode((const char *)pen->seq, &cp) == (int)pen->need) {
                char cells[BOXOS_GLYPH_FOLD_MAX];
                int  n = BoxGlyphFoldCodepoint(cp, cells);
                pen->have = 0;
                pen->need = 0;
                for (int i = 0; i < n; i++)
                    PenCell(pen, (uint8_t)cells[i], pen->tone, draw);
                return;
            }
            PenSpill(pen, draw);
            return;
        }
        PenSpill(pen, draw);
    }

    if (b < 0x80u) { PenAscii(pen, b, tone, draw); return; }

    uint8_t need = Utf8Promise(b);
    if (!need) { PenCell(pen, b, tone, draw); return; }

    pen->seq[0] = b;
    pen->have   = 1;
    pen->need   = need;
    pen->tone   = tone;
}

/* =========================================================================
 *  The tail
 * ========================================================================= */

static uint64_t RingOldest(void)
{
    uint64_t written = LogRingWritten();
    return written > (uint64_t)LOGRING_CAPACITY ? written - (uint64_t)LOGRING_CAPACITY : 0;
}

/* Where to begin so that [begin, end) fills at most `lines` rows. The look
 * back is bounded by what `lines` rows can hold — four bytes a cell at most,
 * and a '\n' a row — and begins at a line's start when it can. */
static uint64_t TailBegin(uint64_t end, uint32_t lines)
{
    uint64_t oldest = RingOldest();
    uint64_t budget = (uint64_t)lines * ((uint64_t)g_glass.cols * 4u + 1u);
    uint64_t begin  = (end - oldest > budget) ? end - budget : oldest;

    if (begin > oldest) {
        uint64_t p = begin;
        while (p < end && LogRingByteAt(p - 1u) != '\n') p++;
        if (p < end) begin = p;
    }

    EmergencyPen count = {0};
    for (uint64_t p = begin; p < end; p++)
        PenByte(&count, (uint8_t)LogRingByteAt(p), LOG_TONE_PLAIN, false);
    if (count.row < lines) return begin;

    uint32_t skip = count.row - (lines - 1u);
    EmergencyPen walk = {0};
    uint64_t p = begin;
    while (p < end && walk.row < skip)
        PenByte(&walk, (uint8_t)LogRingByteAt(p++), LOG_TONE_PLAIN, false);
    return p;
}

/* The glass cleared and [TailBegin(end, lines), end) drawn on it. */
static void DrawTail(uint64_t end, uint32_t lines)
{
    uint64_t begin = TailBegin(end, lines);
    ClearGlass();
    memset(&g_glass.pen, 0, sizeof(g_glass.pen));
    for (uint64_t p = begin; p < end; p++)
        PenByte(&g_glass.pen, (uint8_t)LogRingByteAt(p), LogRingToneAt(p), true);
    g_glass.drawn = end;
}

static uint32_t JumpLines(void)
{
    return g_glass.rows > 1u ? g_glass.rows / 2u : 1u;
}

/* =========================================================================
 *  Public
 * ========================================================================= */

void EmergencyScreenUseText(uint32_t cols, uint32_t rows)
{
    g_glass.kind   = SCREEN_KIND_TEXT;
    g_glass.pixels = NULL;
    g_glass.cols   = cols;
    g_glass.rows   = rows;
    if (g_glass.shown) {
        DrawTail(LogRingWritten(), g_glass.rows);
        Fence();
    }
}

void EmergencyScreenUsePixels(volatile uint8_t *pixels, uint32_t width,
                              uint32_t height, uint32_t stride, uint32_t format)
{
    g_glass.kind   = SCREEN_KIND_PIXELS;
    g_glass.pixels = pixels;
    g_glass.width  = width;
    g_glass.height = height;
    g_glass.stride = stride;
    g_glass.format = format;
    g_glass.cols   = width  / BOXOS_FONT_WIDTH;
    g_glass.rows   = height / BOXOS_FONT_HEIGHT;
    if (g_glass.shown) {
        DrawTail(LogRingWritten(), g_glass.rows);
        Fence();
    }
}

void EmergencyScreenShow(void)
{
    g_glass.shown = true;
    DrawTail(LogRingWritten(), g_glass.rows);
    Fence();
}

void EmergencyScreenHide(void)
{
    g_glass.shown = false;
}

void EmergencyScreenFollow(void)
{
    if (!g_glass.shown) return;

    /* Overtaken by more than the ring holds: what was not drawn is gone from
     * it, and the tail is what there is to show. */
    if (g_glass.drawn < RingOldest()) {
        DrawTail(LogRingWritten(), g_glass.rows);
        Fence();
        return;
    }

    uint64_t written = LogRingWritten();
    while (g_glass.drawn < written) {
        uint64_t p = g_glass.drawn++;
        PenByte(&g_glass.pen, (uint8_t)LogRingByteAt(p), LogRingToneAt(p), true);
        if (g_glass.pen.row >= g_glass.rows)
            DrawTail(g_glass.drawn, JumpLines());
    }
    Fence();
}

void EmergencyScreenPanicTake(void)
{
    g_glass.shown = true;
    DrawTail(LogRingWritten(), g_glass.rows);
    Fence();
}

void EmergencyScreenRedraw(void)
{
    if (!g_glass.shown) return;
    DrawTail(LogRingWritten(), g_glass.rows);
    Fence();
}
