#ifndef BOX_COLOR_H
#define BOX_COLOR_H

#include "box/types.h"

/*
 * Color — 24-bit RGB packed into uint32_t (0x00RRGGBB).
 *
 * BoxOS color model is not ANSI-escape-codes: there are no in-band escape
 * sequences in text streams. Color is metadata, attached to a text segment
 * via a structured op (printf "%color" specifier produces one Manifest op
 * per colored run). One printf call → one syscall regardless of how many
 * color changes occur inside.
 *
 * Display-backend negotiation:
 *   - GOP framebuffer: full 24-bit RGB rendered directly (when supported).
 *   - VGA text mode:   quantize to 16-color palette nearest-neighbour at the
 *                      boxlib boundary; transparent to callers.
 *
 * Reserved sentinels:
 *   COLOR_DEFAULT   "use cabin default" — VGA: light-gray on black.
 *   COLOR_INHERIT   "keep currently set background" (use as bg only).
 */

typedef uint32_t Color;

#define COLOR_RGB(r, g, b)  ((Color)(((uint32_t)((r) & 0xFF) << 16) | \
                                     ((uint32_t)((g) & 0xFF) <<  8) | \
                                     ((uint32_t)((b) & 0xFF))))

#define COLOR_R(c)  (((c) >> 16) & 0xFFu)
#define COLOR_G(c)  (((c) >>  8) & 0xFFu)
#define COLOR_B(c)  ((c) & 0xFFu)

/* Sentinels — not valid RGB triples (high byte set) so they never collide
 * with a legitimate COLOR_RGB() value. */
#define COLOR_DEFAULT  ((Color)0xFE000000u)
#define COLOR_INHERIT  ((Color)0xFF000000u)

/* ------------------------------------------------------------------------- */
/* Standard BoxOS palette — RGB triples chosen to look readable on black bg.
 * Names are descriptive, not POSIX/ANSI: this is BoxOS, not a terminal. */

#define COLOR_BLACK         COLOR_RGB(0x00, 0x00, 0x00)
#define COLOR_WHITE         COLOR_RGB(0xFF, 0xFF, 0xFF)

#define COLOR_DARK_GRAY     COLOR_RGB(0x55, 0x55, 0x55)
#define COLOR_LIGHT_GRAY    COLOR_RGB(0xAA, 0xAA, 0xAA)

#define COLOR_RED           COLOR_RGB(0xE0, 0x40, 0x40)
#define COLOR_GREEN         COLOR_RGB(0x40, 0xC0, 0x40)
#define COLOR_BLUE          COLOR_RGB(0x40, 0x80, 0xE0)
#define COLOR_YELLOW        COLOR_RGB(0xE0, 0xC0, 0x40)
#define COLOR_CYAN          COLOR_RGB(0x40, 0xC0, 0xC0)
#define COLOR_MAGENTA       COLOR_RGB(0xC0, 0x40, 0xC0)

#define COLOR_DARK_RED      COLOR_RGB(0xA0, 0x00, 0x00)
#define COLOR_DARK_GREEN    COLOR_RGB(0x00, 0x80, 0x00)
#define COLOR_DARK_BLUE     COLOR_RGB(0x00, 0x00, 0xA0)
#define COLOR_BROWN         COLOR_RGB(0xA0, 0x60, 0x00)
#define COLOR_DARK_CYAN     COLOR_RGB(0x00, 0x80, 0x80)
#define COLOR_DARK_MAGENTA  COLOR_RGB(0x80, 0x00, 0x80)

/* BoxOS-flavour accents — unique names rather than ANSI clones. */
#define COLOR_BERRY         COLOR_RGB(0xE0, 0x4F, 0x90)
#define COLOR_OCEAN         COLOR_RGB(0x10, 0x80, 0xC0)
#define COLOR_LEAF          COLOR_RGB(0x60, 0xC0, 0x30)
#define COLOR_AMBER         COLOR_RGB(0xFF, 0xB0, 0x40)
#define COLOR_VIOLET        COLOR_RGB(0x90, 0x60, 0xFF)
#define COLOR_TEAL          COLOR_RGB(0x00, 0xB0, 0xA0)
#define COLOR_CORAL         COLOR_RGB(0xFF, 0x80, 0x70)
#define COLOR_SLATE         COLOR_RGB(0x60, 0x70, 0x80)

