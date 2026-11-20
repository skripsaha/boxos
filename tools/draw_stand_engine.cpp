/*
 * draw stand — Painter's engine (painter/core): its coverage the brush's,
 * pixel for pixel, on every kind of shape; its colours within a code of the
 * brush's and of the exact sum; sixteen bits keeping a dark gradient free of
 * bands; clips, masks, several fills, aligned strokes, pictures, dither; and
 * what it must refuse refused.
 */

#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>

#include "draw_stand.h"

#include "boxos_brush.h"
#include "painter/core/canvas.h"
#include "painter/core/cpu_path.h"

namespace stand {

using namespace painter::core;
/* BoxLib's C colour type is named Color too: Painter's is meant here. */
using painter::core::Color;

namespace {

constexpr std::int32_t ONE = BOX_SHAPE_ONE;

float PixelsOf(std::int32_t Places) { return static_cast<float>(Places) / ONE; }

/* The shapes the brush and the engine both draw, told once: each kind built
 * from places into the brush's shape and the engine's path alike. */
struct Twin {
    ShapeRoom Room;
    BoxShape *Shape = nullptr;
    Path      Drawn;
    FillRule  Rule  = FillRule::NonZero;
    bool      Stroked = false;
    Stroke    Line;
};

void MakeTwin(Twin &T, std::uint32_t Kind)
{
    T.Shape                = &T.Room.Begin(Plain);
    const std::int32_t X   = Within(10, 200) * ONE + Within(0, ONE - 1);
    const std::int32_t Y   = Within(10, 150) * ONE + Within(0, ONE - 1);
    const std::int32_t W   = Within(ONE / 3, 90 * ONE), H = Within(ONE / 3, 70 * ONE);
    switch (Kind % 6u) {
    case 0:
        BoxShapeAddRect(T.Shape, X, Y, W, H, 0);
        T.Drawn.AddRect({PixelsOf(X), PixelsOf(Y), PixelsOf(W), PixelsOf(H)});
        break;
    case 1: {
        const std::int32_t R[4] = {Within(0, W), Within(0, H), Within(0, W / 2), Within(0, 30 * ONE)};
        BoxShapeAddRounded(T.Shape, X, Y, W, H, R, 0);
        T.Drawn.AddRoundedRect({PixelsOf(X), PixelsOf(Y), PixelsOf(W), PixelsOf(H)},
                               Corners{PixelsOf(R[0]), PixelsOf(R[1]), PixelsOf(R[2]), PixelsOf(R[3])});
        break;
    }
    case 2:
        BoxShapeAddEllipse(T.Shape, X, Y, W / 2 + 1, H / 2 + 1, 0);
        T.Drawn.AddEllipse({PixelsOf(X), PixelsOf(Y)}, PixelsOf(W / 2 + 1), PixelsOf(H / 2 + 1));
        break;
    case 3: {
        T.Rule        = Random() % 2u ? FillRule::EvenOdd : FillRule::NonZero;
        T.Shape->Rule = T.Rule == FillRule::EvenOdd ? BOX_SHAPE_EVENODD : BOX_SHAPE_NONZERO;
        BoxShapeMoveTo(T.Shape, X, Y);
        T.Drawn.MoveTo({PixelsOf(X), PixelsOf(Y)});
        for (int K = 0; K < 9; K++) {
            const std::int32_t PX = X + Within(-W, W), PY = Y + Within(-H, H);
            BoxShapeLineTo(T.Shape, PX, PY);
            T.Drawn.LineTo({PixelsOf(PX), PixelsOf(PY)});
        }
        T.Drawn.Close();
        break;
    }
    case 4: {
        const std::int32_t C[6] = {X + W, Y - H, X + 2 * W, Y + H, X, Y + 2 * H};
        BoxShapeMoveTo(T.Shape, X, Y);
        BoxShapeCubicTo(T.Shape, C[0], C[1], C[2], C[3], C[4], C[5]);
        BoxShapeQuadTo(T.Shape, X - W, Y + H, X, Y);
        T.Drawn.MoveTo({PixelsOf(X), PixelsOf(Y)})
            .CubicTo({PixelsOf(C[0]), PixelsOf(C[1])}, {PixelsOf(C[2]), PixelsOf(C[3])}, {PixelsOf(C[4]), PixelsOf(C[5])})
            .QuadTo({PixelsOf(X - W), PixelsOf(Y + H)}, {PixelsOf(X), PixelsOf(Y)});
        break;
    }
    default: {
        T.Stroked         = true;
        T.Line.Width      = PixelsOf(Within(ONE / 4, 12 * ONE));
        T.Line.Cap        = static_cast<LineCap>(Random() % 3u);
        T.Line.Join       = static_cast<LineJoin>(Random() % 3u);
        const std::int32_t Dash[2] = {Within(ONE, 20 * ONE), Within(ONE, 10 * ONE)};
        if (Random() % 2u) T.Line.Dashes = {PixelsOf(Dash[0]), PixelsOf(Dash[1])};
        BoxStroke K;
        BoxStrokeBegin(&K, T.Shape, static_cast<std::int32_t>(std::lround(T.Line.Width * ONE)),
                       static_cast<std::uint32_t>(T.Line.Cap), static_cast<std::uint32_t>(T.Line.Join));
        if (!T.Line.Dashes.empty()) BoxStrokeDash(&K, Dash, 2, 0);
        BoxStrokeMoveTo(&K, X, Y);
        T.Drawn.MoveTo({PixelsOf(X), PixelsOf(Y)});
        for (int I = 0; I < 5; I++) {
            const std::int32_t PX = X + Within(-W, W), PY = Y + Within(-H, H);
            BoxStrokeLineTo(&K, PX, PY);
            T.Drawn.LineTo({PixelsOf(PX), PixelsOf(PY)});
        }
        if (Random() % 2u) {
            BoxStrokeClose(&K);
            T.Drawn.Close();
        }
        BoxStrokeEnd(&K);
    }
    }
}

/* The engine's coverage of a canvas filled with opaque white from clear: its
 * opacity, which is the coverage times 257 exactly. */
std::vector<std::uint16_t> EngineCoverage(Twin &T, std::int32_t Width, std::int32_t Height)
{
    std::optional<Canvas> C = Canvas::Make(static_cast<std::uint32_t>(Width), static_cast<std::uint32_t>(Height));
    if (T.Stroked) C->Stroke(T.Drawn, T.Line, Fill(Colors::White));
    else           C->Fill(T.Drawn, Fill(Colors::White), {}, T.Rule);
    std::vector<std::uint16_t> Alpha(static_cast<std::size_t>(Width) * Height);
    for (std::int32_t Y = 0; Y < Height; Y++)
        for (std::int32_t X = 0; X < Width; X++) Alpha[static_cast<std::size_t>(Y) * Width + X] = C->Row(Y)[X].Alpha;
    return Alpha;
}

std::string Said(const char *Format, double A, double B = 0)
{
    char Line[240];
    std::snprintf(Line, sizeof(Line), Format, A, B);
    return Line;
}

/* A brush canvas of one colour, with the row room the brush sweeps in. */
struct Board {
    std::vector<std::uint32_t> Pixels;
    std::vector<std::int32_t>  Row;
    BoxBrushCanvas             Canvas{};

