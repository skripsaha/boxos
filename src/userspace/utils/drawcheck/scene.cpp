/*
 * scene.cpp — a scene of every kind of thing the engine draws. See
 * drawcheck.h.
 */

#include <memory>
#include <optional>
#include <vector>

#include "drawcheck.h"

namespace drawcheck {

namespace {

using namespace painter::core;
/* BoxLib's C colour type is named Color too: Painter's is meant here. */
using painter::core::Color;

class Dice {
public:
    explicit Dice(std::uint32_t Seed) : State(Seed * 2654435761u + 1u) {}
    std::uint32_t Next()
    {
        State ^= State << 13;
        State ^= State >> 17;
        State ^= State << 5;
        return State;
    }
    float         Within(float Low, float High) { return Low + (High - Low) * static_cast<float>(Next() % 10001u) / 10000.0f; }
    painter::core::Color Any() { return Color::Rgba(Next() & 255u, Next() & 255u, Next() & 255u, 40u + Next() % 216u); }

private:
    std::uint32_t State;
};

Path AnyShape(Dice &Roll)
{
    const float X = Roll.Within(0, 560), Y = Roll.Within(0, 340), W = Roll.Within(2, 160), H = Roll.Within(2, 120);
    switch (Roll.Next() % 5u) {
    case 0: return Path::Rect({X, Y, W, H});
    case 1: return Path::RoundedRect({X, Y, W, H}, Corners{Roll.Within(0, 40), Roll.Within(0, 40), Roll.Within(0, 40), Roll.Within(0, 40)});
    case 2: return Path::Ellipse({X, Y}, W / 2, H / 2);
    case 3: {
        Path P;
        P.MoveTo({X, Y});
        for (int K = 0; K < 6; K++) P.LineTo({X + Roll.Within(-W, W), Y + Roll.Within(-H, H)});
        return P.Close();
    }
    default:
        return Path()
            .MoveTo({X, Y})
            .CubicTo({X + W, Y - H}, {X + 2 * W, Y + H}, {X + W, Y + 2 * H})
            .QuadTo({X - W, Y + H}, {X, Y})
            .Close();
    }
}

Fill AnyFill(Dice &Roll, const std::shared_ptr<const Picture> &Shown)
{
    const Extend Past = static_cast<Extend>(Roll.Next() % 4u);
    std::vector<GradientStop> Stops = {{0, Roll.Any()}, {Roll.Within(0.2f, 0.8f), Roll.Any()}, {1, Roll.Any()}};
    switch (Roll.Next() % 6u) {
    case 0:
    case 1: return Fill(Roll.Any());
    case 2: return Fill(Gradient::Linear({Roll.Within(0, 640), Roll.Within(0, 400)}, {Roll.Within(0, 640), Roll.Within(0, 400)}, Stops, Past));
    case 3: return Fill(Gradient::Radial({Roll.Within(0, 640), Roll.Within(0, 400)}, Roll.Within(10, 300), Stops, Past));
    case 4: return Fill(Gradient::Conic({Roll.Within(0, 640), Roll.Within(0, 400)}, Roll.Within(0, 360), Stops));
    default:
        return Fill::Picture(Shown, Transform::Scale(Roll.Within(0.05f, 6)).Then(Transform::Rotate(Roll.Within(0, 90))),
                             Past, Roll.Next() % 2u == 0)
            .WithOpacity(Roll.Within(0.3f, 1));
    }
}

}  // namespace

void DrawScene(Canvas &On, std::uint32_t Seed)
{
    Dice Roll(Seed);
    std::vector<std::uint8_t> Rgba(37u * 23u * 4u);
    for (std::uint8_t &Byte : Rgba) Byte = static_cast<std::uint8_t>(Roll.Next());
    const auto Shown = Picture::FromRgba(Rgba, 37, 23, 37u * 4u);
    On.Clear(Roll.Any());
    for (int K = 0; K < 40; K++) {
        const Path Shape = AnyShape(Roll);
        const Fill Fills[2] = {AnyFill(Roll, Shown), AnyFill(Roll, Shown)};
        if (Roll.Next() % 3u == 0) {
            Stroke Line;
            Line.Width = Roll.Within(0.3f, 14);
            Line.Cap   = static_cast<LineCap>(Roll.Next() % 3u);
            Line.Join  = static_cast<LineJoin>(Roll.Next() % 3u);
            Line.Align = static_cast<StrokeAlign>(Roll.Next() % 3u);
            if (Roll.Next() % 2u) Line.Dashes = {Roll.Within(0, 20), Roll.Within(1, 20)};
            On.Stroke(Shape, Line, std::span<const Fill>(Fills, 1 + Roll.Next() % 2u));
        } else {
            On.Fill(Shape, std::span<const Fill>(Fills, 1 + Roll.Next() % 2u), {},
                    Roll.Next() % 2u ? FillRule::EvenOdd : FillRule::NonZero, Roll.Within(0.2f, 1));
        }
    }
    Shadow Cast;
    Cast.Offset = {Roll.Within(-8, 8), Roll.Within(-8, 8)};
    Cast.Blur   = Roll.Within(0, 20);
    Cast.Spread = Roll.Within(-4, 4);
    Cast.Inner  = Roll.Next() % 2u;
    const Path Card = AnyShape(Roll);
    if (const std::optional<Mask> Kept = Cast.MaskFor(Card)) On.DrawShadow(*Kept, Cast);
    On.PushClip(AnyShape(Roll));
    On.Fill(AnyShape(Roll), AnyFill(Roll, Shown));
    On.Blur(Rect{static_cast<std::int32_t>(Roll.Next() % 400u), static_cast<std::int32_t>(Roll.Next() % 200u), 200, 150}, Roll.Within(1, 12));
    On.PopClip();
    if (std::optional<Canvas> Small = Canvas::Make(120, 90)) {
        Small->Fill(AnyShape(Roll), AnyFill(Roll, Shown), Transform::Scale(0.3f));
        On.Draw(*Small, Point{static_cast<std::int32_t>(Roll.Next() % 520u), static_cast<std::int32_t>(Roll.Next() % 310u)}, Roll.Within(0.2f, 1));
    }
}

}  // namespace drawcheck
