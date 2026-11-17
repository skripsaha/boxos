/*
 * draw stand — shapes, strokes and paints (boxos_shape.h, boxos_stroke.h,
 * boxos_paint.h): a sweep's cells those of every edge laid alone, EVENODD,
 * rounded corners, strokes to their exact areas, turns, gradients and
 * pictures.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

#include "draw_stand.h"

namespace stand {

namespace {

constexpr std::int32_t ONE  = BOX_SHAPE_ONE;
constexpr std::int32_t UNIT = BOX_SHAPE_UNIT;

/* Whether a shape whose round parts are Arc pixels long covers Want less no
 * more than their chords leave out: each chord within FLAT of its arc, a
 * parabola's two thirds of that between them. */
bool Chorded(double Got, double Want, double Arc)
{
    const double Flat = (double)BOX_SHAPE_FLAT / BOX_SHAPE_ONE;
    return Got <= Want + 1e-6 && Want - Got <= Arc * Flat * 2.0 / 3.0;
}

std::string Said(const char *Format, double A, double B)
{
    char Line[240];
    std::snprintf(Line, sizeof(Line), Format, A, B);
    return Line;
}

/* The area a stroke holds, in pixels, on a canvas of 400 x 400. */
double StrokeArea(const std::function<void(BoxStroke &)> &Draw, std::int32_t Width, std::uint32_t Cap,
                  std::uint32_t Join, const BoxMatrix &M = Plain)
{
    ShapeRoom Room;
    BoxShape &S = Room.Begin(M);
    BoxStroke K;
    BoxStrokeBegin(&K, &S, Width, Cap, Join);
    Draw(K);
    BoxStrokeEnd(&K);
    return CoverageOf(S, 400, 400).Area;
}

/* Random shapes of every kind, filled and stroked, into S. */
void RandomShape(BoxShape &S)
{
    const std::int32_t Size = Within(ONE / 4, 90 * ONE);
    switch (Random() % 6u) {
    case 0: BoxShapeAddRect(&S, -Size, -Size / 2, 2 * Size, Size, (int)(Random() % 2u)); break;
    case 1: BoxShapeAddEllipse(&S, 0, 0, Size, Size / 2 + 1, (int)(Random() % 2u)); break;
    case 2: {
        const std::int32_t Radii[4] = { Within(0, Size), Within(0, Size), Within(0, Size), Within(0, Size) };
        BoxShapeAddRounded(&S, -Size, -Size, 2 * Size, Size, Radii, (int)(Random() % 2u));
        break;
    }
    case 3: {
        BoxStroke K;
        BoxStrokeBegin(&K, &S, Within(1, Size / 3 + 1), Random() % 3u, Random() % 3u);
        const std::int32_t Dashes[3] = { Within(0, Size), Within(1, Size), Within(0, Size) };
        if (Random() % 2u) BoxStrokeDash(&K, Dashes, 1u + Random() % 3u, Within(-Size, Size));
        BoxStrokeMoveTo(&K, Within(-Size, Size), Within(-Size, Size));
        for (std::uint32_t I = 0; I < 6u; I++) {
            if (Random() % 3u == 0)
                BoxStrokeCubicTo(&K, Within(-Size, Size), Within(-Size, Size), Within(-Size, Size), Within(-Size, Size),
                                 Within(-Size, Size), Within(-Size, Size));
            else
                BoxStrokeLineTo(&K, Within(-Size, Size), Within(-Size, Size));
        }
        if (Random() % 2u) BoxStrokeClose(&K);
        BoxStrokeEnd(&K);
        break;
    }
    case 4:
        BoxShapeMoveTo(&S, 0, 0);
        BoxShapeCubicTo(&S, Size, -Size, Size, Size, 0, Size);
        BoxShapeQuadTo(&S, -Size, 0, 0, 0);
        break;
    default:
        S.Rule = Random() % 2u ? BOX_SHAPE_EVENODD : BOX_SHAPE_NONZERO;
        BoxShapeMoveTo(&S, 0, 0);
        for (std::uint32_t K = 0; K < 11u; K++) BoxShapeLineTo(&S, Within(-Size, Size), Within(-Size, Size));
    }
}

