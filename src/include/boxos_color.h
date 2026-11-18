#ifndef BOXOS_COLOR_H
#define BOXOS_COLOR_H

/*
 * Shared kernel/userspace colour model — full 24-bit #RRGGBB end to end.
 *
 * BoxOS colour is metadata, never an in-band escape sequence. A colour
 * travels the whole path as a packed 0x00RRGGBB value: printf run → console
 * lane frame → a cell (box/items.h ItemCell) → the glass. A framebuffer
 * renders the value exactly; the VGA text page quantises to its 16-entry
 * hardware palette AT DRAW TIME — the last moment, in the one place that
 * actually has a 4-bit limitation.
 * Nothing upstream ever rounds. (The kernel's own lines carry no colour at
 * all, only a Tone — boxos_log_tone.h.)
 *
 * Sentinels live in the high byte, which a real RGB triple never uses:
 *   COLOR_DEFAULT  "use the role's default" — the screen's own: as text its
 *                  text colour, behind it its page colour, which the display
 *                  holds (light gray on black until the keeper of the programs
 *                  in front says others, box/display.h DISP_CMD_PAGE)
 *   COLOR_INHERIT  "keep what is set" (accepted as bg)
 *   COLOR_THROUGH  "see through to what is beneath here" — only in a layer's
 *                  cell (box/display.h): the display draws the colour of the
 *                  cell beneath the layer's, its background, in its place, and
 *                  a cell whose fg and bg are both COLOR_THROUGH shows the cell
 *                  beneath as it is. It is how a figure whose edge cuts a cell
 *                  leaves the rest of the cell to what lies under it
 *   top byte 0x80..0xBF  "this triple at an opacity over what is beneath" — a
 *                  layer's cell only, as COLOR_THROUGH: the low 24 bits are
 *                  #RRGGBB and the byte is 0x80 + n, n = 0..63 steps of
 *                  opacity (n/63). The display blends it with the background
 *                  of the cell beneath, as COLOR_THROUGH is resolved, into a
 *                  plain triple (BoxColorBlendOver), so no glass ever sees one.
 *                  n = 0 is COLOR_THROUGH, n = 63 the plain triple; the
 *                  helpers below make neither of them in this form
 * On the wire — a console frame, a layer's cell — a colour is a real triple or
 * COLOR_DEFAULT (BoxColorOnWire), and only the display, at the glass, turns
 * COLOR_DEFAULT into the colour it stands for.
 *
 * And colours are mixed here, each of the two ways once (BoxColorOver,
 * BoxColorBetween) — the kernel, the loader, the display, Painter and the
 * host's stands mix alike, so a colour is the same pixels whoever draws it.
 *
 * This is the SINGLE definition of the IBM VGA palette triples and of the
 * dominant-hue quantiser. The quantiser is exact on palette entries, so an
 * attribute converted to RGB and drawn on the VGA text backend produces
 * the identical attribute byte — kernel output is bit-for-bit what it was
 * when the console was 4-bit.
 *
 * Includer must provide uint*_t BEFORE including this header.
 *   Kernel:    #include "ktypes.h"
 *   Userspace: #include "box/types.h"
 */

/* ───── 24-bit value + sentinels ───── */

#define COLOR_RGB(r, g, b)  ((uint32_t)((((uint32_t)(r) & 0xFFu) << 16) | \
                                        (((uint32_t)(g) & 0xFFu) <<  8) | \
                                        ((uint32_t)(b) & 0xFFu)))

#define COLOR_R(c)  (((c) >> 16) & 0xFFu)
#define COLOR_G(c)  (((c) >>  8) & 0xFFu)
#define COLOR_B(c)  ((c) & 0xFFu)

#define COLOR_DEFAULT  ((uint32_t)0xFE000000u)
#define COLOR_INHERIT  ((uint32_t)0xFF000000u)
#define COLOR_THROUGH  ((uint32_t)0xFD000000u)

