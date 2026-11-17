/*
 * boxos_brush.h — the brush: pixels, for the hands that paint a picture
 * rather than cells, with no SSE and no memory of their own. Gangway paints
 * its scenes and its panel with it, and the display plays a scene on from the
 * very pixels the loader left (boxos_scene_play.h); the host stand holds both
 * to it (tools/scene_stand.c).
 *
 * A canvas is pixels in memory, 0x00RRGGBB, never the glass: video memory is
 * slow to read and a picture half made would show. What is made is shown
 * where it changed (BoxBrushShow), in the glass's layout.
 *
 * A shape (boxos_shape.h — outlines, strokes in boxos_stroke.h) is filled by
 * the share of each pixel it covers, with a paint (boxos_paint.h). Coverage,
 * alpha and opacity make one share, and the colour is laid over what is
 * beneath by it, mixed as light (BoxColorOver, boxos_color.h — the one way
 * the kernel, the loader, the display and Painter mix) — so text drawn here is
 * the very pixels the press makes. Painter's engine (painter/core) draws the
 * same shapes with the same coverage, in sixteen bits.
 *
 * The includer supplies uint8_t, uint16_t, uint32_t, int32_t, uint64_t,
 * int64_t and size_t.
 */
#ifndef BOXOS_BRUSH_H
#define BOXOS_BRUSH_H

#include "boxos_color.h"
#include "boxos_imprint.h"
#include "boxos_paint.h"
#include "boxos_pixel.h"
#include "boxos_shape.h"
#include "boxos_utf8.h"

/* ---------------------------------------------------------------------------
 * The canvas, and laying a colour on it
 * ------------------------------------------------------------------------- */

typedef struct BoxBrushCanvas {
    uint32_t    *Pixels;   /* 0x00RRGGBB, row after row */
    uint32_t     Width;    /* pixels */
    uint32_t     Height;
    uint32_t     Stride;   /* pixels from one row to the next */
    BoxRect Clip;     /* the only pixels drawn on */
    int32_t     *Row;      /* room for Width + 2 cells: the coverage of the row being filled */
} BoxBrushCanvas;

/* The pixels the canvas lets be drawn on: its clip, within it. */
static inline BoxRect BoxBrushWithin(const BoxBrushCanvas *K)
{
    const BoxRect Whole = { 0, 0, (int32_t)K->Width, (int32_t)K->Height };
    return BoxRectMeet(K->Clip, Whole);
}

/* Coverage, alpha and opacity, each of 255, made one share of 255. */
static inline uint32_t BoxBrushShare(uint32_t Coverage, uint32_t Alpha, uint32_t Opacity)
{
    return (Coverage * Alpha * Opacity + 32512u) / 65025u;
}

/* Rect, within the clip, made one colour — 0x00RRGGBB, whole. */
static inline void BoxBrushClear(const BoxBrushCanvas *K, BoxRect Rect, uint32_t Color)
{
    const BoxRect At = BoxRectMeet(Rect, BoxBrushWithin(K));
    for (int32_t Y = 0; Y < At.Height; Y++) {
        uint32_t *Line = K->Pixels + (size_t)(At.Y + Y) * K->Stride + At.X;
        for (int32_t X = 0; X < At.Width; X++) Line[X] = Color & 0x00FFFFFFu;
    }
}

/* One pixel: Color, 0xAARRGGBB, laid over it at Opacity. */
static inline void BoxBrushPixel(const BoxBrushCanvas *K, int32_t X, int32_t Y, uint32_t Color, uint32_t Opacity)
{
    const BoxRect At = BoxBrushWithin(K);
    if (X < At.X || Y < At.Y || X >= At.X + At.Width || Y >= At.Y + At.Height) return;
    const uint32_t Share = BoxBrushShare(BOX_SHAPE_FULL, Color >> 24, Opacity);
    uint32_t      *Pixel = K->Pixels + (size_t)Y * K->Stride + X;
    if (Share) *Pixel = BoxColorOver(*Pixel, Color, Share);
}

/* Rect of the canvas onto the glass whose first byte is Glass — as many
 * pixels as the canvas, Stride bytes from row to row, four a pixel, in the
 * layout Format (BOX_PIXEL_*). The glass is written, never read. */
static inline void BoxBrushShow(const BoxBrushCanvas *K, BoxRect Rect, volatile uint8_t *Glass, uint32_t Stride,
                                uint32_t Format)
{
    const BoxRect Whole = { 0, 0, (int32_t)K->Width, (int32_t)K->Height };
    const BoxRect At    = BoxRectMeet(Rect, Whole);
    for (int32_t Y = 0; Y < At.Height; Y++) {
        const uint32_t    *From = K->Pixels + (size_t)(At.Y + Y) * K->Stride + At.X;
        volatile uint32_t *To   = (volatile uint32_t *)(Glass + (size_t)(At.Y + Y) * Stride) + At.X;
        for (int32_t X = 0; X < At.Width; X++) To[X] = BoxPixelEncode(From[X], Format);
    }
}

