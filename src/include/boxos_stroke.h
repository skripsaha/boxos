/*
 * boxos_stroke.h — a stroke: the line drawn along a path, a band Width wide
 * about it, its ends cut short (CAP_BUTT), rounded (CAP_ROUND) or squared
 * (CAP_SQUARE), its corners joined sharp (JOIN_MITER — cut to a bevel past
 * the miter limit), round (JOIN_ROUND) or cut (JOIN_BEVEL), solid or dashed.
 * SVG's stroke, in the brush's whole numbers, for every hand boxos_shape.h
 * draws for.
 *
 * A stroke is itself a shape: its pieces — a band of four sides along each
 * edge of the path, a wedge at each corner, a cap at each end — are added to
 * one, every piece wound the way the ready shapes are, and the shape filled by
 * NONZERO, so where pieces overlap the line is laid once, and where two pieces
 * meet along a side they share, the two edges cancel exactly and leave no
 * seam. A stroke's shape is never filled by EVENODD, which would cut its
 * overlaps out.
 *
 * The stroke is drawn in the shape's own space — the width, the dashes and the
 * miter limit measured there — and lands through the shape's matrix, so a line
 * stretched one way is wider that way, as a pen held so would draw it. A curve
 * is broken into edges in its own space, fine enough to stray less than FLAT
 * where it lands, and their corners joined round, as the curve's own band is.
 *
 * Nothing is kept of the path but the place before: a stroke is drawn as its
 * path is told, edge by edge, and asks for no room of its own.
 *
 * The includer supplies what boxos_shape.h asks for.
 */
#ifndef BOXOS_STROKE_H
#define BOXOS_STROKE_H

#include "boxos_shape.h"

#define BOX_STROKE_CAP_BUTT    0u   /* the line ends where its path ends */
#define BOX_STROKE_CAP_ROUND   1u   /* half a circle past the end */
#define BOX_STROKE_CAP_SQUARE  2u   /* half the width past the end, square */

#define BOX_STROKE_JOIN_MITER  0u   /* the two sides carried on until they meet */
#define BOX_STROKE_JOIN_ROUND  1u   /* a circle's arc about the corner */
#define BOX_STROKE_JOIN_BEVEL  2u   /* the two sides' ends joined straight */

/* The longest a miter may be, in widths, in 1/256: SVG's own 4 unless told
 * otherwise. */
#define BOX_STROKE_MITER_LIMIT (4u * 256u)

/* The most lengths a dash pattern has. */
#define BOX_STROKE_DASHES_MOST 64u

typedef struct BoxStroke {
    BoxShape      *Shape;
    int64_t        Half;            /* half the width, own places */
    uint32_t       Cap, Join;
    uint32_t       MiterLimit;      /* in widths, 1/256; 1 or more */
    uint32_t       RoundSteps;      /* the edges a whole circle of Half takes where it lands */
    const int32_t *Dashes;          /* on, off, on… in own places; none: solid */
    uint32_t       DashCount;
    uint32_t       DashPeriod;      /* the lengths a round of the pattern takes — twice an odd count */
    int64_t        DashOffset;
    /* The contour being drawn. */
    int            Open;
    int            Fresh;           /* nothing of it drawn yet */
    int            Drawn;           /* an edge of some length drawn */
    int64_t        StartX, StartY, AtX, AtY;
    /* The dash the pen is in. */
    uint32_t       DashIndex;
    int64_t        DashLeft;
    int            DashOn;
    int            StartOn;         /* the dash was on where the contour began */
    /* The piece being drawn: the line between two gaps. */
    int            Piece;
    int            Bent;            /* it has an edge; LastX, LastY its way */
    int64_t        LastX, LastY;
    /* The contour's first piece, begun where the contour began: its cap waits,
     * for a closed contour joins it to its last instead. */
    int            FirstWaits;
    int64_t        FirstX, FirstY, FirstWayX, FirstWayY;
} BoxStroke;

