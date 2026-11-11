/* room_life.cpp — what moves in the room, drawn over it every frame: the night
 * in the window with its moon, twinkling stars and now and then a falling one,
 * the herbs that hang from the beam, the candles' flames, and the ring that
 * lights on the boards under him when he casts. */

#include <algorithm>
#include <cmath>

#include "room_parts.h"
#include "shapes.h"

namespace wizard {

namespace {

Color Lit(Color Ink, const Tint &Light)
{
    return Scaled(Ink, Light.Red, Light.Green, Light.Blue);
}

// One star at X, Y that is lit by Level, 0..1.
void Star(Canvas &Out, int X, int Y, float Level)
{
    const Color Pale = Color::Rgb(0xFF, 0xFA, 0xE0);
    Out.Cover(X, Y, Pale, Level);
    if (Level > 0.8f) {
        Out.Cover(X - 1, Y, Pale, 0.30f);
        Out.Cover(X + 1, Y, Pale, 0.30f);
        Out.Cover(X, Y - 1, Pale, 0.30f);
        Out.Cover(X, Y + 1, Pale, 0.30f);
    }
}

// The moon, its craters and the halo it throws on the stars around it.
void DrawMoon(Canvas &Out, const Box &Window, const Show &Now)
{
    const float Radius = std::max(2.0f, static_cast<float>(Window.Width) * 0.17f);
    const float Cx = static_cast<float>(Window.X) + static_cast<float>(Window.Width) * 0.64f;
    const float Cy = static_cast<float>(Window.Y) + static_cast<float>(Window.Width) * 0.62f;
    Out.Glow(Cx, Cy, Radius * 2.6f, Now.Moon, 70);
    FillEllipse(Out, Cx, Cy, Radius, Radius, [&](float Nx, float Ny, float) {
        const Color Face = Color::Rgb(0xF2, 0xEC, 0xD0);
        const float Crater = Grain(static_cast<int>((Nx + 2.0f) * 9.0f), static_cast<int>((Ny + 2.0f) * 9.0f), 71);
        float Level = 1.0f - 0.18f * (Nx * 0.6f + Ny * 0.6f + 0.6f) - (Crater > 0.82f ? 0.12f : 0.0f);
        return Toned(Face, Level);
    });
}

}  // namespace

int Flicker(std::uint64_t Ms, std::uint32_t Seed, int Base, int Wander)
{
    const float Quick = Grain(static_cast<int>(Ms / 80), static_cast<int>(Seed), 81) - 0.5f;
    const float Slow = Wave(static_cast<float>(Ms % 3100) / 3100.0f + static_cast<float>(Seed) * 0.17f);
    return Base + static_cast<int>(static_cast<float>(Wander) * (Quick * 1.4f + 0.5f * Slow));
}

void DrawSky(Canvas &Out, const Scene &Where, const Show &Now)
{
    const Box W = Where.Window;
    const Color Top = Color::Rgb(0x06, 0x08, 0x14), Bottom = Color::Rgb(0x1A, 0x22, 0x44);
    for (int Y = W.Y; Y < W.Y + W.Height; Y++) {
        const float Down = static_cast<float>(Y - W.Y) / static_cast<float>(W.Height);
        for (int X = W.X; X < W.X + W.Width; X++) {
            if (!InGlass(W, X, Y)) continue;
            Color Here = Mixed(Top, Bottom, Down * Down);
            if (((X * 3 + Y * 2) % 11) < 2) Here = Added(Here, 7, 8, 10);
            Out.Put(X, Y, Here);
        }
    }
    DrawMoon(Out, W, Now);
    const int Stars = std::max(4, W.Width / 2);
    for (int Index = 0; Index < Stars; Index++) {
        const std::uint32_t Roll = Hash(Index, 3, 91);
        const int X = W.X + 2 + static_cast<int>(Roll % static_cast<std::uint32_t>(std::max(1, W.Width - 4)));
        const int Y = W.Y + 2 + static_cast<int>((Roll >> 10) % static_cast<std::uint32_t>(std::max(1, W.Height * 2 / 3)));
        if (!InGlass(W, X, Y)) continue;
        const float Level = 0.25f + 0.75f * Beat(Now.Ms + Roll % 4000u, 1600 + Roll % 1700u);
        Star(Out, X, Y, Level);
    }
    // A falling star now and then, from the upper right to the lower left.
    const std::uint64_t Cycle = Now.Ms % 17000u;
    if (Cycle < 700u) {
        const float Along = static_cast<float>(Cycle) / 700.0f;
        for (int Tail = 0; Tail < 6; Tail++) {
            const float At = Along - static_cast<float>(Tail) * 0.04f;
            if (At < 0.0f) continue;
            const int X = W.X + W.Width - 3 - static_cast<int>(At * static_cast<float>(W.Width) * 0.7f);
            const int Y = W.Y + 3 + static_cast<int>(At * static_cast<float>(W.Height) * 0.45f);
            if (InGlass(W, X, Y)) Out.Cover(X, Y, Color::Rgb(0xFF, 0xFF, 0xF0), 1.0f - static_cast<float>(Tail) * 0.17f);
        }
    }
    // The lead between the panes.
    const Color Lead = Color::Rgb(0x16, 0x12, 0x0E);
    for (int Y = W.Y; Y < W.Y + W.Height; Y++)
        if (InGlass(W, W.X + W.Width / 2, Y)) Out.Put(W.X + W.Width / 2, Y, Lead);
    for (int X = W.X; X < W.X + W.Width; X++)
        if (InGlass(W, X, W.Y + W.Height * 11 / 20)) Out.Put(X, W.Y + W.Height * 11 / 20, Lead);
}

void DrawHerbs(Canvas &Out, const Scene &Where, const Show &Now, const Lighting &Lights)
{
    const int Bunches = 3;
    for (int Index = 0; Index < Bunches; Index++) {
        const int X = static_cast<int>(static_cast<float>(Where.Width) * (0.37f + 0.08f * static_cast<float>(Index)));
        const int Length = static_cast<int>(Where.Unit * static_cast<float>(14 + (Index * 5) % 9)) + 4;
        const float Phase = static_cast<float>(Index) * 0.31f;
        float SwayX = 0.0f;
        for (int Down = 2; Down < Length; Down++) {
            const float Reach = static_cast<float>(Down) / static_cast<float>(Length);
            SwayX = Wave(static_cast<float>(Now.Ms % 3300) / 3300.0f + Phase) * 1.2f * Reach * Reach;
            const int Px = X + static_cast<int>(std::lround(SwayX));
            Out.Put(Px, Down, Lit(Color::Rgb(0xA0, 0x84, 0x58), Lights.At(Px, Down)));
        }
        const int Cx = X + static_cast<int>(std::lround(SwayX)), Top = Length;
        const int Wide = std::max(2, static_cast<int>(Where.Unit * 4.0f));
        for (int Row = 0; Row < Wide * 2 + 2; Row++) {
            const int Reach = std::max(0, Wide - Row / 2);
            for (int Column = -Reach; Column <= Reach; Column++) {
                const std::uint32_t Roll = Hash(Column + 20, Row, static_cast<std::uint32_t>(Index) + 95u);
                Color Leaf = Index == 0 ? Color::Rgb(0x8A, 0x62, 0xC8) : Index == 1 ? Color::Rgb(0xC0, 0x34, 0x2C) : Color::Rgb(0x5E, 0x98, 0x44);
                if (Roll % 4u == 0) Leaf = Color::Rgb(0x4A, 0x70, 0x34);
                Out.Put(Cx + Column, Top + Row, Lit(Toned(Leaf, 0.7f + 0.4f * Grain(Column, Row, 96)), Lights.At(Cx + Column, Top + Row)));
            }
        }
    }
}

void DrawFlames(Canvas &Out, const Scene &Where, const Show &Now)
{
    for (int Which = 0; Which < 3; Which++) {
        const int X = Where.CandleX[Which], Row = FlameRow(Where, Which);
        const float Tall = std::max(2.5f, Where.Unit * 7.0f);
        const float Lean = Wave(static_cast<float>(Now.Ms % 1300) / 1300.0f + static_cast<float>(Which) * 0.4f) * 0.7f +
                           (Grain(static_cast<int>(Now.Ms / 90), Which, 97) - 0.5f);
        const float Size = Tall * (0.88f + 0.24f * Grain(static_cast<int>(Now.Ms / 70), Which, 98));
        Out.Glow(static_cast<float>(X), static_cast<float>(Row) - Size * 0.5f, Size * 3.2f, Now.Candle, 50 + Flicker(Now.Ms, static_cast<std::uint32_t>(Which), 20, 18));
        const Color Outer = Color::Rgb(0xFF, 0x8A, 0x20), Mid = Color::Rgb(0xFF, 0xD0, 0x58), Core = Color::Rgb(0xFF, 0xF6, 0xC8);
        FillEllipse(Out, static_cast<float>(X) + Lean * 0.4f, static_cast<float>(Row) - Size * 0.5f, std::max(1.2f, Size * 0.30f), Size * 0.62f,
                    [&](float Nx, float Ny, float) {
                        const float Heat = 1.0f - (std::fabs(Nx) * 0.9f + (Ny + 1.0f) * 0.25f);
                        return Heat > 0.62f ? Core : Heat > 0.30f ? Mid : Outer;
                    });
        Out.Put(X, Row + 1, Color::Rgb(0x70, 0x98, 0xE8));
    }
}

void DrawCircle(Canvas &Out, const Scene &Where, const Show &Now)
{
    if (Now.Excite < 0.02f) return;
    const float Cx = static_cast<float>(Where.WizardX), Cy = static_cast<float>(Where.FeetY) + 0.5f;
    const float Rx = Where.Unit * 52.0f, Ry = std::max(2.5f, static_cast<float>(Where.ShelfTop - Where.FloorTop) * 0.52f);
    const float Turn = static_cast<float>(Now.Ms % 6000u) / 6000.0f;
    const int Dots = 150;
    for (int Dot = 0; Dot < Dots; Dot++) {
        const float Angle = static_cast<float>(Dot) / static_cast<float>(Dots);
        const bool Rune = (Dot % 15) < 2;
        const float Level = Now.Excite * (Rune ? 1.0f : 0.6f) * (0.7f + 0.3f * Wave(Angle * 3.0f - Turn * 2.0f));
        const float Px = Cx + Rx * Wave(Angle + 0.25f), Py = Cy + Ry * Wave(Angle);
        Out.Cover(static_cast<int>(Px), static_cast<int>(Py), Lighter(Now.Glow, Rune ? 120 : 20), Level);
        if (Rune) Out.Cover(static_cast<int>(Px), static_cast<int>(Py) - 1, Now.Glow, Level * 0.6f);
    }
    for (int Dot = 0; Dot < 60; Dot++) {
        const float Angle = static_cast<float>(Dot) / 60.0f + Turn;
        Out.Cover(static_cast<int>(Cx + Rx * 0.62f * Wave(Angle + 0.25f)), static_cast<int>(Cy + Ry * 0.62f * Wave(Angle)), Now.Glow,
                  Now.Excite * 0.5f);
    }
    Out.Glow(Cx, Cy, Rx * 0.9f, Now.Glow, static_cast<int>(60.0f * Now.Excite));
}

}  // namespace wizard
