/*
 * boxos_paint.h — paints: what colour a pixel of a shape is given. One colour,
 * a gradient — linear, radial (its focal point anywhere within its circle) or
 * conic, round its middle — or a picture, smooth between its pixels or sharp.
 * Where a pixel falls in a paint is worked out here once, in whole numbers,
 * for the brush (boxos_brush.h) and Painter's engine (painter/core) alike;
 * each then gives that place its colour by the one rule (boxos_color.h): a
 * gradient's colours walked straight in their numbers, a picture's pixels
 * mixed as light.
 *
 * A gradient's own space runs from its first stop at 0 to its last at
 * BOX_PAINT_GRADIENT: along x (LINEAR), out from its middle to that radius
 * (RADIAL), or round its middle from +x toward +y (CONIC). Past its ends it
 * holds its end colours (PAD), begins again (REPEAT), runs back (REFLECT) or
 * is clear (NONE). A picture's pixel I, J covers places I·ONE to (I+1)·ONE of
 * its space across and J·ONE to (J+1)·ONE down; past its edge it holds its
 * edge pixels, repeats, mirrors or is clear the same way.
 *
 * The includer supplies what boxos_shape.h and boxos_color.h ask for.
 */
#ifndef BOXOS_PAINT_H
#define BOXOS_PAINT_H

#include "boxos_color.h"
#include "boxos_shape.h"

#define BOX_PAINT_SOLID   0u
#define BOX_PAINT_LINEAR  1u
#define BOX_PAINT_RADIAL  2u
#define BOX_PAINT_PICTURE 3u
#define BOX_PAINT_CONIC   4u

#define BOX_PAINT_PAD     0u   /* the end colours, or edge pixels, held on */
#define BOX_PAINT_REPEAT  1u   /* begun again */
#define BOX_PAINT_REFLECT 2u   /* run back, and on again */
#define BOX_PAINT_NONE    3u   /* clear */

/* How far a gradient runs in places of its own space; a stop's place and a
 * conic gradient's whole turn. */
#define BOX_PAINT_GRADIENT BOX_SHAPE_UNIT

/* A gradient's colour, 0xAARRGGBB, from At (0 to GRADIENT) of its way on. */
typedef struct BoxPaintStop {
    uint32_t At;
    uint32_t Color;
} BoxPaintStop;

/*
 * A paint. SOLID: Color, 0xAARRGGBB. LINEAR, RADIAL and CONIC: StopCount
 * stops, At rising, at least one; RADIAL's circle drawn from FocalX, FocalY of
 * its space — 0, 0 its middle — within the circle. PICTURE: PictureWidth x
 * PictureHeight pixels, 0xAARRGGBB, PictureStride pixels from row to row,
 * smooth between the middles of its pixels or, Sharp, each pixel square. Space
 * lays the paint's own space on the canvas; Extend says what lies past its
 * ends.
 */
typedef struct BoxPaint {
    uint32_t            Kind;
    uint32_t            Color;
    const BoxPaintStop *Stops;
    uint32_t            StopCount;
    BoxMatrix           Space;
    const uint32_t     *Picture;
    uint32_t            PictureWidth;
    uint32_t            PictureHeight;
    uint32_t            PictureStride;
    uint32_t            Extend;
    uint32_t            Sharp;
    int32_t             FocalX, FocalY;
} BoxPaint;

/* A paint made ready for a fill: the canvas onto its space — u = (IA·dx +
 * IC·dy) / UNIT, v = (IB·dx + ID·dy) / UNIT, dx and dy a pixel's middle less
 * the space's 0,0. */
typedef struct BoxPaintReady {
    const BoxPaint *Paint;
    int64_t         IA, IB, IC, ID;
} BoxPaintReady;

/* 0 when the paint is of no kind, its space does not fit, folds flat or
 * shrinks past what is undone, its focal point lies on or past its circle, or
 * it has nothing to show. */
