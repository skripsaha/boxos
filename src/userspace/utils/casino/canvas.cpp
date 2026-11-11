/* canvas.cpp — the casino's shapes (canvas.h). */

#include "canvas.h"

#include <algorithm>
#include <cmath>

#include "trig.h"

namespace casino {

namespace {

std::uint8_t Weight(float Cover)
{
    return static_cast<std::uint8_t>(std::clamp(Cover, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// How much of a pixel whose center is Beyond past an edge lies inside it.
float Inside(float Beyond)
{
    return std::clamp(0.5f - Beyond, 0.0f, 1.0f);
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
    const std::int32_t Left = std::max(static_cast<std::int32_t>(Across - Radius - 1), 0);
    const std::int32_t Top = std::max(static_cast<std::int32_t>(Down - Radius - 1), 0);
    const std::int32_t Right = std::min(static_cast<std::int32_t>(Across + Radius + 2), Wide());
    const std::int32_t Bottom = std::min(static_cast<std::int32_t>(Down + Radius + 2), High());
    for (std::int32_t Row = Top; Row < Bottom; Row++) {
        for (std::int32_t Col = Left; Col < Right; Col++) {
            const float Across2 = Col + 0.5f - Across, Down2 = Row + 0.5f - Down;
            Plot(Col, Row, Ink, Inside(std::sqrt(Across2 * Across2 + Down2 * Down2) - Radius) * Opacity);
        }
    }
}

void Canvas::Ellipse(float Across, float Down, float ReachAcross, float ReachDown, Color Ink, float Opacity)
{
    const std::int32_t Left = std::max(static_cast<std::int32_t>(Across - ReachAcross - 1), 0);
    const std::int32_t Top = std::max(static_cast<std::int32_t>(Down - ReachDown - 1), 0);
    const std::int32_t Right = std::min(static_cast<std::int32_t>(Across + ReachAcross + 2), Wide());
    const std::int32_t Bottom = std::min(static_cast<std::int32_t>(Down + ReachDown + 2), High());
    const float Reach = std::max(std::min(ReachAcross, ReachDown), 0.5f);
    for (std::int32_t Row = Top; Row < Bottom; Row++) {
        for (std::int32_t Col = Left; Col < Right; Col++) {
            const float Over = (Col + 0.5f - Across) / std::max(ReachAcross, 0.5f);
            const float Under = (Row + 0.5f - Down) / std::max(ReachDown, 0.5f);
            Plot(Col, Row, Ink, Inside((std::sqrt(Over * Over + Under * Under) - 1.0f) * Reach) * Opacity);
        }
    }
}

void Canvas::Ring(float Across, float Down, float Outer, float Inner, Color Ink)
{
    const std::int32_t Left = std::max(static_cast<std::int32_t>(Across - Outer - 1), 0);
    const std::int32_t Top = std::max(static_cast<std::int32_t>(Down - Outer - 1), 0);
    const std::int32_t Right = std::min(static_cast<std::int32_t>(Across + Outer + 2), Wide());
    const std::int32_t Bottom = std::min(static_cast<std::int32_t>(Down + Outer + 2), High());
    for (std::int32_t Row = Top; Row < Bottom; Row++) {
        for (std::int32_t Col = Left; Col < Right; Col++) {
            const float Across2 = Col + 0.5f - Across, Down2 = Row + 0.5f - Down;
            const float Far     = std::sqrt(Across2 * Across2 + Down2 * Down2);
            Plot(Col, Row, Ink, std::min(Inside(Far - Outer), Inside(Inner - Far)));
        }
    }
}

void Canvas::Rounded(float Left, float Top, float Across, float Down, float Radius, Color Ink, float Opacity)
{
    const float HalfAcross = Across / 2.0f, HalfDown = Down / 2.0f;
    const float MiddleAcross = Left + HalfAcross, MiddleDown = Top + HalfDown;
    const std::int32_t From = std::max(static_cast<std::int32_t>(std::floor(Left)) - 1, 0);
    const std::int32_t Up = std::max(static_cast<std::int32_t>(std::floor(Top)) - 1, 0);
    const std::int32_t To = std::min(static_cast<std::int32_t>(std::ceil(Left + Across)) + 1, Wide());
    const std::int32_t Bottom = std::min(static_cast<std::int32_t>(std::ceil(Top + Down)) + 1, High());
    for (std::int32_t Row = Up; Row < Bottom; Row++) {
        for (std::int32_t Col = From; Col < To; Col++) {
            const float OverAcross = std::fabs(Col + 0.5f - MiddleAcross) - (HalfAcross - Radius);
            const float OverDown   = std::fabs(Row + 0.5f - MiddleDown) - (HalfDown - Radius);
            const float Beyond = std::sqrt(std::max(OverAcross, 0.0f) * std::max(OverAcross, 0.0f) +
                                           std::max(OverDown, 0.0f) * std::max(OverDown, 0.0f)) +
                                 std::min(std::max(OverAcross, OverDown), 0.0f) - Radius;
            Plot(Col, Row, Ink, Inside(Beyond) * Opacity);
        }
    }
}

void Canvas::Line(float FromAcross, float FromDown, float ToAcross, float ToDown, float Thick, Color Ink)
{
    const float Along = ToAcross - FromAcross, Down = ToDown - FromDown;
    const float Length2 = Along * Along + Down * Down;
    const float Half = Thick / 2.0f;
    const std::int32_t Left = std::max(static_cast<std::int32_t>(std::min(FromAcross, ToAcross) - Half - 1), 0);
    const std::int32_t Top = std::max(static_cast<std::int32_t>(std::min(FromDown, ToDown) - Half - 1), 0);
    const std::int32_t Right = std::min(static_cast<std::int32_t>(std::max(FromAcross, ToAcross) + Half + 2), Wide());
    const std::int32_t Bottom = std::min(static_cast<std::int32_t>(std::max(FromDown, ToDown) + Half + 2), High());
    for (std::int32_t Row = Top; Row < Bottom; Row++) {
        for (std::int32_t Col = Left; Col < Right; Col++) {
            const float PointAcross = Col + 0.5f - FromAcross, PointDown = Row + 0.5f - FromDown;
            const float Reach = Length2 > 0 ? std::clamp((PointAcross * Along + PointDown * Down) / Length2, 0.0f, 1.0f) : 0.0f;
            const float OffAcross = PointAcross - Reach * Along, OffDown = PointDown - Reach * Down;
            Plot(Col, Row, Ink, Inside(std::sqrt(OffAcross * OffAcross + OffDown * OffDown) - Half));
        }
    }
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