    Board(std::uint32_t Width, std::uint32_t Height, std::uint32_t Color)
        : Pixels(static_cast<std::size_t>(Width) * Height, Color & 0xFFFFFFu), Row(Width + 2u)
    {
        Canvas = BoxBrushCanvas{Pixels.data(), Width, Height, Width, BoxRect{0, 0, static_cast<std::int32_t>(Width), static_cast<std::int32_t>(Height)}, Row.data()};
    }
};

std::uint32_t CodeAt(std::uint32_t Rgb, int Channel) { return (Rgb >> (16 - 8 * Channel)) & 0xFFu; }

}  // namespace

void CheckEngine(Tally &Count)
{
    /* Coverage: the engine's the brush's, pixel for pixel. */
    {
        std::uint32_t Shapes = 0, Wrong = 0;
        for (std::uint32_t I = 0; I < 1200u; I++) {
            Twin T;
            MakeTwin(T, I);
            const Coverage                   Brush  = CoverageOf(*T.Shape, 320, 240);
            const std::vector<std::uint16_t> Engine = EngineCoverage(T, 320, 240);
            Shapes++;
            for (std::size_t P = 0; P < Engine.size(); P++)
                if (Engine[P] != Brush.Shares[P] * 257u) {
                    Wrong++;
                    break;
                }
        }
        Count.Check(Wrong == 0, Said("the engine covers every pixel as the brush does: %.0f shapes of every kind, filled "
                                     "and stroked, %.0f not alike",
                                     Shapes, Wrong));
    }

    /* Colour: one layer within a code of the exact sum — its light read
     * through the system's one table of numbers (BoxColorCode). The brush is
     * said beside it, not held to it: its share of 255 lays a colour of
     * little opacity at a soft edge coarser than the engine's sixteen bits,
     * most where dark numbers lie close in light. */
    {
        std::uint32_t Channels = 0, OffExact = 0, OffBrush = 0, WorstExact = 0, WorstBrush = 0, BrushOff = 0;
        for (std::uint32_t I = 0; I < 300u; I++) {
            const std::uint32_t Paper = Random() & 0xFFFFFFu, Ink = Random();
            Twin                T;
            MakeTwin(T, I % 5u);
            Board B(320, 240, Paper);
            const BoxPaint P = {BOX_PAINT_SOLID, Ink, nullptr, 0, Plain, nullptr, 0, 0, 0, BOX_PAINT_PAD, 0, 0, 0};
            BoxBrushFill(&B.Canvas, T.Shape, &P, 255);
            std::optional<Canvas> C = Canvas::Make(320, 240);
            C->Clear(Color::FromRaw(Paper));
            C->Fill(T.Drawn, Fill(Color::Rgba(Ink >> 16, Ink >> 8, Ink, Ink >> 24)), {}, T.Rule);
            std::vector<std::uint32_t> Out(320u * 240u);
            C->CopyOut(Rect{0, 0, 320, 240}, Out.data(), 320, BOX_PIXEL_BGR, Dither::Off);
            const Coverage Cov = CoverageOf(*T.Shape, 320, 240);
            for (std::size_t K = 0; K < Out.size(); K++) {
                if (!Cov.Shares[K]) continue;
                const double Share = Cov.Shares[K] / 255.0 * (Ink >> 24) / 255.0;
                for (int Ch = 0; Ch < 3; Ch++) {
                    Channels++;
                    const double Exact = BoxColorLightOf[CodeAt(Paper, Ch)] * (1 - Share) +
                                         BoxColorLightOf[CodeAt(Ink, Ch)] * Share;
                    const std::uint32_t Want = BoxColorCode(static_cast<std::uint32_t>(std::lround(Exact)));
                    const std::uint32_t Got = CodeAt(Out[K], Ch), Brushed = CodeAt(B.Pixels[K], Ch);
                    const std::uint32_t DE = Got > Want ? Got - Want : Want - Got;
                    const std::uint32_t DB = Got > Brushed ? Got - Brushed : Brushed - Got;
                    const std::uint32_t BE = Brushed > Want ? Brushed - Want : Want - Brushed;
                    OffExact += DE != 0;
                    OffBrush += DB != 0;
                    BrushOff += BE > 1;
                    WorstExact = std::max(WorstExact, DE);
                    WorstBrush = std::max(WorstBrush, DB);
                }
            }
        }
        char Line[300];
        std::snprintf(Line, sizeof(Line),
                      "a colour laid over another, %u channels of 300 shapes' pixels: the exact sum's number but %u off "
                      "by %u at most; the brush's but %u, off by %u at most, the brush itself %u times two or more off "
                      "the exact",
                      Channels, OffExact, WorstExact, OffBrush, WorstBrush, BrushOff);
        Count.Check(WorstExact <= 1, Line);
    }

    /* Sixteen bits: a wide dark gradient, dithered, keeps no bands — each
     * column's mean within a fifth of a code of the exact light; the brush,
     * in eight, shows its steps. */
    {
        const std::int32_t    W = 1024, H = 64;
        std::optional<Canvas> C = Canvas::Make(W, H);
        C->Fill(Path::Rect({0, 0, W, H}),
                Fill(Gradient::Linear({0, 0}, {W, 0}, {{0, Color::Rgb(16, 16, 16)}, {1, Color::Rgb(24, 24, 24)}})));
        std::vector<std::uint32_t> Out(static_cast<std::size_t>(W) * H);
        C->CopyOut(Rect{0, 0, W, H}, Out.data(), W, BOX_PIXEL_BGR, Dither::On);
        double Worst = 0;
        for (std::int32_t X = 8; X < W - 8; X++) {
            double Light = 0;
            for (std::int32_t Y = 0; Y < H; Y++)
                for (std::int32_t K = -8; K < 8; K++) Light += BoxColorLightOf[Out[static_cast<std::size_t>(Y) * W + X + K] & 0xFF];
            Light /= H * 16.0;
            const double Code  = 16.0 + 8.0 * (X + 0.5) / W;
            const double Exact = BoxColorLightOf[(int)Code] + (BoxColorLightOf[(int)Code + 1] - BoxColorLightOf[(int)Code]) * (Code - (int)Code);
            Worst = std::max(Worst, std::fabs(Light - Exact) / (BoxColorLightOf[(int)Code + 1] - BoxColorLightOf[(int)Code]));
        }
        std::vector<std::uint32_t> Plainly(static_cast<std::size_t>(W) * H);
        C->CopyOut(Rect{0, 0, W, H}, Plainly.data(), W, BOX_PIXEL_BGR, Dither::Off);
        std::uint32_t Steps = 0;
        for (std::int32_t X = 1; X < W; X++) Steps += (Plainly[X] & 0xFF) != (Plainly[X - 1] & 0xFF);
        std::vector<std::uint32_t> Again(Out.size());
        C->CopyOut(Rect{0, 0, W, H}, Again.data(), W, BOX_PIXEL_BGR, Dither::On);
        Count.Check(Worst < 0.2 && Steps == 8 && Again == Out,
                    Said("a gradient of eight dark numbers across 1024: dithered, every column's light within %.2f of a "
                         "number of the exact (undithered, its %.0f steps); the pattern the same each time",
                         Worst, Steps));
    }

    /* Several fills on one shape: each laid as if alone, one after another. */
    {
        const Path  Shape = Path::RoundedRect({20.5f, 10.25f, 150, 90}, Corners::All(24));
        const Fill  Both[2] = {Fill(Color::Rgb(200, 40, 90)),
                               Fill(Gradient::Radial({90, 50}, 80, {{0, Color::Rgba(255, 255, 255, 200)}, {1, Color::Rgba(0, 0, 80, 40)}}))};
        std::optional<Canvas> A = Canvas::Make(200, 120), B = Canvas::Make(200, 120);
        A->Clear(Color::Rgb(10, 20, 30));
        B->Clear(Color::Rgb(10, 20, 30));
        A->Fill(Shape, std::span<const Fill>(Both, 2));
        B->Fill(Shape, Both[0]);
        B->Fill(Shape, Both[1]);
        bool Same = true;
        for (std::uint32_t Y = 0; Y < 120u; Y++) Same = Same && std::memcmp(A->Row(Y), B->Row(Y), 200 * sizeof(Pixel)) == 0;
        Count.Check(Same, "a shape of two fills is the shape filled twice, its coverage counted once");
    }

    /* A mask: its coverage the shape's; filled, the shape filled. */
    {
        const Path                Shape = Path::Circle({60.5f, 50}, 37.25f);
        const std::optional<Mask> M     = Mask::Of(Shape, {}, FillRule::NonZero, Rect{0, 0, 128, 100});
        std::optional<Canvas>     A = Canvas::Make(128, 100), B = Canvas::Make(128, 100);
        A->Fill(Shape, Fill(Color::Rgb(250, 200, 10)));
        B->FillMask(*M, Fill(Color::Rgb(250, 200, 10)));
        bool Same = true;
        for (std::uint32_t Y = 0; Y < 100u; Y++) Same = Same && std::memcmp(A->Row(Y), B->Row(Y), 128 * sizeof(Pixel)) == 0;
        Count.Check(M && Same && A->Changed() == B->Changed(), "a mask filled is its shape filled, pixel for pixel");
    }

    /* Clips: a rectangle keeps every draw within it; a circle softly; popped,
     * as before. */
    {
        std::optional<Canvas> C = Canvas::Make(100, 100);
        C->PushClip(Rect{10, 20, 30, 40});
        C->Fill(Path::Rect({0, 0, 100, 100}), Fill(Colors::White));
        bool Kept = true;
        for (std::uint32_t Y = 0; Y < 100u; Y++)
            for (std::uint32_t X = 0; X < 100u; X++) {
                const bool In = X >= 10 && X < 40 && Y >= 20 && Y < 60;
                Kept = Kept && (C->Row(Y)[X].Alpha == (In ? 65535 : 0));
            }
        C->PopClip();
        C->PushClip(Path::Circle({50, 50}, 30));
        C->Clear(Rect{0, 0, 100, 100}, Color::Through());
        C->Fill(Path::Rect({0, 0, 100, 100}), Fill(Colors::White));
        const std::optional<Mask> Circle = Mask::Of(Path::Circle({50, 50}, 30), {}, FillRule::NonZero, Rect{0, 0, 100, 100});
        bool Soft = true;
        for (std::uint32_t Y = 0; Y < 100u; Y++)
            for (std::uint32_t X = 0; X < 100u; X++) Soft = Soft && C->Row(Y)[X].Alpha == Circle->At(X, Y) * 257u;
        C->PopClip();
        C->PopClip();
        Count.Check(Kept && Soft && C->Clip() == (Rect{0, 0, 100, 100}),
                    "a clip keeps every draw within it, a round one as softly as a fill's edge; popped, the canvas whole");
    }

    /* Strokes aligned to their shape: across its edge, within, without. */
    {
        const Path Box = Path::Rect({20, 20, 60, 40});
        double     Areas[3];
        for (int K = 0; K < 3; K++) {
            std::optional<Canvas> C = Canvas::Make(120, 100);
            Stroke Line;
            Line.Width = 6;
            Line.Align = static_cast<StrokeAlign>(K);
            C->Stroke(Box, Line, Fill(Colors::White));
            double Sum = 0;
            for (std::uint32_t Y = 0; Y < 100u; Y++)
                for (std::uint32_t X = 0; X < 120u; X++) Sum += C->Row(Y)[X].Alpha / 65535.0;
            Areas[K] = Sum;
        }
        Count.Check(Areas[0] == 66.0 * 46.0 - 54.0 * 34.0 && Areas[1] == 60.0 * 40.0 - 48.0 * 28.0 &&
                        Areas[2] == 72.0 * 52.0 - 60.0 * 40.0,
                    Said("a rectangle stroked 6 wide: across its edge %.0f, within it %.0f, without — each its ring",
                         Areas[0], Areas[1]));
    }

    /* Pictures: laid one to one, the very colours; four times larger and
     * sharp, each pixel a square; drawn an eighth of its size, the light of
     * its pattern, not a shimmer. */
    {
        std::vector<std::uint8_t> Rgba(16u * 16u * 4u);
        for (std::size_t I = 0; I < Rgba.size(); I++) Rgba[I] = static_cast<std::uint8_t>(I % 4u == 3u ? 255 : Random());
        const auto            Shown = Picture::FromRgba(Rgba, 16, 16, 64);
        std::optional<Canvas> C     = Canvas::Make(64, 64);
        C->Fill(Path::Rect({0, 0, 16, 16}), Fill::Picture(Shown));
        std::vector<std::uint32_t> Out(64u * 64u);
        C->CopyOut(Rect{0, 0, 64, 64}, Out.data(), 64, BOX_PIXEL_BGR, Dither::Off);
        bool Same = true;
        for (std::uint32_t Y = 0; Y < 16u; Y++)
            for (std::uint32_t X = 0; X < 16u; X++) {
                const std::uint8_t *P = &Rgba[(Y * 16u + X) * 4u];
                Same = Same && Out[Y * 64u + X] == (static_cast<std::uint32_t>(P[0]) << 16 | P[1] << 8 | P[2]);
            }
        C->Clear(Color::Through());
        C->Fill(Path::Rect({0, 0, 64, 64}), Fill::Picture(Shown, Transform::Scale(4), Extend::None, true));
        bool Squares = true;
        for (std::uint32_t Y = 0; Y < 64u; Y++)
            for (std::uint32_t X = 0; X < 64u; X++) Squares = Squares && C->Row(Y)[X] == C->Row(Y & ~3u)[X & ~3u];
        std::vector<std::uint8_t> Checker(256u * 256u * 4u);
        for (std::uint32_t I = 0; I < 256u * 256u; I++) {
            const std::uint8_t V = ((I % 256u) + (I / 256u)) % 2u ? 255 : 0;
            Checker[I * 4u] = Checker[I * 4u + 1] = Checker[I * 4u + 2] = V;
            Checker[I * 4u + 3]                                        = 255;
        }
        const auto            Fine  = Picture::FromRgba(Checker, 256, 256, 1024);
        std::optional<Canvas> Small = Canvas::Make(32, 32);
        Small->Fill(Path::Rect({0, 0, 32, 32}), Fill::Picture(Fine, Transform::Scale(0.125f)));
        std::vector<std::uint32_t> Seen(32u * 32u);
        Small->CopyOut(Rect{0, 0, 32, 32}, Seen.data(), 32, BOX_PIXEL_BGR, Dither::Off);
        std::uint32_t Least = 255, Most = 0;
        for (std::uint32_t I = 0; I < 32u * 32u; I++) Least = std::min(Least, Seen[I] & 0xFFu), Most = std::max(Most, Seen[I] & 0xFFu);
        Count.Check(Same && Squares && Least >= 186 && Most <= 190,
                    Said("a picture one to one its very colours; sharp and four times larger, squares; a checker an "
                         "eighth its size, its light — %.0f to %.0f, not a shimmer",
                         Least, Most));
    }

    /* SVG's path data. */
    {
        const auto Square = Path::FromSvg("M10 10 H 70 v60 L10,70 z");
        const auto Round  = Path::FromSvg("M 50 10 A 40 40 0 1 1 50 90 A 40 40 0 1 1 50 10 Z");
        const auto Broken = Path::FromSvg("M 10 10 L 20");
        std::optional<Canvas> C = Canvas::Make(100, 100);
        C->Fill(*Square, Fill(Colors::White));
        double A = 0;
        for (std::uint32_t Y = 0; Y < 100u; Y++)
            for (std::uint32_t X = 0; X < 100u; X++) A += C->Row(Y)[X].Alpha / 65535.0;
        C->Clear(Color::Through());
        C->Fill(*Round, Fill(Colors::White));
        double B = 0;
        for (std::uint32_t Y = 0; Y < 100u; Y++)
            for (std::uint32_t X = 0; X < 100u; X++) B += C->Row(Y)[X].Alpha / 65535.0;
        Count.Check(Square && Round && !Broken && A == 3600.0 && Near(B, M_PI * 1600.0, 0.002),
                    Said("SVG's path data read: a square of relative and absolute steps %.0f, a circle of two arcs %.1f; "
                         "a broken path refused",
                         A, B));
    }

    /* Refused whole, nothing drawn: sizes past a canvas's, a place past a
     * shape's reach, a transform stretching too far or not a number, a
     * gradient with no stops. */
    {
        std::optional<Canvas> C = Canvas::Make(50, 50);
        const bool Refused = !Canvas::Make(0, 10) && !Canvas::Make(40000, 1) &&
                             !C->Fill(Path::Rect({0, 0, 1e7f, 10}), Fill(Colors::White)) &&
                             !C->Fill(Path::Rect({0, 0, 10, 10}), Fill(Colors::White), Transform::Scale(1000)) &&
                             !C->Fill(Path::Rect({0, 0, 10, 10}), Fill(Colors::White), Transform{NAN, 0, 0, 1, 0, 0}) &&
                             !C->Fill(Path::Rect({0, 0, 10, 10}), Fill(Gradient::Linear({0, 0}, {10, 0}, {}))) &&
                             !C->Stroke(Path::Line({0, 10}, {30000, 10}), Stroke{1, LineCap::Butt, LineJoin::Miter, 4, {0.01f, 0.01f}},
                                        Fill(Colors::White));
        Count.Check(Refused && C->Changed().Empty(), "refused whole, and nothing drawn: what a canvas or a shape cannot take — dashes of a hair along 30000 pixels too");
    }

    /* Pixels another hand paints, light past opacity among them, laid where
     * they lie: as the very canvas taken from them, held, is laid — whole,
     * faded, under a round clip, partly off the canvas. */
    {
        std::uint32_t Laid = 0, Wrong = 0;
        for (std::uint32_t Round = 0; Round < 200u; Round++) {
            const std::uint32_t Wide = 1 + Random() % 90, High = 1 + Random() % 60, Stride = Wide + Random() % 7;
            std::vector<Pixel>  Painted(static_cast<std::size_t>(Stride) * High);
            for (Pixel &Each : Painted)
                Each = Pixel{static_cast<std::uint16_t>(Random()), static_cast<std::uint16_t>(Random()),
                             static_cast<std::uint16_t>(Random()), static_cast<std::uint16_t>(Random() % 3 == 0 ? 65535u : Random())};
            std::optional<Canvas> Taken = Canvas::Make(Wide, High), Got = Canvas::Make(96, 72), Want = Canvas::Make(96, 72);
            Taken->TakePixels(Rect{0, 0, static_cast<std::int32_t>(Wide), static_cast<std::int32_t>(High)}, Painted.data(), Stride);
            const Color Ground = Color::Rgba(static_cast<std::uint8_t>(Random()), static_cast<std::uint8_t>(Random()), 90,
                                             static_cast<std::uint8_t>(Random()));
            Got->Clear(Ground);
            Want->Clear(Ground);
            if (Round % 3 == 1) {
                Got->PushClip(Path::Circle({48, 36}, 30));
                Want->PushClip(Path::Circle({48, 36}, 30));
            }
            const Point At{Within(-40, 80), Within(-30, 60)};
            const float Opacity = Round % 4 == 0 ? 1.0f : static_cast<float>(Random() % 1000) / 1000.0f;
            Got->Draw(Foreign{Painted.data(), Wide, High, Stride}, At, Opacity);
            Want->Draw(*Taken, At, Opacity);
            Laid++;
            for (std::uint32_t Y = 0; Y < 72u; Y++)
                if (std::memcmp(Got->Row(Y), Want->Row(Y), 96 * sizeof(Pixel)) != 0 || Got->Changed() != Want->Changed()) {
                    Wrong++;
                    break;
                }
        }
        Count.Check(Wrong == 0, Said("pixels another hand paints laid as the canvas taken from them, held, is laid: %.0f of every "
                                     "size, faded, clipped round, partly off the canvas, %.0f not alike",
                                     Laid, Wrong));
    }
}

}  // namespace stand