static inline int BoxPaintPrepare(const BoxPaint *P, BoxPaintReady *Ready)
{
    Ready->Paint = P;
    Ready->IA = Ready->IB = Ready->IC = Ready->ID = 0;
    if (P->Kind == BOX_PAINT_SOLID) return 1;
    if (P->Kind > BOX_PAINT_CONIC || P->Extend > BOX_PAINT_NONE) return 0;
    if (P->Kind == BOX_PAINT_PICTURE ? !P->Picture || !P->PictureWidth || !P->PictureHeight ||
                                           P->PictureStride < P->PictureWidth
                                     : !P->Stops || !P->StopCount)
        return 0;
    if (P->Kind == BOX_PAINT_RADIAL) {
        const int64_t G = BOX_PAINT_GRADIENT;
        if (BoxShapeSize(P->FocalX) >= G || BoxShapeSize(P->FocalY) >= G ||
            (int64_t)P->FocalX * P->FocalX + (int64_t)P->FocalY * P->FocalY >= G * G)
            return 0;
    }
    const BoxMatrix *M = &P->Space;
    if (!BoxMatrixFits(M)) return 0;
    const int64_t Folds = (int64_t)M->A * M->D - (int64_t)M->B * M->C;
    if (Folds == 0) return 0;
    Ready->IA = ((int64_t)M->D * 4294967296ll) / Folds;
    Ready->IB = ((int64_t)-M->B * 4294967296ll) / Folds;
    Ready->IC = ((int64_t)-M->C * 4294967296ll) / Folds;
    Ready->ID = ((int64_t)M->A * 4294967296ll) / Folds;
    const int64_t Most = 2147483647ll;
    return BoxShapeSize(Ready->IA) <= Most && BoxShapeSize(Ready->IB) <= Most && BoxShapeSize(Ready->IC) <= Most &&
           BoxShapeSize(Ready->ID) <= Most;
}

/* Where pixel X, Y's middle lies in the paint's space, in places. */
static inline void BoxPaintPlace(const BoxPaintReady *R, int32_t X, int32_t Y, int64_t *U, int64_t *V)
{
    const BoxPaint *P  = R->Paint;
    const int64_t   DX = (int64_t)X * BOX_SHAPE_ONE + BOX_SHAPE_ONE / 2 - P->Space.E;
    const int64_t   DY = (int64_t)Y * BOX_SHAPE_ONE + BOX_SHAPE_ONE / 2 - P->Space.F;
    *U                 = BoxShapeRound(R->IA * DX + R->IC * DY);
    *V                 = BoxShapeRound(R->IB * DX + R->ID * DY);
}

/* A way T past a gradient's ends brought within them as Extend says: 0 when
 * it is clear there. */
static inline int BoxPaintExtendWay(uint32_t Extend, int64_t T, uint32_t *Way)
{
    const int64_t G = BOX_PAINT_GRADIENT;
    if (Extend == BOX_PAINT_REPEAT) {
        T %= G;
        if (T < 0) T += G;
    } else if (Extend == BOX_PAINT_REFLECT) {
        T %= 2 * G;
        if (T < 0) T += 2 * G;
        if (T > G) T = 2 * G - T;
    } else if (Extend == BOX_PAINT_NONE) {
        if (T < 0 || T > G) return 0;
    } else {
        T = BoxShapeLeast(BoxShapeMost(T, 0), G);
    }
    *Way = (uint32_t)T;
    return 1;
}

/* How far out a radial gradient's place U, V lies, in places of its way: the
 * circle through it drawn from the focal point toward the middle as the way
 * grows. Past any way a screen shows, the very far. */