BoxMatrix RandomMatrix()
{
    return BoxMatrixPlaced(Within(0, 200) * ONE + Within(0, ONE), Within(0, 160) * ONE + Within(0, ONE),
                           Within(UNIT / 4, 2 * UNIT), Random());
}

}  // namespace

void CheckShapes(Tally &Count)
{
    /* The sweep's cells, row by row, those of every edge laid on its own. */
    std::uint32_t Rows = 0, Wrong = 0;
    for (std::uint32_t I = 0; I < 600u; I++) {
        ShapeRoom Room;
        BoxShape &S = Room.Begin(RandomMatrix());
        RandomShape(S);
        BoxShapeClose(&S);
        if (!BoxShapeDrawable(&S)) continue;
        const std::vector<BoxShapeEdge> Alone(S.Edges, S.Edges + S.Count);
        const BoxRect Held = BoxShapeReach(&S, BoxRect{ 0, 0, 260, 220 });
        if (Held.Width <= 0 || Held.Height <= 0) continue;
        std::vector<std::int32_t> Swept((std::size_t)Held.Width + 2), Each((std::size_t)Held.Width + 2);
        BoxShapeSweep             Sweep;
        BoxShapeSweepBegin(&Sweep, &S, Swept.data(), Held.X, Held.X + Held.Width);
        for (std::int32_t Y = Held.Y; Y < Held.Y + Held.Height; Y++) {
            BoxShapeSweepRow(&Sweep, Y);
            std::fill(Each.begin(), Each.end(), 0);
            for (const BoxShapeEdge &E : Alone) BoxShapeRowEdge(Each.data(), Held.X, Held.X + Held.Width, &E, Y);
            Rows++;
            if (Swept != Each) Wrong++;
        }
    }
    Count.Check(Wrong == 0 && Rows > 10000,
                Said("a shape swept row by row lays the very cells of every edge laid alone (%.0f rows, %.0f wrong)",
                     Rows, Wrong));

    /* EVENODD: where two squares overlap, a hole; a star's middle bare. */
    {
        ShapeRoom Room;
        BoxShape &S = Room.Begin(Plain);
        BoxShapeAddRect(&S, 10 * ONE, 10 * ONE, 40 * ONE, 40 * ONE, 0);
        BoxShapeAddRect(&S, 30 * ONE, 30 * ONE, 40 * ONE, 40 * ONE, 0);
        const Coverage Joined = CoverageOf(S, 100, 100);
        ShapeRoom      Room2;
        BoxShape      &T = Room2.Begin(Plain);
        T.Rule           = BOX_SHAPE_EVENODD;
        BoxShapeAddRect(&T, 10 * ONE, 10 * ONE, 40 * ONE, 40 * ONE, 0);
        BoxShapeAddRect(&T, 30 * ONE, 30 * ONE, 40 * ONE, 40 * ONE, 0);
        const Coverage Odd = CoverageOf(T, 100, 100);
        Count.Check(Joined.Area == 2800.0 && Odd.Area == 2400.0 && Joined.At(40, 40) == 255 && Odd.At(40, 40) == 0 &&
                        Odd.At(20, 20) == 255,
                    Said("two squares overlapping: NONZERO covers %.0f, EVENODD %.0f, the overlap bare", Joined.Area,
                         Odd.Area));
        ShapeRoom Room3;
        BoxShape &Star = Room3.Begin(Plain);
        Star.Rule      = BOX_SHAPE_EVENODD;
        for (std::uint32_t K = 0; K < 5u; K++) {
            const std::uint32_t Turn = (std::uint32_t)K * 2u * BOX_SHAPE_TURN / 5u + BOX_SHAPE_TURN * 3u / 4u;
            const std::int32_t  X = 50 * ONE + (std::int32_t)BoxShapeRound((std::int64_t)40 * ONE * BoxShapeCosine(Turn));
            const std::int32_t  Y = 50 * ONE + (std::int32_t)BoxShapeRound((std::int64_t)40 * ONE * BoxShapeSine(Turn));
            if (K == 0) BoxShapeMoveTo(&Star, X, Y);
            else        BoxShapeLineTo(&Star, X, Y);
        }
        const Coverage Holed = CoverageOf(Star, 100, 100);
        Star.Rule            = BOX_SHAPE_NONZERO;
        const Coverage Whole = CoverageOf(Star, 100, 100);
        Count.Check(Holed.At(50, 50) == 0 && Whole.At(50, 50) == 255 && Whole.Area > Holed.Area,
                    "a star drawn in one stroke: EVENODD leaves its middle bare, NONZERO fills it");
    }

    /* Rounded corners, each its own radius, shrunk as CSS shrinks them. */
    {
        ShapeRoom          Room;
        BoxShape          &S        = Room.Begin(Plain);
        const std::int32_t Radii[4] = { 0, 10 * ONE, 30 * ONE, 50 * ONE };
        BoxShapeAddRounded(&S, 20 * ONE, 20 * ONE, 200 * ONE, 100 * ONE, Radii, 0);
        const Coverage C    = CoverageOf(S, 300, 200);
        const double   Want = 20000.0 - (1.0 - M_PI / 4.0) * (100.0 + 900.0 + 2500.0);
        Count.Check(Chorded(C.Area, Want, M_PI / 2.0 * 90.0) && C.At(20, 20) == 255 && C.At(219, 119) == 0 && C.At(219, 20) < 255,
                    Said("a rounded rectangle of four radii covers its area, %.2f of %.2f; a corner of none square",
                         C.Area, Want));
        ShapeRoom          Room2;
        BoxShape          &T    = Room2.Begin(Plain);
        const std::int32_t Big[4] = { 80 * ONE, 80 * ONE, 80 * ONE, 80 * ONE };
        BoxShapeAddRounded(&T, 50 * ONE, 50 * ONE, 100 * ONE, 100 * ONE, Big, 0);
        const double Circle = CoverageOf(T, 200, 200).Area;
        ShapeRoom          Room3;
        BoxShape          &U     = Room3.Begin(Plain);
        const std::int32_t Top[4] = { 150 * ONE, 150 * ONE, 0, 0 };
        BoxShapeAddRounded(&U, 20 * ONE, 20 * ONE, 200 * ONE, 100 * ONE, Top, 0);
        const double Arch = CoverageOf(U, 300, 200).Area, ArchWant = 20000.0 - 2.0 * (1.0 - M_PI / 4.0) * 10000.0;
        Count.Check(Chorded(Circle, M_PI * 2500.0, M_PI * 100.0) && Chorded(Arch, ArchWant, M_PI * 100.0),
                    Said("radii past their sides shrunk alike: a square of radius 80 a circle (%.2f), an arch %.2f",
                         Circle, Arch));
    }

    /* The turn of a place held to the host's atan2. */
    {
        std::int64_t Worst = 0;
        for (std::uint32_t I = 0; I < 200000u; I++) {
            const std::int64_t X = (std::int64_t)Within(-1 << 30, 1 << 30) >> (Random() % 30u);
            const std::int64_t Y = (std::int64_t)Within(-1 << 30, 1 << 30) >> (Random() % 30u);
            if (X == 0 && Y == 0) continue;
            double Want = std::atan2((double)Y, (double)X) / (2.0 * M_PI) * 65536.0;
            if (Want < 0) Want += 65536.0;
            std::int64_t Off = std::llabs((std::int64_t)BoxShapeTurnOf(X, Y) - std::llround(Want)) % 65536;
            if (Off > 32768) Off = 65536 - Off;
            if (Off > Worst) Worst = Off;
        }
        Count.Check(Worst <= 1 && BoxShapeTurnOf(1, 0) == 0 && BoxShapeTurnOf(0, 1) == 16384 &&
                        BoxShapeTurnOf(-1, 0) == 32768 && BoxShapeTurnOf(0, -1) == 49152 && BoxShapeTurnOf(5, 5) == 8192,
                    Said("the turn of a place is the host's atan2 to %.0f of 65536, the quarters exact", (double)Worst,
                         0));
    }
}