/* ---------------------------------------------------------------------------
 * Pieces
 * ------------------------------------------------------------------------- */

static inline void BoxStrokeTo(BoxStroke *K, int64_t X, int64_t Y, int First)
{
    if (First) BoxShapeMoveTo(K->Shape, BoxShapeNarrow(X), BoxShapeNarrow(Y));
    else       BoxShapeLineTo(K->Shape, BoxShapeNarrow(X), BoxShapeNarrow(Y));
}

/* WayX, WayY made Half long, to the nearest place. */
static inline void BoxStrokeAlong(const BoxStroke *K, int64_t WayX, int64_t WayY, int64_t *AX, int64_t *AY)
{
    const int64_t Length = BoxShapeApart(WayX, WayY);
    *AX = (2 * WayX * K->Half + (WayX < 0 ? -Length : Length)) / (2 * Length);
    *AY = (2 * WayY * K->Half + (WayY < 0 ? -Length : Length)) / (2 * Length);
}

/* The band along the edge from A to B whose way is WayX, WayY: its two sides
 * Half to either hand. */
static inline void BoxStrokeBand(BoxStroke *K, int64_t AX, int64_t AY, int64_t BX, int64_t BY, int64_t WayX,
                                 int64_t WayY)
{
    int64_t UX, UY;
    BoxStrokeAlong(K, WayX, WayY, &UX, &UY);
    const int64_t NX = -UY, NY = UX;
    BoxStrokeTo(K, AX - NX, AY - NY, 1);
    BoxStrokeTo(K, BX - NX, BY - NY, 0);
    BoxStrokeTo(K, BX + NX, BY + NY, 0);
    BoxStrokeTo(K, AX + NX, AY + NY, 0);
    BoxShapeClose(K->Shape);
}

/*
 * A fan about P: from P + V1 round by Sweep turns (negative the other way) to
 * P + V2 along a circle of Half, Steps edges, through Middle (a place, or none)
 * when Steps is 0 — a bevel's or a miter's wedge, a round join, a round cap.
 * Wound as the ready shapes are, whichever way it sweeps.
 */
static inline void BoxStrokeFan(BoxStroke *K, int64_t PX, int64_t PY, int64_t V1X, int64_t V1Y, int64_t V2X,
                                int64_t V2Y, int32_t Sweep, uint32_t Steps, const int64_t *Middle)
{
    const uint32_t From   = BoxShapeTurnOf(V1X, V1Y);
    const int      Ahead  = Sweep >= 0;
    BoxStrokeTo(K, PX, PY, 1);
    BoxStrokeTo(K, PX + (Ahead ? V1X : V2X), PY + (Ahead ? V1Y : V2Y), 0);
    if (Middle) BoxStrokeTo(K, Middle[0], Middle[1], 0);
    for (uint32_t I = 1; I < Steps; I++) {
        const uint32_t J    = Ahead ? I : Steps - I;
        const uint32_t Turn = From + (uint32_t)(int32_t)((int64_t)Sweep * J / (int64_t)Steps);
        BoxStrokeTo(K, PX + BoxShapeRound(K->Half * BoxShapeCosine(Turn)),
                    PY + BoxShapeRound(K->Half * BoxShapeSine(Turn)), 0);
    }
    BoxStrokeTo(K, PX + (Ahead ? V2X : V1X), PY + (Ahead ? V2Y : V1Y), 0);
    BoxShapeClose(K->Shape);
}

/* The edges a round piece sweeping Sweep turns takes: never fewer than one. */
static inline uint32_t BoxStrokeArcSteps(const BoxStroke *K, int32_t Sweep)
{
    const uint64_t Steps = ((uint64_t)K->RoundSteps * (uint64_t)BoxShapeSize(Sweep) + BOX_SHAPE_TURN - 1u) / BOX_SHAPE_TURN;
    return Steps < 1u ? 1u : (uint32_t)Steps;
}

