/* canvas.cpp — the canvas's rectangles, lines and glows, and the sprite's edge. */

#include "canvas.h"

#include <cstdlib>

namespace wizard {

void Canvas::Fill(int X, int Y, int Wide, int High, Color Ink)
{
    const int Left = std::max(0, X), Right = std::min(Width, X + Wide);
    const int Top = std::max(0, Y), Bottom = std::min(Height, Y + High);
    for (int Row = Top; Row < Bottom; Row++)
        for (int Column = Left; Column < Right; Column++) At(Column, Row) = Ink;
}

void Canvas::Line(int X0, int Y0, int X1, int Y1, Color Ink)
{
    const int Steps = std::max(std::abs(X1 - X0), std::abs(Y1 - Y0));
    for (int Step = 0; Step <= Steps; Step++) {
        const float T = Steps == 0 ? 0.0f : static_cast<float>(Step) / static_cast<float>(Steps);
        Put(X0 + static_cast<int>(std::lround(static_cast<float>(X1 - X0) * T)),
            Y0 + static_cast<int>(std::lround(static_cast<float>(Y1 - Y0) * T)), Ink);
    }
}

void Canvas::Glow(float X, float Y, float Radius, Color Ink, int Strength)
{
    const int Left = static_cast<int>(X - Radius), Right = static_cast<int>(X + Radius) + 1;
    const int Top = static_cast<int>(Y - Radius), Bottom = static_cast<int>(Y + Radius) + 1;
    const float Inverse = 1.0f / (Radius * Radius);
    for (int Row = std::max(0, Top); Row <= std::min(Height - 1, Bottom); Row++) {
        for (int Column = std::max(0, Left); Column <= std::min(Width - 1, Right); Column++) {
            const float Dx = static_cast<float>(Column) + 0.5f - X, Dy = (static_cast<float>(Row) + 0.5f - Y) * 1.1f;
            const float Near = 1.0f - (Dx * Dx + Dy * Dy) * Inverse;
            if (Near <= 0.0f) continue;
            const int Weight = static_cast<int>(Near * Near * static_cast<float>(Strength));
            Add(Column, Row, (Ink.Red() * Weight) >> 8, (Ink.Green() * Weight) >> 8, (Ink.Blue() * Weight) >> 8);
        }
    }
}

void Sprite::Cover(int X, int Y, Color Ink, float Coverage)
{
    if (!Inside(X, Y) || Coverage <= 0.0f) return;
    std::uint32_t &Texel = Texels[static_cast<std::size_t>(Y) * Width + X];
    const int Old = static_cast<int>(Texel >> 24);
    const int Add = Weighted(Coverage);
    const int Sum = std::min(255, Old + Add);
    // What the shape adds takes the share of the colour its coverage has of what is there.
    const Color Below = Color::FromRaw(Texel);
    const int Share = Old == 0 ? 256 : std::min(256, Add * 256 / std::max(1, Sum));
    const Color Blended = Mixed(Below, Ink, Share);
    Texel = (static_cast<std::uint32_t>(Sum) << 24) | Blended.Raw();
}

}  // namespace wizard