#define COLOR_TRANSLUCENT_BASE   ((uint32_t)0x80u)
#define COLOR_OPACITY_STEPS      ((uint32_t)63u)

/* ───── Mixing: the two ways, each once ─────
 *
 * A colour laid over another — a glyph's edge on its paper, a shape's soft
 * edge, a colour at an opacity — is mixed as LIGHT. An sRGB number is not the
 * light it shows: a pixel half covered by ink shows half the ink's light and
 * half the paper's, and half of each number shows darker than that — light
 * text on a dark page thin, dark text on a light one heavy. So each number is
 * turned into its light by sRGB's own curve (BoxColorLight), the lights mixed
 * by the share, and the light written back as the number nearest it
 * (BoxColorCode): BoxColorOver.
 *
 * A way from one colour to another — a gradient's run, a palette's steps — is
 * walked straight in the numbers, as drawing tools walk it, so a picture shows
 * the colours it was drawn in: BoxColorBetween.
 *
 * Whole numbers only, read from the curve's tables (boxos_color_light.h): the
 * kernel in a panic and the loader with no SSE mix as the display does. */

#include "boxos_color_light.h"

/* The light an sRGB number shows, 0..BOX_COLOR_LIGHT_MOST. */
static inline uint32_t BoxColorLight(uint32_t code)
{
    return BoxColorLightOf[code & 0xFFu];
}

/* The sRGB number nearest a light, 0..BOX_COLOR_LIGHT_MOST. */
static inline uint32_t BoxColorCode(uint32_t light)
{
    return BoxColorCodeOf[(light > BOX_COLOR_LIGHT_MOST ? BOX_COLOR_LIGHT_MOST : light) >> 4];
}

/* Color laid over Below at Share of 255, both their #RRGGBB, mixed as light:
 * 0 is Below, 255 is Color, each channel's light the way between rounded to
 * the nearest. */
static inline uint32_t BoxColorOver(uint32_t below, uint32_t color, uint32_t share)
{
    if (share == 0u) return below & 0x00FFFFFFu;
    if (share >= 255u || ((below ^ color) & 0x00FFFFFFu) == 0u) return color & 0x00FFFFFFu;
    uint32_t laid = 0;
    for (uint32_t shift = 0; shift <= 16u; shift += 8u) {
        const int32_t under = (int32_t)BoxColorLight(below >> shift);
        const int32_t over  = (int32_t)BoxColorLight(color >> shift);
        const int32_t part  = (over - under) * (int32_t)share;
        const int32_t light = under + (part >= 0 ? (part + 127) / 255 : -((-part + 127) / 255));
        laid |= BoxColorCode((uint32_t)light) << shift;
    }
    return laid;
}

/* Color laid at Share of 255 over Below, itself *Opacity of 255 opaque —
 * Porter-Duff "over", mixed as light: the colour seen, and in *Opacity how
 * opaque the two are together. Over a whole Below it is BoxColorOver; over
 * none it is Color at Share. */
static inline uint32_t BoxColorOverPart(uint32_t below, uint32_t color, uint32_t share, uint32_t *opacity)
{
    if (*opacity >= 255u) return BoxColorOver(below, color, share);
    if (share == 0u) return below & 0x00FFFFFFu;
    const uint32_t kept  = *opacity * (255u - share) / 255u;
    const uint32_t total = share + kept;
    uint32_t laid = 0;
    for (uint32_t shift = 0; shift <= 16u; shift += 8u) {
        const uint32_t light = (BoxColorLight(color >> shift) * share + BoxColorLight(below >> shift) * kept + total / 2u) / total;
        laid |= BoxColorCode(light) << shift;
    }
    *opacity = total;
    return laid;
}

/* A whole way, in the weights BoxColorBetween takes. */
#define BOX_COLOR_WAY 65536u