/* The cap at P of a line whose way out is WayX, WayY. */
static inline void BoxStrokeCap(BoxStroke *K, int64_t PX, int64_t PY, int64_t WayX, int64_t WayY)
{
    int64_t AX, AY;
    if (K->Cap == BOX_STROKE_CAP_BUTT) return;
    BoxStrokeAlong(K, WayX, WayY, &AX, &AY);
    if (K->Cap == BOX_STROKE_CAP_SQUARE) {
        BoxStrokeBand(K, PX, PY, PX + AX, PY + AY, WayX, WayY);
        return;
    }
    /* From the left hand round through the way out to the right: half a turn
     * back from the left hand's own. */
    const int32_t Sweep = -(int32_t)(BOX_SHAPE_TURN / 2u);
    BoxStrokeFan(K, PX, PY, -AY, AX, AY, -AX, Sweep, BoxStrokeArcSteps(K, Sweep), (const int64_t *)0);
}

/* A line of no length at P: the cap twice over, the way it would have run. */
static inline void BoxStrokeDot(BoxStroke *K, int64_t PX, int64_t PY, int64_t WayX, int64_t WayY)
{
    if (K->Cap == BOX_STROKE_CAP_ROUND) {
        BoxShapeAddEllipse(K->Shape, BoxShapeNarrow(PX), BoxShapeNarrow(PY), (int32_t)K->Half, (int32_t)K->Half, 0);
        return;
    }
    BoxStrokeCap(K, PX, PY, WayX, WayY);
    BoxStrokeCap(K, PX, PY, -WayX, -WayY);
}

/* The corner at P where a line of way In turns to way Out, joined as Join
 * says. */
static inline void BoxStrokeCorner(BoxStroke *K, int64_t PX, int64_t PY, int64_t InX, int64_t InY, int64_t OutX,
                                   int64_t OutY, uint32_t Join)
{
    const int64_t Cross = InX * OutY - InY * OutX, Dot = InX * OutX + InY * OutY;
    if (Cross == 0 && Dot > 0) return;
    int64_t A1X, A1Y, A2X, A2Y;
    BoxStrokeAlong(K, InX, InY, &A1X, &A1Y);
    BoxStrokeAlong(K, OutX, OutY, &A2X, &A2Y);
    /* The side the line turns away from, where the corner shows. */
    const int64_t Side = Cross > 0 ? -1 : 1;
    const int64_t V1X = -A1Y * Side, V1Y = A1X * Side, V2X = -A2Y * Side, V2Y = A2X * Side;
    if (Join == BOX_STROKE_JOIN_ROUND) {
        int32_t Sweep = (int32_t)(int16_t)(uint16_t)(BoxShapeTurnOf(V2X, V2Y) - BoxShapeTurnOf(V1X, V1Y));
        /* Turned right back the sweep is half a turn either way: the one
         * round the way the line came in. */
        if (Cross == 0) Sweep = -(int32_t)(BOX_SHAPE_TURN / 2u) * (int32_t)Side;
        BoxStrokeFan(K, PX, PY, V1X, V1Y, V2X, V2Y, Sweep, BoxStrokeArcSteps(K, Sweep), (const int64_t *)0);
        return;
    }
    /* The sides turn with the line: from V1 to V2 the way it turns. */
    const int32_t Sweep = Cross > 0 ? 1 : -1;
    if (Join == BOX_STROKE_JOIN_MITER && Cross != 0) {
        /* The two ways, 2^14 long: their cosine in 2^28. Within the limit
         * when limit² · (1 + cos) ≥ 2, the miter's tip (V1 + V2) / (1 + cos)
         * from the corner. */
        int64_t U1X, U1Y, U2X, U2Y;
        const int64_t L1 = BoxShapeApart(InX, InY), L2 = BoxShapeApart(OutX, OutY);
        U1X = BoxShapeRoundDiv(InX << 14, L1);
        U1Y = BoxShapeRoundDiv(InY << 14, L1);
        U2X = BoxShapeRoundDiv(OutX << 14, L2);
        U2Y = BoxShapeRoundDiv(OutY << 14, L2);
        const int64_t Bow   = (1ll << 28) + U1X * U2X + U1Y * U2Y;
        const int64_t Limit = K->MiterLimit;
        if (Bow > 0 && Limit * Limit * Bow >= (1ll << 45)) {
            const int64_t Tip[2] = { PX + BoxShapeRoundDiv((V1X + V2X) << 28, Bow),
                                     PY + BoxShapeRoundDiv((V1Y + V2Y) << 28, Bow) };
            BoxStrokeFan(K, PX, PY, V1X, V1Y, V2X, V2Y, Sweep, 0u, Tip);
            return;
        }
    }
    BoxStrokeFan(K, PX, PY, V1X, V1Y, V2X, V2Y, Sweep, 0u, (const int64_t *)0);
}