void CheckStrokes(Tally &Count)
{
    const std::int32_t W = 8 * ONE;   /* half a width of 4 pixels */
    const double       H = 4.0;
    auto Line = [](std::int32_t X0, std::int32_t Y0, std::int32_t X1, std::int32_t Y1) {
        return [=](BoxStroke &K) {
            BoxStrokeMoveTo(&K, X0, Y0);
            BoxStrokeLineTo(&K, X1, Y1);
        };
    };
    const auto Straight = Line(50 * ONE, 100 * ONE, 150 * ONE, 100 * ONE);
    const double Butt   = StrokeArea(Straight, W, BOX_STROKE_CAP_BUTT, BOX_STROKE_JOIN_MITER);
    const double Square = StrokeArea(Straight, W, BOX_STROKE_CAP_SQUARE, BOX_STROKE_JOIN_MITER);
    const double Round  = StrokeArea(Straight, W, BOX_STROKE_CAP_ROUND, BOX_STROKE_JOIN_MITER);
    Count.Check(Butt == 800.0 && Square == 864.0 && Near(Round, 800.0 + M_PI * H * H, 0.002),
                Said("a line 100 long and 8 wide: %.2f cut short, square past both ends 864, %.2f round", Butt, Round));

    /* A right angle: two legs of 50, joined. */
    const auto Corner = [](BoxStroke &K) {
        BoxStrokeMoveTo(&K, 50 * ONE, 50 * ONE);
        BoxStrokeLineTo(&K, 100 * ONE, 50 * ONE);
        BoxStrokeLineTo(&K, 100 * ONE, 100 * ONE);
    };
    const double Miter = StrokeArea(Corner, W, BOX_STROKE_CAP_BUTT, BOX_STROKE_JOIN_MITER);
    const double Bevel = StrokeArea(Corner, W, BOX_STROKE_CAP_BUTT, BOX_STROKE_JOIN_BEVEL);
    const double Arc   = StrokeArea(Corner, W, BOX_STROKE_CAP_BUTT, BOX_STROKE_JOIN_ROUND);
    Count.Check(Miter == 800.0 && Bevel == 792.0 && Near(Arc, 800.0 - H * H + M_PI * H * H / 4.0, 0.001),
                Said("a right angle of two legs of 50, 8 wide: %.2f mitered, 792 beveled, %.2f round", Miter, Arc));
    const double Limited = StrokeArea(
        [&](BoxStroke &K) {
            BoxStrokeMiterLimit(&K, 307u);   /* 1.2 widths: under the right angle's 1.41 */
            Corner(K);
        },
        W, BOX_STROKE_CAP_BUTT, BOX_STROKE_JOIN_MITER);
    Count.Check(Limited == 792.0, Said("a miter past its limit is cut to a bevel, %.2f of %.0f", Limited, 792.0));

    /* A sharp turn, 20°: past the limit of 4 a bevel, within 6 a miter whose
     * tip adds h²·(tan 80° − sin 160° / 2). */
    const auto Sharp = [](BoxStroke &K) {
        BoxStrokeMoveTo(&K, 60 * ONE, 100 * ONE);
        BoxStrokeLineTo(&K, 160 * ONE, 100 * ONE);
        BoxStrokeLineTo(&K, (std::int32_t)(160 * ONE - std::lround(100.0 * ONE * std::cos(M_PI / 9.0))),
                        (std::int32_t)(100 * ONE + std::lround(100.0 * ONE * std::sin(M_PI / 9.0))));
    };
    const double Cut  = StrokeArea(Sharp, W, BOX_STROKE_CAP_BUTT, BOX_STROKE_JOIN_MITER);
    const double Long = StrokeArea(
        [&](BoxStroke &K) {
            BoxStrokeMiterLimit(&K, 6u * 256u);
            Sharp(K);
        },
        W, BOX_STROKE_CAP_BUTT, BOX_STROKE_JOIN_MITER);
    const double Tip = H * H * (std::tan(80.0 * M_PI / 180.0) - std::sin(160.0 * M_PI / 180.0) / 2.0);
    Count.Check(Near(Long - Cut, Tip, 0.01), Said("a turn of 20°: its miter's tip %.2f past the bevel, of %.2f", Long - Cut, Tip));

    /* A square closed: no caps, every corner joined, the first to the last. */
    const auto Box = [](BoxStroke &K) {
        BoxStrokeMoveTo(&K, 20 * ONE, 20 * ONE);
        BoxStrokeLineTo(&K, 80 * ONE, 20 * ONE);
        BoxStrokeLineTo(&K, 80 * ONE, 80 * ONE);
        BoxStrokeLineTo(&K, 20 * ONE, 80 * ONE);
        BoxStrokeClose(&K);
    };
    const double BoxMiter = StrokeArea(Box, W, BOX_STROKE_CAP_ROUND, BOX_STROKE_JOIN_MITER);
    const double BoxBevel = StrokeArea(Box, W, BOX_STROKE_CAP_ROUND, BOX_STROKE_JOIN_BEVEL);
    const double BoxRound = StrokeArea(Box, W, BOX_STROKE_CAP_ROUND, BOX_STROKE_JOIN_ROUND);
    Count.Check(BoxMiter == 1920.0 && BoxBevel == 1888.0 && Near(BoxRound, 1920.0 - 4 * H * H + M_PI * H * H, 0.001),
                Said("a square of 60 closed, 8 wide: %.2f mitered — its ring exactly — 1888 beveled, %.2f round, no caps",
                     BoxMiter, BoxRound));

    /* Dashes and dots. */
    const auto Dashed = [&](const std::vector<std::int32_t> &Pattern, std::int32_t Offset, std::uint32_t Cap) {
        return StrokeArea(
            [&](BoxStroke &K) {
                BoxStrokeDash(&K, Pattern.data(), (std::uint32_t)Pattern.size(), Offset);
                Straight(K);
            },
            W, Cap, BOX_STROKE_JOIN_MITER);
    };
    const double Even  = Dashed({ 10 * ONE, 10 * ONE }, 0, BOX_STROKE_CAP_BUTT);
    const double Moved = Dashed({ 10 * ONE, 10 * ONE }, 5 * ONE, BOX_STROKE_CAP_BUTT);
    const double Odd   = Dashed({ 10 * ONE, 5 * ONE, 5 * ONE }, 0, BOX_STROKE_CAP_BUTT);
    const double Dots  = Dashed({ 0, 10 * ONE }, 0, BOX_STROKE_CAP_ROUND);
    Count.Check(Even == 400.0 && Moved == 400.0, Said("dashes along 100, 10 on 10 off: %.0f, and begun 5 in, %.0f", Even, Moved));
    Count.Check(Odd == 440.0, Said("dashes 10 5 5, an odd count, gone round twice: %.0f of %.0f", Odd, 440.0));
    Count.Check(Chorded(Dots, 11.0 * M_PI * H * H, 11.0 * 2.0 * M_PI * H),
                Said("dots — dashes of no length every 10, round: eleven circles, %.2f of %.2f", Dots, 11.0 * M_PI * H * H));

    /* A line of no length: a dot of its cap. */
    const auto Point = Line(100 * ONE, 100 * ONE, 100 * ONE, 100 * ONE);
    Count.Check(Near(StrokeArea(Point, W, BOX_STROKE_CAP_ROUND, 0), M_PI * H * H, 0.01) &&
                    StrokeArea(Point, W, BOX_STROKE_CAP_SQUARE, 0) == 64.0 &&
                    StrokeArea(Point, W, BOX_STROKE_CAP_BUTT, 0) == 0.0,
                "a line of no length: a circle round, a square squared, nothing cut short");

    /* Turned right back: the round join half a circle past the turn. */
    const double Back = StrokeArea(
        [](BoxStroke &K) {
            BoxStrokeMoveTo(&K, 50 * ONE, 100 * ONE);
            BoxStrokeLineTo(&K, 100 * ONE, 100 * ONE);
            BoxStrokeLineTo(&K, 50 * ONE, 100 * ONE);
        },
        W, BOX_STROKE_CAP_BUTT, BOX_STROKE_JOIN_ROUND);
    Count.Check(Near(Back, 400.0 + M_PI * H * H / 2.0, 0.002),
                Said("a line turned right back, joined round: %.2f, half a circle past the turn — %.2f", Back,
                     400.0 + M_PI * H * H / 2.0));

    /* A pen stretched three times down draws three times as wide; mirrored,
     * the same. */
    const BoxMatrix Tall   = { UNIT, 0, 0, 3 * UNIT, 0, 0 };
    const BoxMatrix Mirror = { -UNIT, 0, 0, UNIT, 300 * ONE, 0 };
    const auto      Flat   = Line(50 * ONE, 50 * ONE, 150 * ONE, 50 * ONE);
    Count.Check(StrokeArea(Flat, W, BOX_STROKE_CAP_BUTT, 0, Tall) == 2400.0 &&
                    StrokeArea(Box, W, BOX_STROKE_CAP_BUTT, BOX_STROKE_JOIN_MITER, Mirror) == 1920.0,
                "a stroke stretched three times across its width covers three times; mirrored, its very area");

    /* A circle stroked along four cubic quarters: its ring. */
    const double Ring = StrokeArea(
        [](BoxStroke &K) {
            const std::int32_t C = 150 * ONE, R = 100 * ONE, P = (std::int32_t)std::lround(100.0 * ONE * 0.5522847498);
            BoxStrokeMoveTo(&K, C + R, C);
            BoxStrokeCubicTo(&K, C + R, C + P, C + P, C + R, C, C + R);
            BoxStrokeCubicTo(&K, C - P, C + R, C - R, C + P, C - R, C);
            BoxStrokeCubicTo(&K, C - R, C - P, C - P, C - R, C, C - R);
            BoxStrokeCubicTo(&K, C + P, C - R, C + R, C - P, C + R, C);
            BoxStrokeClose(&K);
        },
        6 * ONE, BOX_STROKE_CAP_BUTT, BOX_STROKE_JOIN_MITER);
    Count.Check(Near(Ring, 2.0 * M_PI * 100.0 * 6.0, 0.003),
                Said("a circle of 100 stroked 6 wide along its curves covers its ring, %.2f of %.2f", Ring,
                     2.0 * M_PI * 600.0));

    /* Ready shapes stroked along their fills' own edges: a band the length of
     * the middle line times the width. */
    const double Framed = StrokeArea([](BoxStroke &K) { BoxStrokeRect(&K, 20 * ONE, 20 * ONE, 60 * ONE, 60 * ONE); }, W,
                                     BOX_STROKE_CAP_BUTT, BOX_STROKE_JOIN_MITER);
    const double Oval   = StrokeArea([](BoxStroke &K) { BoxStrokeEllipse(&K, 150 * ONE, 150 * ONE, 100 * ONE, 100 * ONE); },
                                     6 * ONE, BOX_STROKE_CAP_BUTT, BOX_STROKE_JOIN_MITER);
    const double Pill   = StrokeArea(
        [](BoxStroke &K) {
            const std::int32_t Radii[4] = { 20 * ONE, 20 * ONE, 0, 40 * ONE };
            BoxStrokeRounded(&K, 50 * ONE, 50 * ONE, 200 * ONE, 120 * ONE, Radii);
        },
        W, BOX_STROKE_CAP_BUTT, BOX_STROKE_JOIN_MITER);
    /* The pill's middle line: its sides less what its corners cut, and the
     * corners' quarter circles; its square corner adds a miter's square. */
    const double PillWant = (2.0 * (200.0 + 120.0) - 2.0 * (20 + 20 + 40) + M_PI / 2.0 * (20 + 20 + 40)) * 8.0;
    Count.Check(Framed == 1920.0 && Near(Oval, 2.0 * M_PI * 100.0 * 6.0, 0.003) && Near(Pill, PillWant, 0.003),
                Said("ready shapes stroked on their own edges: a square's ring 1920, a circle's %.2f, a rounded "
                     "rectangle's %.2f",
                     Oval, Pill));
}