/* ---------------------------------------------------------------------------
 * Filling
 * ------------------------------------------------------------------------- */

/* The shape (closed first) filled with the paint at Opacity of 255, within
 * the canvas's clip, swept row by row (BoxShapeSweep) in the canvas's Row.
 * 0, and nothing drawn, when the shape is refused or the paint does not fit. */
static inline int BoxBrushFill(const BoxBrushCanvas *K, BoxShape *S, const BoxPaint *P, uint32_t Opacity)
{
    BoxPaintReady Ready;
    BoxShapeClose(S);
    if (!BoxShapeDrawable(S) || !BoxPaintPrepare(P, &Ready)) return 0;
    if (Opacity == 0) return 1;

    const BoxRect Held = BoxShapeReach(S, BoxBrushWithin(K));
    if (Held.Width <= 0 || Held.Height <= 0) return 1;
    const int32_t Left  = Held.X, End = Held.X + Held.Width;
    const int     Plain = P->Kind == BOX_PAINT_SOLID && (P->Color >> 24) == BOX_SHAPE_FULL && Opacity >= BOX_SHAPE_FULL;
    BoxShapeSweep Sweep;
    BoxShapeSweepBegin(&Sweep, S, K->Row, Left, End);
    for (int32_t Y = Held.Y; Y < Held.Y + Held.Height; Y++) {
        BoxShapeSweepRow(&Sweep, Y);
        uint32_t *Line = K->Pixels + (size_t)Y * K->Stride;
        int64_t   Run  = K->Row[0];
        for (int32_t X = Left; X < End; X++) {
            Run += K->Row[1 + X - Left];
            const uint32_t Coverage = BoxShapeCoverage(Run, S->Rule);
            if (Coverage == 0) continue;
            if (Plain && Coverage == BOX_SHAPE_FULL) {
                Line[X] = P->Color & 0x00FFFFFFu;
                continue;
            }
            const uint32_t Color = BoxPaintAt(&Ready, X, Y);
            const uint32_t Share = BoxBrushShare(Coverage, Color >> 24, Opacity);
            if (Share) Line[X] = BoxColorOver(Line[X], Color, Share);
        }
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * Text
 * ------------------------------------------------------------------------- */
/* ---------------------------------------------------------------------------
 * Text
 * ------------------------------------------------------------------------- */

/* Text — Bytes of UTF-8 — in the imprint's size Size, its first cell's
 * top-left corner at pixel X, Y, in Color (0xAARRGGBB) at Opacity: what it
 * covers of each pixel laid as a shape's coverage is. A character the imprint
 * lacks is its empty box. The pixels it took across; none drawn, and 0, for a
 * size the imprint has not. */
static inline int32_t BoxBrushText(const BoxBrushCanvas *K, const BoxImprint *Imprint, uint32_t Size, int32_t X,
                                   int32_t Y, const char *Text, size_t Bytes, uint32_t Color, uint32_t Opacity)
{
    if (Size >= Imprint->SizeCount) return 0;
    const BoxImprintSize *Cell   = &Imprint->Sizes[Size];
    const BoxRect    Within = BoxBrushWithin(K);
    int64_t               At     = X;
    size_t                Read   = 0;
    while (Read < Bytes) {
        uint32_t Character;
        Read += BoxUtf8Next(Text + Read, Bytes - Read, &Character);
        const uint32_t Glyph = BoxImprintFind(Imprint, Character);
        for (uint32_t GY = 0; GY < Cell->Height; GY++) {
            const int64_t PY = (int64_t)Y + GY;
            if (PY < Within.Y || PY >= (int64_t)Within.Y + Within.Height) continue;
            uint32_t *Line = K->Pixels + (size_t)PY * K->Stride;
            for (uint32_t GX = 0; GX < Cell->Width; GX++) {
                const int64_t PX = At + GX;
                if (PX < Within.X || PX >= (int64_t)Within.X + Within.Width) continue;
                const uint32_t Inked = BoxImprintShade(Imprint, Size, Glyph, GX, GY);
                const uint32_t Share =
                    Inked ? BoxBrushShare(Inked * BOX_SHAPE_FULL / BOXOS_IMPRINT_INKED, Color >> 24, Opacity) : 0u;
                if (Share) Line[PX] = BoxColorOver(Line[PX], Color, Share);
            }
        }
        At += Cell->Width;
    }
    return (int32_t)(At - X);
}

#endif /* BOXOS_BRUSH_H */