/* ------------------------------------------------------------------------- */
/* User-side RGB → 16-color VGA palette quantization.
 *
 * Used by boxlib when targeting VGA text mode (no GOP framebuffer). The
 * 16-color palette layout matches the IBM VGA hardware indexing so the
 * resulting 4-bit value goes straight into a VGA attribute byte. */

#define COLOR_VGA_BLACK         0x0
#define COLOR_VGA_BLUE          0x1
#define COLOR_VGA_GREEN         0x2
#define COLOR_VGA_CYAN          0x3
#define COLOR_VGA_RED           0x4
#define COLOR_VGA_MAGENTA       0x5
#define COLOR_VGA_BROWN         0x6
#define COLOR_VGA_LIGHT_GRAY    0x7
#define COLOR_VGA_DARK_GRAY     0x8
#define COLOR_VGA_LIGHT_BLUE    0x9
#define COLOR_VGA_LIGHT_GREEN   0xA
#define COLOR_VGA_LIGHT_CYAN    0xB
#define COLOR_VGA_LIGHT_RED     0xC
#define COLOR_VGA_LIGHT_MAGENTA 0xD
#define COLOR_VGA_YELLOW        0xE
#define COLOR_VGA_WHITE         0xF

/* Squared Euclidean distance in RGB cube — cheap and good enough for the
 * 16-entry palette. Static table mirrors the IBM VGA RGB triples. */
INLINE uint8_t color_to_vga4(Color c)
{
    if (c == COLOR_DEFAULT) return COLOR_VGA_LIGHT_GRAY;
    if (c == COLOR_INHERIT) return COLOR_VGA_LIGHT_GRAY;

    static const uint8_t pal_r[16] = {
        0x00, 0x00, 0x00, 0x00, 0xAA, 0xAA, 0xAA, 0xAA,
        0x55, 0x55, 0x55, 0x55, 0xFF, 0xFF, 0xFF, 0xFF
    };
    static const uint8_t pal_g[16] = {
        0x00, 0x00, 0xAA, 0xAA, 0x00, 0x00, 0x55, 0xAA,
        0x55, 0x55, 0xFF, 0xFF, 0x55, 0x55, 0xFF, 0xFF
    };
    static const uint8_t pal_b[16] = {
        0x00, 0xAA, 0x00, 0xAA, 0x00, 0xAA, 0x00, 0xAA,
        0x55, 0xFF, 0x55, 0xFF, 0x55, 0xFF, 0x55, 0xFF
    };

    int r = (int)COLOR_R(c);
    int g = (int)COLOR_G(c);
    int b = (int)COLOR_B(c);

    int best     = 0;
    int best_dist = 0x7FFFFFFF;
    for (int i = 0; i < 16; i++) {
        int dr = r - (int)pal_r[i];
        int dg = g - (int)pal_g[i];
        int db = b - (int)pal_b[i];
        int d  = dr * dr + dg * dg + db * db;
        if (d < best_dist) { best_dist = d; best = i; }
    }
    return (uint8_t)best;
}

/* Compose a VGA 8-bit attribute (high nibble = bg, low nibble = fg). */
INLINE uint8_t color_to_vga_attr(Color fg, Color bg)
{
    uint8_t f = color_to_vga4(fg) & 0x0F;
    uint8_t b = (bg == COLOR_INHERIT) ? COLOR_VGA_BLACK : (color_to_vga4(bg) & 0x0F);
    return (uint8_t)((b << 4) | f);
}

/* ------------------------------------------------------------------------- */
/* Color state — process-local current text colors. Used by print/println and
 * the printf output path when no inline %color override applies. */

void  set_color(Color fg);
Color get_color(void);
void  set_color_bg(Color bg);
Color get_color_bg(void);

#endif /* BOX_COLOR_H */
