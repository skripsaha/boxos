/* canvas.cpp — the canvas's rectangles and glows. */

#include "canvas.h"

namespace seabattle {

void Canvas::Fill(int X, int Y, int Wide, int High, Color Ink)
{
    const int Left = std::max(0, X), Right = std::min(Width, X + Wide);
    const int Top = std::max(0, Y), Bottom = std::min(Height, Y + High);
    for (int Row = Top; Row < Bottom; Row++)
        for (int Column = Left; Column < Right; Column++) Put(Column, Row, Ink);
}

void Canvas::Glow(float X, float Y, float Radius, Color Ink, int Strength)
{
    const int Left = static_cast<int>(X - Radius), Right = static_cast<int>(X + Radius) + 1;
    const int Top = static_cast<int>(Y - Radius), Bottom = static_cast<int>(Y + Radius) + 1;
    const float Inverse = 1.0f / (Radius * Radius);
    for (int Row = std::max(0, Top); Row <= std::min(Height - 1, Bottom); Row++) {
        for (int Column = std::max(0, Left); Column <= std::min(Width - 1, Right); Column++) {
            const float Dx = static_cast<float>(Column) + 0.5f - X, Dy = static_cast<float>(Row) + 0.5f - Y;
            const float Near = 1.0f - (Dx * Dx + Dy * Dy) * Inverse;
            if (Near <= 0.0f) continue;
            const int Weight = static_cast<int>(Near * Near * static_cast<float>(Strength));
            Add(Column, Row, (Ink.Red() * Weight) >> 8, (Ink.Green() * Weight) >> 8, (Ink.Blue() * Weight) >> 8);
        }
    }
}

}  // namespace seabattle