/* ---------------------------------------------------------------------------
 * The run of the line
 * ------------------------------------------------------------------------- */

/* A piece begun at P, its way WayX, WayY: its cap drawn, or kept waiting when
 * it begins where the contour began. */
static inline void BoxStrokeOpenPiece(BoxStroke *K, int64_t PX, int64_t PY, int64_t WayX, int64_t WayY)
{
    K->Piece = 1;
    K->Bent  = 0;
    if (K->Fresh) {
        K->FirstWaits = 1;
        K->FirstX     = PX;
        K->FirstY     = PY;
        K->FirstWayX  = WayX;
        K->FirstWayY  = WayY;
    } else {
        BoxStrokeCap(K, PX, PY, -WayX, -WayY);
    }
}

/* The piece ended at P: its cap, the way it ran last — or WayX, WayY when it
 * has no edge. */
static inline void BoxStrokeEndPiece(BoxStroke *K, int64_t PX, int64_t PY, int64_t WayX, int64_t WayY)
{
    if (!K->Piece) return;
    if (K->Bent) BoxStrokeCap(K, PX, PY, K->LastX, K->LastY);
    else         BoxStrokeCap(K, PX, PY, WayX, WayY);
    K->Piece = 0;
    K->Bent  = 0;
}

/* The pattern's length Index of a round of it. */
static inline int64_t BoxStrokeDashLength(const BoxStroke *K, uint32_t Index)
{
    return K->Dashes[Index % K->DashCount];
}

/* The dash at P ended: on to the next with any length — a dash of none drawn
 * as a dot, the way the line runs. */
static inline void BoxStrokeNextDash(BoxStroke *K, int64_t PX, int64_t PY, int64_t WayX, int64_t WayY)
{
    do {
        if (K->DashOn) BoxStrokeEndPiece(K, PX, PY, WayX, WayY);
        K->DashIndex = (K->DashIndex + 1u) % K->DashPeriod;
        K->DashLeft  = BoxStrokeDashLength(K, K->DashIndex);
        K->DashOn    = (K->DashIndex & 1u) == 0;
        if (K->DashOn && K->DashLeft == 0) {
            BoxStrokeOpenPiece(K, PX, PY, WayX, WayY);
            BoxStrokeEndPiece(K, PX, PY, WayX, WayY);
        }
    } while (K->DashLeft == 0);
}

/* The line's edge from A to B, joined to the one before it as Join says —
 * round within a curve. */