void CheckPaints(Tally &Count)
{
    const std::int64_t   G       = BOX_PAINT_GRADIENT;
    const BoxPaintStop   Stops[2] = { { 0, 0xFF000000u }, { (std::uint32_t)G, 0xFFFFFFFFu } };
    /* The gradient's space: 0 to its end over 100 pixels from 100, 100. */
    const BoxMatrix Space = { (std::int32_t)(100 * ONE), 0, 0, (std::int32_t)(100 * ONE), 100 * ONE, 100 * ONE };

    /* A plain radial gradient's way: the root of its place, exactly. */
    {
        BoxPaint P{};
        P.Kind = BOX_PAINT_RADIAL;
        std::uint32_t Wrong = 0;
        for (std::uint32_t I = 0; I < 100000u; I++) {
            const std::int64_t U = Within(-4 * (std::int32_t)G, 4 * (std::int32_t)G);
            const std::int64_t V = Within(-4 * (std::int32_t)G, 4 * (std::int32_t)G);
            if (BoxPaintRadialWay(&P, U, V) != (std::int64_t)std::floor(std::sqrt((double)(U * U + V * V)))) Wrong++;
        }
        Count.Check(Wrong == 0, "a radial gradient's way is the root of its place's distance, below, exactly");
    }

    /* Drawn from a focal point: its first colour there, its last on its
     * circle. */
    {
        BoxPaint P{};
        P.Kind      = BOX_PAINT_RADIAL;
        P.Stops     = Stops;
        P.StopCount = 2;
        P.Space     = Space;
        P.FocalX    = (std::int32_t)(G / 2);
        BoxPaintReady R;
        std::uint32_t AtFocus = 1, OnCircle = 0, Within = 0;
        const int     Ready = BoxPaintPrepare(&P, &R);
        BoxPaintWay(&R, 149, 99, &AtFocus);    /* the middle of pixel 149, 99: half a pixel from the focus */
        BoxPaintWay(&R, 0, 0, &OnCircle);      /* past the circle, at the canvas's corner */
        BoxPaintWay(&R, 99, 99, &Within);
        P.FocalX = (std::int32_t)G;
        BoxPaintReady Refused;
        Count.Check(Ready && AtFocus < G / 100 && OnCircle == G && Within > AtFocus && Within < G &&
                        !BoxPaintPrepare(&P, &Refused),
                    "a radial gradient from a focal point: none of its way there, all of it on its circle; a focal "
                    "point on the circle refused");
    }

    /* Conic: its way the turn round its middle. */
    {
        BoxPaint P{};
        P.Kind      = BOX_PAINT_CONIC;
        P.Stops     = Stops;
        P.StopCount = 2;
        P.Space     = Space;
        BoxPaintReady R;
        BoxPaintPrepare(&P, &R);
        std::uint32_t Wrong = 0;
        for (std::int32_t Y = 0; Y < 200; Y += 7)
            for (std::int32_t X = 0; X < 200; X += 7) {
                std::uint32_t Way;
                BoxPaintWay(&R, X, Y, &Way);
                double Want = std::atan2(Y + 0.5 - 100.0, X + 0.5 - 100.0) / (2 * M_PI) * 65536.0;
                if (Want < 0) Want += 65536.0;
                const double Off = std::fabs(Way - Want);
                if (Off > 2.0 && Off < 65534.0) Wrong++;
            }
        Count.Check(Wrong == 0, "a conic gradient's way is the turn round its middle, from +x toward +y");
    }

    /* Past the ends: held, begun again, run back, clear. */
    {
        const std::uint32_t Extends[4] = { BOX_PAINT_PAD, BOX_PAINT_REPEAT, BOX_PAINT_REFLECT, BOX_PAINT_NONE };
        /* Pixel 229's middle lies 129.5 past the start: 1.295 of the way. */
        const double Past      = 129.5 / 100.0 * (double)G;
        const double Wanted[4] = { (double)G, Past - (double)G, 2.0 * (double)G - Past, -1.0 };
        bool         Holds     = true;
        for (std::uint32_t K = 0; K < 4u; K++) {
            BoxPaint P{};
            P.Kind      = BOX_PAINT_LINEAR;
            P.Stops     = Stops;
            P.StopCount = 2;
            P.Space     = Space;
            P.Extend    = Extends[K];
            BoxPaintReady R;
            BoxPaintPrepare(&P, &R);
            std::uint32_t Way = 0;
            const int     On  = BoxPaintWay(&R, 229, 0, &Way);
            if (Wanted[K] < 0 ? On != 0 : (!On || std::fabs((double)Way - Wanted[K]) > 2.0)) Holds = false;
        }
        Count.Check(Holds, "past a gradient's end: held at its last colour, begun again, run back, or clear");
    }

    /* A walk along a row: each place the one BoxPaintPlace gives. */
    {
        std::uint32_t Wrong = 0;
        for (std::uint32_t I = 0; I < 2000u; I++) {
            BoxPaint P{};
            P.Kind      = BOX_PAINT_LINEAR;
            P.Stops     = Stops;
            P.StopCount = 2;
            P.Space     = BoxMatrixPlaced(Within(-100, 300) * ONE, Within(-100, 300) * ONE, Within(UNIT / 8, 64 * UNIT), Random());
            P.Space.C   = Random() % 3u ? P.Space.C : Within(-UNIT, UNIT);
            BoxPaintReady R;
            if (!BoxPaintPrepare(&P, &R)) continue;
            const std::int32_t X0 = Within(-500, 500), Y = Within(-500, 500);
            BoxPaintWalk       W;
            BoxPaintWalkBegin(&R, X0, Y, &W);
            for (std::int32_t K = 0; K < 300; K++, BoxPaintWalkNext(&W)) {
                std::int64_t U, V, PU, PV;
                BoxPaintWalkAt(&W, &U, &V);
                BoxPaintPlace(&R, X0 + K, Y, &PU, &PV);
                if (U != PU || V != PV) Wrong++;
            }
        }
        Count.Check(Wrong == 0, "a walk along a row falls on the very places each pixel's own is worked out at");
    }

    /* Pictures: sharp, smooth as light, past their edge. */
    {
        const std::uint32_t Two[2] = { 0xFF000000u, 0xFFFFFFFFu };
        BoxPaint            P{};
        P.Kind          = BOX_PAINT_PICTURE;
        P.Picture       = Two;
        P.PictureWidth  = 2;
        P.PictureHeight = 1;
        P.PictureStride = 2;
        P.Space         = Plain;
        const std::uint32_t Middle = BoxPaintSample(&P, ONE, ONE / 2);
        const std::uint32_t Gray   = BoxColorCode(32768u);
        Count.Check(Middle == (0xFF000000u | Gray << 16 | Gray << 8 | Gray) && Gray == 188u,
                    "a picture between a black pixel and a white one is half their light, 188 — not 128");
        P.Sharp = 1;
        Count.Check(BoxPaintSample(&P, ONE - 1, ONE / 2) == Two[0] && BoxPaintSample(&P, ONE, ONE / 2) == Two[1],
                    "a sharp picture: each pixel square to its edge");
        std::size_t Index = 0;
        const std::uint32_t Extends[4] = { BOX_PAINT_PAD, BOX_PAINT_REPEAT, BOX_PAINT_REFLECT, BOX_PAINT_NONE };
        const std::size_t   Wanted[4]  = { 0, 1, 0, 9 };
        bool                Holds      = true;
        for (std::uint32_t K = 0; K < 4u; K++) {
            P.Extend      = Extends[K];
            const int On  = BoxPaintTexel(&P, -1, 0, &Index);
            if (Wanted[K] == 9 ? On != 0 : (!On || Index != Wanted[K])) Holds = false;
        }
        Count.Check(Holds, "a picture past its edge: its edge pixel held, repeated, mirrored, or clear");
    }
}

}  // namespace stand