/* The way from A to B at Weight of BOX_COLOR_WAY, each of the four bytes —
 * 0xAARRGGBB, or #RRGGBB with a byte of 0 — walked straight, the step rounded
 * to the nearest, halves away from A. */
static inline uint32_t BoxColorBetween(uint32_t a, uint32_t b, uint32_t weight)
{
    const int32_t way = (int32_t)BOX_COLOR_WAY;
    uint32_t mixed = 0;
    for (uint32_t shift = 0; shift <= 24u; shift += 8u) {
        const int32_t from = (int32_t)((a >> shift) & 0xFFu), to = (int32_t)((b >> shift) & 0xFFu);
        const int32_t step = (to - from) * (int32_t)weight;
        const int32_t went = step >= 0 ? (step + way / 2) / way : -((-step + way / 2) / way);
        mixed |= (uint32_t)(from + went) << shift;
    }
    return mixed;
}

/* ───── A colour at an opacity ───── */

static inline int BoxColorIsTranslucent(uint32_t c)
{
    uint32_t top = c >> 24;
    return top >= COLOR_TRANSLUCENT_BASE && top <= COLOR_TRANSLUCENT_BASE + COLOR_OPACITY_STEPS;
}

/* How opaque a colour is, 0..COLOR_OPACITY_STEPS: a plain triple and any
 * sentinel but COLOR_THROUGH are whole, COLOR_THROUGH is none. */
static inline uint32_t BoxColorOpacity(uint32_t c)
{
    if (BoxColorIsTranslucent(c)) return (c >> 24) - COLOR_TRANSLUCENT_BASE;
    return c == COLOR_THROUGH ? 0u : COLOR_OPACITY_STEPS;
}

/* The triple at Steps of opacity: none is COLOR_THROUGH, whole the plain
 * triple, anything between the encoded colour. */
static inline uint32_t BoxColorTranslucent(uint32_t rgb, uint32_t steps)
{
    if (steps == 0u) return COLOR_THROUGH;
    if (steps >= COLOR_OPACITY_STEPS) return rgb & 0x00FFFFFFu;
    return ((COLOR_TRANSLUCENT_BASE + steps) << 24) | (rgb & 0x00FFFFFFu);
}

/* A translucent colour laid over a plain triple, as the plain triple that is
 * seen: the colour laid over it by its opacity (BoxColorOver). */
static inline uint32_t BoxColorBlendOver(uint32_t translucent, uint32_t beneath)
{
    const uint32_t share = (BoxColorOpacity(translucent) * 255u + COLOR_OPACITY_STEPS / 2u) / COLOR_OPACITY_STEPS;
    return BoxColorOver(beneath, translucent, share);
}

/* ───── IBM VGA 16-colour palette (#RRGGBB per index) ───── */

static inline uint32_t BoxVgaPaletteRgb(uint8_t idx)
{
    static const uint32_t pal[16] = {
        0x000000u, /* 0  BLACK         */
        0x0000AAu, /* 1  BLUE          */
        0x00AA00u, /* 2  GREEN         */
        0x00AAAAu, /* 3  CYAN          */
        0xAA0000u, /* 4  RED           */
        0xAA00AAu, /* 5  MAGENTA       */
        0xAA5500u, /* 6  BROWN         */
        0xAAAAAAu, /* 7  LIGHT_GRAY    */
        0x555555u, /* 8  DARK_GRAY     */
        0x5555FFu, /* 9  LIGHT_BLUE    */
        0x55FF55u, /* A  LIGHT_GREEN   */
        0x55FFFFu, /* B  LIGHT_CYAN    */
        0xFF5555u, /* C  LIGHT_RED     */
        0xFF55FFu, /* D  LIGHT_MAGENTA */
        0xFFFF55u, /* E  YELLOW        */
        0xFFFFFFu, /* F  WHITE         */
    };
    return pal[idx & 0x0Fu];
}

/* ───── A colour on the wire ───── */