static inline void BoxStrokeEdge(BoxStroke *K, int64_t AX, int64_t AY, int64_t BX, int64_t BY, uint32_t Join)
{
    const int64_t WayX = BX - AX, WayY = BY - AY;
    const int64_t Length = BoxShapeApart(WayX, WayY);
    if (Length == 0 || K->Half == 0) return;
    int64_t Done = 0;
    while (Done < Length) {
        const int64_t Step = K->Dashes ? BoxShapeLeast(K->DashLeft, Length - Done) : Length;
        const int64_t FromX = AX + BoxShapeRoundDiv(WayX * Done, Length), FromY = AY + BoxShapeRoundDiv(WayY * Done, Length);
        const int64_t ToX = AX + BoxShapeRoundDiv(WayX * (Done + Step), Length);
        const int64_t ToY = AY + BoxShapeRoundDiv(WayY * (Done + Step), Length);
        if (!K->Dashes || K->DashOn) {
            if (!K->Piece) BoxStrokeOpenPiece(K, FromX, FromY, WayX, WayY);
            else if (K->Bent) BoxStrokeCorner(K, FromX, FromY, K->LastX, K->LastY, WayX, WayY, Join);
            BoxStrokeBand(K, FromX, FromY, ToX, ToY, WayX, WayY);
            K->Bent  = 1;
            K->LastX = WayX;
            K->LastY = WayY;
        }
        K->Fresh = 0;
        K->Drawn = 1;
        Done += Step;
        if (K->Dashes) {
            K->DashLeft -= Step;
            if (K->DashLeft == 0) BoxStrokeNextDash(K, ToX, ToY, WayX, WayY);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Telling the path
 * ------------------------------------------------------------------------- */

/* A stroke Width wide (own places) into Shape, which keeps NONZERO; the miter
 * limit SVG's own, no dashes. */
static inline void BoxStrokeBegin(BoxStroke *K, BoxShape *Shape, int32_t Width, uint32_t Cap, uint32_t Join)
{
    K->Shape      = Shape;
    K->Half       = BoxShapeMost(Width, 0) / 2;
    K->Cap        = Cap <= BOX_STROKE_CAP_SQUARE ? Cap : BOX_STROKE_CAP_BUTT;
    K->Join       = Join <= BOX_STROKE_JOIN_BEVEL ? Join : BOX_STROKE_JOIN_MITER;
    K->MiterLimit = BOX_STROKE_MITER_LIMIT;
    K->RoundSteps = BoxShapeRoundSteps(BoxShapeOnCanvas(Shape, K->Half));
    K->Dashes     = (const int32_t *)0;
    K->DashCount  = K->DashPeriod = 0;
    K->DashOffset = 0;
    K->Open       = 0;
    K->Piece      = 0;
    K->FirstWaits = 0;
    if (BoxShapeOwn(Shape, &Width, 1)) Shape->Rule = BOX_SHAPE_NONZERO;
}

/* The longest a miter may be before it is cut to a bevel, in widths, in
 * 1/256 — never under one width. */
static inline void BoxStrokeMiterLimit(BoxStroke *K, uint32_t Limit)
{
    K->MiterLimit = Limit < 256u ? 256u : Limit > 65535u ? 65535u : Limit;
}

/* Dashes: Count lengths, on, off, on… in own places — an odd count gone round
 * twice, as SVG's — begun Offset into the pattern at each contour. A pattern
 * of no length, a negative length or more than DASHES_MOST of them is no
 * pattern: the stroke is solid — and a negative length spills the shape. */
static inline void BoxStrokeDash(BoxStroke *K, const int32_t *Dashes, uint32_t Count, int32_t Offset)
{
    int64_t Round = 0;
    K->Dashes = (const int32_t *)0;
    if (!Dashes || Count == 0 || Count > BOX_STROKE_DASHES_MOST) return;
    for (uint32_t I = 0; I < Count; I++) {
        if (Dashes[I] < 0 || Dashes[I] > BOX_SHAPE_OWN_MOST) {
            K->Shape->Spilled = 1;
            return;
        }
        Round += Dashes[I];
    }
    if (Round == 0) return;
    K->Dashes     = Dashes;
    K->DashCount  = Count;
    K->DashPeriod = Count & 1u ? 2u * Count : Count;
    if (Count & 1u) Round *= 2;
    K->DashOffset = ((int64_t)Offset % Round + Round) % Round;
}

/* The contour's dash set where it begins: Offset walked into the pattern. */
static inline void BoxStrokeDashStart(BoxStroke *K)
{
    if (!K->Dashes) {
        K->DashOn = 1;
        return;
    }
    int64_t Into  = K->DashOffset;
    K->DashIndex  = 0;
    K->DashLeft   = BoxStrokeDashLength(K, 0);
    while (Into > 0 && Into >= K->DashLeft) {
        Into -= K->DashLeft;
        K->DashIndex = (K->DashIndex + 1u) % K->DashPeriod;
        K->DashLeft  = BoxStrokeDashLength(K, K->DashIndex);
    }
    K->DashLeft -= Into;
    K->DashOn    = (K->DashIndex & 1u) == 0;
}

/* The contour left open: the caps at both its ends — or, when it has no
 * length, a dot where it began. */
static inline void BoxStrokeEnd(BoxStroke *K)
{
    if (!K->Open) return;
    if (!K->Drawn) {
        if (K->StartOn) BoxStrokeDot(K, K->StartX, K->StartY, 1, 0);
    } else {
        BoxStrokeEndPiece(K, K->AtX, K->AtY, K->LastX, K->LastY);
        if (K->FirstWaits) BoxStrokeCap(K, K->FirstX, K->FirstY, -K->FirstWayX, -K->FirstWayY);
    }
    K->Open       = 0;
    K->Piece      = 0;
    K->FirstWaits = 0;
}

static inline void BoxStrokeMoveTo(BoxStroke *K, int32_t X, int32_t Y)
{
    BoxStrokeEnd(K);
    K->Open   = 1;
    K->Fresh  = 1;
    K->Drawn  = 0;
    K->StartX = K->AtX = X;
    K->StartY = K->AtY = Y;
    K->Piece  = 0;
    K->Bent   = 0;
    BoxStrokeDashStart(K);
    K->StartOn = K->DashOn;
}

/* The pen, before an edge: none begun spills the shape. */
static inline int BoxStrokePen(BoxStroke *K)
{
    if (!K->Open) K->Shape->Spilled = 1;
    return K->Open && !K->Shape->Spilled;
}

/* The contour's next corner at X, Y, joined as Join says. */
static inline void BoxStrokeOn(BoxStroke *K, int32_t X, int32_t Y, uint32_t Join)
{
    if (!BoxStrokePen(K)) return;
    BoxStrokeEdge(K, K->AtX, K->AtY, X, Y, Join);
    K->AtX = X;
    K->AtY = Y;
}

static inline void BoxStrokeLineTo(BoxStroke *K, int32_t X, int32_t Y)
{
    BoxStrokeOn(K, X, Y, K->Join);
}

/* A quadratic curve from the pen through the pull of CX, CY to X, Y. */
static inline void BoxStrokeQuadTo(BoxStroke *K, int32_t CX, int32_t CY, int32_t X, int32_t Y)
{
    const int32_t Own[4] = { CX, CY, X, Y };
    if (!BoxStrokePen(K) || !BoxShapeOwn(K->Shape, Own, 4)) return;
    const int64_t  SX = K->AtX, SY = K->AtY;
    const uint32_t N  = BoxShapeCurveSteps(BoxShapeOnCanvas(K->Shape, BoxShapeApart(SX - 2 * CX + X, SY - 2 * CY + Y)), 1u);
    int64_t        PX = SX, PY = SY;
    for (uint32_t I = 1; I <= N; I++) {
        const int64_t T  = (int64_t)I * BOX_SHAPE_UNIT / N;
        const int64_t QX = I == N ? X : BoxShapeLerp(BoxShapeLerp(SX, CX, T), BoxShapeLerp(CX, X, T), T);
        const int64_t QY = I == N ? Y : BoxShapeLerp(BoxShapeLerp(SY, CY, T), BoxShapeLerp(CY, Y, T), T);
        BoxStrokeEdge(K, PX, PY, QX, QY, I == 1 ? K->Join : BOX_STROKE_JOIN_ROUND);
        PX = QX;
        PY = QY;
    }
    K->AtX = X;
    K->AtY = Y;
}

/* A cubic curve from the pen, pulled by C1 and C2, to X, Y. */
static inline void BoxStrokeCubicTo(BoxStroke *K, int32_t C1X, int32_t C1Y, int32_t C2X, int32_t C2Y, int32_t X,
                                    int32_t Y)
{
    const int32_t Own[6] = { C1X, C1Y, C2X, C2Y, X, Y };
    if (!BoxStrokePen(K) || !BoxShapeOwn(K->Shape, Own, 6)) return;
    const int64_t  SX   = K->AtX, SY = K->AtY;
    const int64_t  Bend = BoxShapeMost(BoxShapeApart(SX - 2 * C1X + C2X, SY - 2 * C1Y + C2Y),
                                       BoxShapeApart(C1X - 2 * C2X + X, C1Y - 2 * C2Y + Y));
    const uint32_t N    = BoxShapeCurveSteps(BoxShapeOnCanvas(K->Shape, Bend), 3u);
    int64_t        PX = SX, PY = SY;
    for (uint32_t I = 1; I <= N; I++) {
        const int64_t T = (int64_t)I * BOX_SHAPE_UNIT / N;
        int64_t       QX = X, QY = Y;
        if (I < N) {
            const int64_t AX  = BoxShapeLerp(SX, C1X, T), AY = BoxShapeLerp(SY, C1Y, T);
            const int64_t BX  = BoxShapeLerp(C1X, C2X, T), BY = BoxShapeLerp(C1Y, C2Y, T);
            const int64_t CX  = BoxShapeLerp(C2X, X, T), CY = BoxShapeLerp(C2Y, Y, T);
            const int64_t ABX = BoxShapeLerp(AX, BX, T), ABY = BoxShapeLerp(AY, BY, T);
            const int64_t BCX = BoxShapeLerp(BX, CX, T), BCY = BoxShapeLerp(BY, CY, T);
            QX                = BoxShapeLerp(ABX, BCX, T);
            QY                = BoxShapeLerp(ABY, BCY, T);
        }
        BoxStrokeEdge(K, PX, PY, QX, QY, I == 1 ? K->Join : BOX_STROKE_JOIN_ROUND);
        PX = QX;
        PY = QY;
    }
    K->AtX = X;
    K->AtY = Y;
}

/* The contour closed: an edge back to where it began, and that corner joined
 * — the first piece to the last, no caps — when the line runs on through it. */
static inline void BoxStrokeClose(BoxStroke *K)
{
    if (!K->Open || K->Shape->Spilled) return;
    BoxStrokeEdge(K, K->AtX, K->AtY, K->StartX, K->StartY, K->Join);
    K->AtX = K->StartX;
    K->AtY = K->StartY;
    if (!K->Drawn) {
        if (K->StartOn) BoxStrokeDot(K, K->StartX, K->StartY, 1, 0);
    } else if (K->Piece && K->FirstWaits) {
        BoxStrokeCorner(K, K->StartX, K->StartY, K->LastX, K->LastY, K->FirstWayX, K->FirstWayY, K->Join);
    } else {
        BoxStrokeEndPiece(K, K->StartX, K->StartY, K->LastX, K->LastY);
        if (K->FirstWaits) BoxStrokeCap(K, K->FirstX, K->FirstY, -K->FirstWayX, -K->FirstWayY);
    }
    K->Open       = 0;
    K->Piece      = 0;
    K->FirstWaits = 0;
}

/* ---------------------------------------------------------------------------
 * Ready shapes stroked: along the very edges their fills are made of
 * (boxos_shape.h), so a line drawn round a filled shape lies on its edge.
 * ------------------------------------------------------------------------- */

static inline void BoxStrokeRect(BoxStroke *K, int32_t X, int32_t Y, int32_t Width, int32_t Height)
{
    BoxStrokeMoveTo(K, X, Y);
    BoxStrokeLineTo(K, X + Width, Y);
    BoxStrokeLineTo(K, X + Width, Y + Height);
    BoxStrokeLineTo(K, X, Y + Height);
    BoxStrokeClose(K);
}

/* The contour closed with every corner joined round, as a curve's are. */
static inline void BoxStrokeCloseRound(BoxStroke *K)
{
    const uint32_t Join = K->Join;
    K->Join             = BOX_STROKE_JOIN_ROUND;
    BoxStrokeClose(K);
    K->Join = Join;
}

static inline void BoxStrokeEllipse(BoxStroke *K, int32_t CX, int32_t CY, int32_t RX, int32_t RY)
{
    const int32_t Own[4] = { CX, CY, RX, RY };
    if (!BoxShapeOwn(K->Shape, Own, 4)) return;
    const BoxShapeOval O = BoxShapeOvalOf(K->Shape, CX, CY, RX, RY);
    for (uint32_t I = 0; I < O.Steps; I++) {
        int32_t X, Y;
        BoxShapeOvalPoint(&O, I, &X, &Y);
        if (I == 0) BoxStrokeMoveTo(K, X, Y);
        else        BoxStrokeOn(K, X, Y, BOX_STROKE_JOIN_ROUND);
    }
    BoxStrokeCloseRound(K);
}

/* A rounded rectangle's edge (BoxShapeAddRounded): its arcs joined round, a
 * corner of no radius as the stroke's Join says. */
static inline void BoxStrokeRounded(BoxStroke *K, int32_t X, int32_t Y, int32_t Width, int32_t Height,
                                    const int32_t Radii[4])
{
    const int32_t Own[8] = { X, Y, Width, Height, Radii[0], Radii[1], Radii[2], Radii[3] };
    if (!BoxShapeOwn(K->Shape, Own, 8) || Width <= 0 || Height <= 0) return;
    BoxShapeRounding R;
    if (!BoxShapeRoundingOf(K->Shape, X, Y, Width, Height, Radii, &R)) {
        BoxStrokeRect(K, X, Y, Width, Height);
        return;
    }
    int First = 1;
    for (uint32_t C = 0; C < 4u; C++)
        for (uint32_t J = 0; J <= R.Steps[C]; J++) {
            int32_t PX, PY;
            BoxShapeRoundingPoint(&R, C, J, &PX, &PY);
            if (First) BoxStrokeMoveTo(K, PX, PY);
            else       BoxStrokeOn(K, PX, PY, R.Radius[C] > 0 ? BOX_STROKE_JOIN_ROUND : K->Join);
            First = 0;
        }
    /* The last corner closes onto the first: round when the top right is. */
    const uint32_t Join = K->Join;
    if (R.Radius[0] > 0) K->Join = BOX_STROKE_JOIN_ROUND;
    BoxStrokeClose(K);
    K->Join = Join;
}

/* A line Width wide from X0, Y0 to X1, Y1 into Shape, its ends as Cap says; a
 * line of no length a dot of its cap, nothing when it is cut short. */
static inline void BoxStrokeLine(BoxShape *Shape, int32_t X0, int32_t Y0, int32_t X1, int32_t Y1, int32_t Width,
                                 uint32_t Cap)
{
    BoxStroke K;
    BoxStrokeBegin(&K, Shape, Width, Cap, BOX_STROKE_JOIN_MITER);
    BoxStrokeMoveTo(&K, X0, Y0);
    BoxStrokeLineTo(&K, X1, Y1);
    BoxStrokeEnd(&K);
}

#endif /* BOXOS_STROKE_H */
