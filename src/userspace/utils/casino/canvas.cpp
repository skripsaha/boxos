/* canvas.cpp — the casino's shapes (canvas.h). */

#include "canvas.h"

#include <algorithm>

#include "trig.h"

namespace casino {

namespace {

std::uint8_t Weight(float Cover)
{
    return static_cast<std::uint8_t>(std::clamp(Cover, 0.0f, 1.0f) * 255.0f + 0.5f);
}

}  // namespace

Color Shade(Color Ink, float Factor)
{
    if (Factor <= 1.0f) return Color::Mix(painter::core::Colors::Black, Ink, Weight(Factor));
    return Color::Mix(Ink, painter::core::Colors::White, Weight(Factor - 1.0f));
}

void Canvas::Plot(std::int32_t X, std::int32_t Y, Color Ink, float Cover)
{
    if (Edged) {
        if (Cover >= 0.5f) Out.PutPixel({X, Y}, Ink);
        return;
    }
    if (Cover >= 0.99f) Out.PutPixel({X, Y}, Ink);
    else if (Cover > 0.0f) Out.BlendPixel({X, Y}, Ink, Weight(Cover));
}

void Canvas::Fill(std::int32_t X, std::int32_t Y, std::int32_t Across, std::int32_t Down, Color Ink)
{
    const std::int32_t Left = std::max(X, 0), Top = std::max(Y, 0);
    const std::int32_t Right = std::min(X + Across, Wide()), Bottom = std::min(Y + Down, High());
    for (std::int32_t Row = Top; Row < Bottom; Row++) {
        // Two pixel rows that make a whole cell are one cell of a single color.
        const bool Whole = (Row & 1) == 0 && Row + 1 < Bottom;
        for (std::int32_t Col = Left; Col < Right; Col++) {
            if (Whole) Out.Put({Col, Row / 2}, painter::tui::Cell{' ', Ink, Ink});
            else Out.PutPixel({Col, Row}, Ink);
        }
        if (Whole) Row++;
    }
}

void Canvas::Disc(float Across, float Down, float Radius, Color Ink, float Opacity)
{
    Fine.Disc(Across, Down, Radius, Ink, Opacity);
}

void Canvas::Ellipse(float Across, float Down, float ReachAcross, float ReachDown, Color Ink, float Opacity)
{
    Fine.Ellipse(Across, Down, ReachAcross, ReachDown, Ink, Opacity);
}

void Canvas::Ring(float Across, float Down, float Outer, float Inner, Color Ink)
{
    Fine.Ring(Across, Down, Outer, Inner, Ink);
}

void Canvas::Rounded(float Left, float Top, float Across, float Down, float Radius, Color Ink, float Opacity)
{
    Fine.Rounded(Left, Top, Across, Down, Radius, Ink, Opacity);
}

void Canvas::Line(float FromAcross, float FromDown, float ToAcross, float ToDown, float Thick, Color Ink)
{
    Fine.Line({FromAcross, FromDown}, {ToAcross, ToDown}, Thick, Ink);
}

void Canvas::Chip(float Across, float Down, float Radius, Color Body)
{
    Disc(Across, Down, Radius, Shade(Body, 0.55f));
    Disc(Across, Down, Radius * 0.82f, Body);
    if (Radius < 3.0f) return;
    Ring(Across, Down, Radius * 0.62f, Radius * 0.5f, Shade(Body, 1.55f));
    for (int Stripe = 0; Stripe < 6; Stripe++) {
        const float Angle = Stripe * Pi / 3.0f;
        const float Edge = Radius * 0.88f;
        Disc(Across + Sine(Angle) * Edge, Down - Cosine(Angle) * Edge, std::max(Radius * 0.14f, 0.6f),
             Shade(Body, 1.7f));
    }
}

}  // namespace casino