static inline int64_t BoxPaintRadialWay(const BoxPaint *P, int64_t U, int64_t V)
{
    const int64_t G = BOX_PAINT_GRADIENT, Far = (int64_t)1 << 40;
    if (P->FocalX == 0 && P->FocalY == 0) {
        if (BoxShapeSize(U) >= (1ll << 31) || BoxShapeSize(V) >= (1ll << 31)) return Far;
        const uint64_t Square = (uint64_t)(U * U + V * V);
        return Square < (1ull << 32) ? (int64_t)BoxShapeRoot((uint32_t)Square) : (int64_t)BoxShapeRoot64(Square);
    }
    /* The way t solves |Q + t·F| = t·G, Q the place less the focal point F:
     * t = (Q·F + √((Q·F)² + (G² − F²)·Q²)) / (G² − F²) — worked with every
     * length shifted down until Q is within 2^18, so no square spills. */
    int64_t QX = U - P->FocalX, QY = V - P->FocalY;
    if (BoxShapeSize(QX) >= (1ll << 40) || BoxShapeSize(QY) >= (1ll << 40)) return Far;
    uint32_t Shift = 4u;
    while ((BoxShapeMost(BoxShapeSize(QX), BoxShapeSize(QY)) >> Shift) > (1ll << 18)) Shift++;
    const int64_t Down = (int64_t)1 << Shift;
    const int64_t QXs = BoxShapeRoundDiv(QX, Down), QYs = BoxShapeRoundDiv(QY, Down);
    const int64_t FXs = BoxShapeRoundDiv(P->FocalX, Down), FYs = BoxShapeRoundDiv(P->FocalY, Down);
    const int64_t Rs  = G >> Shift;
    const int64_t Room = Rs * Rs - (FXs * FXs + FYs * FYs);
    if (Room <= 0) return Far;
    const int64_t  Towards = QXs * FXs + QYs * FYs;
    const uint64_t Under   = (uint64_t)(Towards * Towards) + (uint64_t)Room * (uint64_t)(QXs * QXs + QYs * QYs);
    const int64_t  Top     = Towards + (int64_t)BoxShapeRoot64(Under);
    return Top <= 0 ? 0 : Top * G / Room;
}

/* A gradient's way at pixel X, Y, within its ends: 0 when it is clear
 * there. */
static inline int BoxPaintWay(const BoxPaintReady *R, int32_t X, int32_t Y, uint32_t *Way)
{
    const BoxPaint *P = R->Paint;
    int64_t         U, V;
    BoxPaintPlace(R, X, Y, &U, &V);
    if (P->Kind == BOX_PAINT_CONIC) {
        *Way = BoxShapeTurnOf(U, V);
        return 1;
    }
    return BoxPaintExtendWay(P->Extend, P->Kind == BOX_PAINT_LINEAR ? U : BoxPaintRadialWay(P, U, V), Way);
}

/* The gradient's colour, 0xAARRGGBB, At (0 to GRADIENT) of its way: between
 * two stops walked straight (BoxColorBetween), the first before the first
 * stop and the last past the last. */
static inline uint32_t BoxPaintAlong(const BoxPaint *P, uint32_t At)
{
    const BoxPaintStop *S = P->Stops;
    if (At <= S[0].At) return S[0].Color;
    for (uint32_t K = 1; K < P->StopCount; K++) {
        if (At > S[K].At) continue;
        const uint32_t Span = S[K].At - S[K - 1u].At;
        if (Span == 0) return S[K].Color;
        return BoxColorBetween(S[K - 1u].Color, S[K].Color, (uint32_t)(((uint64_t)(At - S[K - 1u].At) * BOX_COLOR_WAY) / Span));
    }
    return S[P->StopCount - 1u].Color;
}

/* Where the picture's pixel I, J is, past its edge brought within it as
 * Extend says: 0 when it is clear there. */