/* A real triple as it is; any sentinel or translucent colour (any non-zero
 * high byte) as COLOR_DEFAULT — the display's to draw. A layer's cell is
 * not on this wire: it carries COLOR_THROUGH and translucent colours as they
 * are, and the display resolves them. */
static inline uint32_t BoxColorOnWire(uint32_t c)
{
    return (c >> 24) ? COLOR_DEFAULT : c;
}

/* ───── RGB → VGA palette index — by DOMINANT hue, exact on the palette ─────
 *
 * VGA text mode cannot show #RRGGBB, so it must degrade by the MEANING of
 * the colour, not by distance in the RGB cube: a dark navy must read as
 * BLUE, a dark purple as MAGENTA — nearest-distance sent both to black or
 * gray and the identity of the colour was lost (owner decision 2026-09-01;
 * UEFI/GOP renders exactly and is the priority target, VGA is a faithful
 * hue projection).
 *
 * Achromatic values (relative saturation under the palette's own floor)
 * pick a gray by lightness; chromatic ones classify into one of six hue
 * families (integer HSV, 256 units per sector, half-sector offset so the
 * boundaries land between palette hues) and pick the dark or light family
 * member by lightness. Every one of the 16 palette entries maps to its own
 * index — the round-trip is exact, so kernel output stays bit-identical. */
static inline uint8_t BoxColorToVga4(uint32_t rgb)
{
    uint32_t r = COLOR_R(rgb);
    uint32_t g = COLOR_G(rgb);
    uint32_t b = COLOR_B(rgb);
    uint32_t M = r > g ? r : g;  if (b > M) M = b;
    uint32_t m = r < g ? r : g;  if (b < m) m = b;
    uint32_t C = M - m;
    uint32_t L = (M + m) / 2;

    /* Achromatic: palette colours sit at relative saturation ≥170;
     * everything under 85 is a gray in this projection. */
    if (M == 0 || (C * 255u) / M < 85u) {
        if (L < 43u)  return 0x0;   /* BLACK      */
        if (L < 128u) return 0x8;   /* DARK_GRAY  */
        if (L < 213u) return 0x7;   /* LIGHT_GRAY */
        return 0xF;                 /* WHITE      */
    }

    int32_t h;
    if (M == r)      h = (int32_t)(256 * ((int32_t)g - (int32_t)b) / (int32_t)C);
    else if (M == g) h = 512 + (int32_t)(256 * ((int32_t)b - (int32_t)r) / (int32_t)C);
    else             h = 1024 + (int32_t)(256 * ((int32_t)r - (int32_t)g) / (int32_t)C);
    if (h < 0) h += 1536;
    uint32_t sector = (uint32_t)((h + 128) / 256) % 6u;  /* R Y G C B M */

    static const uint8_t dark_of [6] = { 0x4, 0x6, 0x2, 0x3, 0x1, 0x5 };
    static const uint8_t light_of[6] = { 0xC, 0xE, 0xA, 0xB, 0x9, 0xD };
    return L >= 128u ? light_of[sector] : dark_of[sector];
}

/* ───── VGA attribute byte ⇄ RGB pair ───── */

/* The attribute of a pair of real triples — a sentinel is the display's to
 * turn into one first. */
static inline uint8_t BoxColorPairToAttr(uint32_t fg, uint32_t bg)
{
    uint8_t f = BoxColorToVga4(fg);
    uint8_t b = BoxColorToVga4(bg);
    return (uint8_t)((b << 4) | f);
}

static inline uint32_t BoxAttrFgRgb(uint8_t attr)
{
    return BoxVgaPaletteRgb((uint8_t)(attr & 0x0Fu));
}

static inline uint32_t BoxAttrBgRgb(uint8_t attr)
{
    return BoxVgaPaletteRgb((uint8_t)((attr >> 4) & 0x0Fu));
}

#endif /* BOXOS_COLOR_H */
