/* room_parts.cpp — what is painted once: the wall of stone blocks with their
 * mortar, the ceiling beam, the boards and the rug. */

#include <algorithm>
#include <cmath>

#include "room_parts.h"
#include "shapes.h"

namespace wizard {

namespace {

// One block of the wall at X, Top, Wide by Course pixels: lit along its top and
// its left, dark along its bottom and its right, the mortar around it.
void PaintBlock(Canvas &Out, int X, int Top, int Wide, int Course, int Bottom, Color Stone, std::uint32_t Roll)
{
    const float Tone = 0.78f + 0.36f * static_cast<float>(Roll >> 8 & 255u) / 255.0f;
    const Color Base = Mixed(Toned(Stone, Tone), Color::Rgb(0x70, 0x68, 0x58), static_cast<int>((Roll >> 16 & 63u)));
    const Color Mortar = Toned(Stone, 0.34f);
    for (int Y = Top; Y < std::min(Top + Course, Bottom); Y++) {
        for (int Column = X; Column < X + Wide; Column++) {
            const int U = Column - X, V = Y - Top;
            const float Roughness = Grain(Column, Y, 3);
            Color Here = Toned(Base, 0.90f + 0.20f * Roughness);
            if (V == 0 || U == 0) Here = Mortar;
            else if (V == 1 || U == 1) Here = Toned(Base, 1.20f + 0.2f * Roughness);
            else if (V == Course - 1 || U == Wide - 1) Here = Toned(Base, 0.72f);
            else if (Hash(Column, Y, 11) % 97u == 0) Here = Toned(Base, 0.62f);
            Out.Put(Column, Y, Here);
        }
    }
}

}  // namespace

void PaintWall(Canvas &Out, const Scene &Where, Color Stone)
{
    const int Course = std::max(4, Where.FloorTop / 10);
    const int Brick = Course * 5 / 2;
    for (int Row = 0, Top = 0; Top < Where.FloorTop; Row++, Top += Course) {
        int X = -static_cast<int>(Hash(Row, 0, 1) % static_cast<std::uint32_t>(Brick));
        for (int Index = 0; X < Where.Width; Index++) {
            const std::uint32_t Roll = Hash(Index, Row, 7);
            const int Wide = Brick * static_cast<int>(70 + Roll % 50) / 100 + 2;
            PaintBlock(Out, X, Top, Wide, Course, Where.FloorTop, Stone, Roll);
            X += Wide;
        }
    }
    // The wall is darker where the roof comes down on it, and where the floor meets it.
    for (int Y = 0; Y < Where.FloorTop; Y++) {
        const float Roof = 0.55f + 0.45f * Smooth(0.0f, static_cast<float>(Where.FloorTop) * 0.30f, static_cast<float>(Y));
        const float Base = 0.78f + 0.22f * Smooth(0.0f, 5.0f, static_cast<float>(Where.FloorTop - Y));
        for (int X = 0; X < Where.Width; X++) Out.At(X, Y) = Toned(Out.At(X, Y), Roof * Base);
    }
    // A little moss where the stones are damp, and the cracks between them.
    for (int Y = Where.FloorTop / 2; Y < Where.FloorTop; Y++)
        for (int X = 0; X < Where.Width; X++)
            if (Grain(X / 2, Y / 2, 21) > 0.965f) Out.Put(X, Y, Mixed(Out.At(X, Y), Color::Rgb(0x3E, 0x60, 0x34), 150));
}

void PaintBeam(Canvas &Out, const Scene &Where)
{
    const Color Wood = Color::Rgb(0x52, 0x36, 0x20);
    for (int X = 0; X < Where.Width; X++) {
        Out.Put(X, 0, Toned(Wood, 0.7f + 0.3f * Grain(X / 3, 0, 5)));
        Out.Put(X, 1, Toned(Wood, 0.45f + 0.2f * Grain(X / 2, 1, 6)));
    }
    for (int X = 5; X < Where.Width; X += std::max(14, Where.Width / 7))
        for (int Y = 2; Y < 4; Y++) Out.Put(X, Y, Toned(Wood, 0.38f));
}

void PaintFloor(Canvas &Out, const Scene &Where, Color Boards)
{
    const int Board = std::max(2, (Where.ShelfTop - Where.FloorTop) / 3);
    for (int Y = Where.FloorTop; Y < Where.ShelfTop; Y++) {
        const int Row = (Y - Where.FloorTop) / Board;
        const int Line = (Y - Where.FloorTop) % Board;
        const int Joint = static_cast<int>(Hash(Row, 2, 9) % 31u);
        for (int X = 0; X < Where.Width; X++) {
            const int Plank = (X + Joint) / 31;
            const float Tone = 0.80f + 0.36f * Grain(Plank, Row, 12) + 0.14f * (Grain(X / 4, Y, 13) - 0.5f);
            Color Here = Toned(Boards, Tone);
            if (Line == 0) Here = Toned(Boards, Tone * 1.18f);
            if (Line == Board - 1) Here = Toned(Boards, Tone * 0.55f);
            if ((X + Joint) % 31 == 0) Here = Toned(Boards, 0.40f);
            Out.Put(X, Y, Here);
        }
    }
    for (int X = 0; X < Where.Width; X++) Out.Put(X, Where.FloorTop - 1, Toned(Boards, 0.42f));
}

void PaintRug(Canvas &Out, const Scene &Where)
{
    const float Cx = static_cast<float>(Where.WizardX), Cy = static_cast<float>(Where.FeetY);
    const float Rx = Where.Unit * 58.0f, Ry = std::max(2.5f, static_cast<float>(Where.ShelfTop - Where.FloorTop) * 0.55f);
    const Color Wine = Color::Rgb(0x7E, 0x2A, 0x34), Gold = Color::Rgb(0xD0, 0xA0, 0x48), Navy = Color::Rgb(0x34, 0x30, 0x6C);
    FillEllipse(Out, Cx, Cy, Rx, Ry, [&](float Nx, float Ny, float Inside) {
        const float Far = std::sqrt(Nx * Nx + Ny * Ny);
        Color Here = Far < 0.40f ? Navy : Wine;
        if (Far > 0.86f || (Far > 0.40f && Far < 0.50f)) Here = Gold;
        if (Far > 0.70f && Far < 0.78f && (static_cast<int>(Nx * 14.0f + 20.0f) & 1) == 0) Here = Gold;
        if (Inside < 1.0f) Here = Darker(Here, 90);
        return Toned(Here, 0.85f + 0.2f * Grain(static_cast<int>(Nx * 40.0f), static_cast<int>(Ny * 40.0f), 31));
    });
}

}  // namespace wizard