static inline int BoxPaintTexel(const BoxPaint *P, int64_t I, int64_t J, size_t *Index)
{
    const int64_t W = P->PictureWidth, H = P->PictureHeight;
    if (P->Extend == BOX_PAINT_NONE) {
        if (I < 0 || J < 0 || I >= W || J >= H) return 0;
    } else if (P->Extend == BOX_PAINT_REPEAT) {
        I %= W;
        J %= H;
        if (I < 0) I += W;
        if (J < 0) J += H;
    } else if (P->Extend == BOX_PAINT_REFLECT) {
        I %= 2 * W;
        J %= 2 * H;
        if (I < 0) I += 2 * W;
        if (J < 0) J += 2 * H;
        if (I >= W) I = 2 * W - 1 - I;
        if (J >= H) J = 2 * H - 1 - J;
    } else {
        I = BoxShapeLeast(BoxShapeMost(I, 0), W - 1);
        J = BoxShapeLeast(BoxShapeMost(J, 0), H - 1);
    }
    *Index = (size_t)J * P->PictureStride + (size_t)I;
    return 1;
}

/* The picture's places U, V seen through: the pixel I, J nearest below the
 * middles round them, and how far past that middle they lie, of ONE. */
static inline void BoxPaintBetween(int64_t U, int64_t V, int64_t *I, int64_t *J, uint32_t *FX, uint32_t *FY)
{
    U -= BOX_SHAPE_ONE / 2;
    V -= BOX_SHAPE_ONE / 2;
    *I  = BoxShapePixelOf(U);
    *J  = BoxShapePixelOf(V);
    *FX = (uint32_t)(U - *I * BOX_SHAPE_ONE);
    *FY = (uint32_t)(V - *J * BOX_SHAPE_ONE);
}

/* The picture at U, V of its space, 0xAARRGGBB. Sharp: the pixel there.
 * Smooth: the four pixels whose middles lie round it, weighed by nearness and
 * each by its alpha, mixed as light — so a clear pixel lends no colour to the
 * edge of one beside it, and a fine pattern seen from far is as bright as it
 * was. */
static inline uint32_t BoxPaintSample(const BoxPaint *P, int64_t U, int64_t V)
{
    size_t Index;
    if (P->Sharp)
        return BoxPaintTexel(P, BoxShapePixelOf(U), BoxShapePixelOf(V), &Index) ? P->Picture[Index] : 0u;
    int64_t  I, J;
    uint32_t FX, FY;
    BoxPaintBetween(U, V, &I, &J, &FX, &FY);
    const uint32_t Weights[4] = { (256u - FX) * (256u - FY), FX * (256u - FY), (256u - FX) * FY, FX * FY };
    uint64_t       Alpha = 0, Light[3] = { 0, 0, 0 };
    for (uint32_t K = 0; K < 4u; K++) {
        if (!Weights[K] || !BoxPaintTexel(P, I + (K & 1u), J + (K >> 1), &Index)) continue;
        const uint32_t Texel = P->Picture[Index];
        const uint64_t Held  = (uint64_t)Weights[K] * (Texel >> 24);
        Alpha += Held;
        for (uint32_t C = 0; C < 3u; C++) Light[C] += Held * BoxColorLight(Texel >> (8u * C));
    }
    if (Alpha == 0) return 0;
    uint32_t Color = (uint32_t)((Alpha + 32768u) >> 16) << 24;
    for (uint32_t C = 0; C < 3u; C++) Color |= BoxColorCode((uint32_t)((Light[C] + Alpha / 2u) / Alpha)) << (8u * C);
    return Color;
}

/* The paint's colour, 0xAARRGGBB, at pixel X, Y's middle. */
static inline uint32_t BoxPaintAt(const BoxPaintReady *R, int32_t X, int32_t Y)
{
    const BoxPaint *P = R->Paint;
    if (P->Kind == BOX_PAINT_SOLID) return P->Color;
    if (P->Kind == BOX_PAINT_PICTURE) {
        int64_t U, V;
        BoxPaintPlace(R, X, Y, &U, &V);
        return BoxPaintSample(P, U, V);
    }
    uint32_t Way;
    return BoxPaintWay(R, X, Y, &Way) ? BoxPaintAlong(P, Way) : 0u;
}

#endif /* BOXOS_PAINT_H */
