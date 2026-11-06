#include "tumbleweed.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Color = TumbleColor;
namespace Colors = painter::core::Colors;
using painter::tui::Frame;
using painter::tui::TextStyle;
constexpr float Pi = 3.14159265358979323846f;

std::uint32_t Hash(int X, int Y)
{
    std::uint32_t Value = static_cast<std::uint32_t>(X) * 73856093u ^ static_cast<std::uint32_t>(Y) * 19349663u;
    Value ^= Value >> 13;
    return Value * 0x5BD1E995u;
}

int ScreenX(const TumbleWorld &World, int X) { return X - static_cast<int>(World.CameraX); }
int ScreenY(const TumbleWorld &World, int Y) { return Y - static_cast<int>(World.CameraY); }

void Pixel(Frame &Canvas, const TumbleWorld &World, int X, int Y, Color Ink, int Layer = 1)
{
    Canvas.PutPixel({ScreenX(World, X), ScreenY(World, Y)}, World.Illuminate(Ink, Y, Layer));
}

void RawPixel(Frame &Canvas, const TumbleWorld &World, int X, int Y, Color Ink)
{
    Canvas.PutPixel({ScreenX(World, X), ScreenY(World, Y)}, Ink);
}

void ScreenPixel(Frame &Canvas, int X, int Y, Color Ink)
{
    Canvas.PutPixel({X, Y}, Ink);
}

void Line(Frame &Canvas, const TumbleWorld &World, int X0, int Y0, int X1, int Y1, Color Ink, int Layer = 1)
{
    const int Steps = std::max(std::abs(X1 - X0), std::abs(Y1 - Y0));
    for (int At = 0; At <= Steps; At++) {
        const float T = Steps == 0 ? 0.0f : static_cast<float>(At) / Steps;
        Pixel(Canvas, World, static_cast<int>(X0 + (X1 - X0) * T), static_cast<int>(Y0 + (Y1 - Y0) * T), Ink, Layer);
    }
}

void Disc(Frame &Canvas, const TumbleWorld &World, int X, int Y, int Radius, Color Ink, int Layer = 1)
{
    for (int Dy = -Radius; Dy <= Radius; Dy++)
        for (int Dx = -Radius; Dx <= Radius; Dx++)
            if (Dx * Dx + Dy * Dy <= Radius * Radius) Pixel(Canvas, World, X + Dx, Y + Dy, Ink, Layer);
}

void RawDisc(Frame &Canvas, const TumbleWorld &World, int X, int Y, int Radius, Color Ink)
{
    for (int Dy = -Radius; Dy <= Radius; Dy++)
        for (int Dx = -Radius; Dx <= Radius; Dx++)
            if (Dx * Dx + Dy * Dy <= Radius * Radius) RawPixel(Canvas, World, X + Dx, Y + Dy, Ink);
}

int BaseY(const TumbleWorld &World, int X, int Layer)
{
    return Layer == 0 ? World.DistantGround(X) : World.Ground(X);
}

void PaintStars(Frame &Canvas, const TumbleWorld &World, const TumbleSky &Sky)
{
    if (!Sky.Night && Sky.Sunlight > 110) return;
    for (const TumbleStar &Star : World.Stars) {
        const int X = Star.X - static_cast<int>(World.CameraX * 0.18f);
        const int Y = Star.Y - static_cast<int>(World.CameraY * 0.12f);
        const float Pulse = 0.68f + 0.32f * std::sin(World.Time * 1.3f + Star.Phase);
        const Color Dim = Sky.Night ? Color::Rgb(0x9E, 0xB8, 0xD8) : Color::Rgb(0xE1, 0xC0, 0xAE);
        const Color Ink = Color::Mix(Colors::Black, Dim, static_cast<std::uint8_t>(Pulse * 255.0f));
        ScreenPixel(Canvas, X, Y, Ink);
        if (Star.Size == 2) {
            ScreenPixel(Canvas, X + 1, Y, Ink);
            ScreenPixel(Canvas, X, Y + 1, Ink);
        }
    }
}

void PaintSun(Frame &Canvas, const TumbleWorld &World, const TumbleSky &Sky)
{
    if (Sky.Sunlight <= 0) return;
    Color Glow = World.Season == TumbleSeason::Winter ? Color::Rgb(0xE8, 0xE8, 0xC8)
               : Sky.Sunlight < 150 ? Color::Rgb(0xFF, 0xA4, 0x52)
                                    : Color::Rgb(0xFF, 0xD0, 0x62);
    for (int Radius = 13; Radius >= 5; Radius--) {
        const std::uint8_t Weight = static_cast<std::uint8_t>(28 + (13 - Radius) * 27);
        RawDisc(Canvas, World, Sky.SunX, Sky.SunY, Radius, Color::Mix(World.SkyAt(Sky.SunY, Sky), Glow, Weight));
    }
    RawDisc(Canvas, World, Sky.SunX, Sky.SunY, 5, Glow);
}

void PaintMoon(Frame &Canvas, const TumbleWorld &World, const TumbleSky &Sky)
{
    if (!Sky.Night) return;
    RawDisc(Canvas, World, Sky.MoonX, Sky.MoonY, 5, Color::Rgb(0xC5, 0xD0, 0xD8));
    RawDisc(Canvas, World, Sky.MoonX + 2, Sky.MoonY - 1, 5, World.SkyAt(Sky.MoonY, Sky));
    RawPixel(Canvas, World, Sky.MoonX - 2, Sky.MoonY + 1, Color::Rgb(0xE8, 0xE5, 0xC8));
}

void PaintClouds(Frame &Canvas, const TumbleWorld &World, const TumbleSky &Sky)
{
    const bool Heavy = World.Weather == TumbleWeather::Rain || World.Weather == TumbleWeather::Storm;
    const int Count = Heavy ? 8 : World.Weather == TumbleWeather::Wind ? 5 : 3;
    const Color Cloud = Sky.Night ? Color::Rgb(0x1C, 0x27, 0x43)
                      : Heavy ? Color::Rgb(0x5D, 0x68, 0x78) : Color::Rgb(0xA5, 0xB4, 0xBA);
    for (int Index = 0; Index < Count; Index++) {
        const int X = static_cast<int>(std::fmod(Index * 79.0f + World.Time * (Heavy ? 9.0f : 3.0f), World.Width + 44)) - 22;
        const int Y = 10 + (Index * 19) % 22;
        const int Width = 5 + (Index % 4) * 3;
        for (int Puff = 0; Puff < Width; Puff++)
            Disc(Canvas, World, X + Puff * 2, Y - (Puff % 3 == 1 ? 2 : 0), 2 + (Puff % 2), Cloud, 0);
    }
}

void PaintCanyon(Frame &Canvas, const TumbleWorld &World)
{
    for (int Col = 0; Col < World.ViewWidth; Col++) {
        const int X = static_cast<int>(World.CameraX) + Col;
        const int Top = World.DistantGround(X);
        const int Bottom = std::min(World.Ground(X) - 2, Top + 26);
        for (int Y = Top + 2; Y < Bottom; Y++) {
            const std::uint32_t Value = Hash(X / 2, Y / 2);
            if ((Value & 15u) == 0u)
                Pixel(Canvas, World, X, Y, World.Season == TumbleSeason::Winter ? Color::Rgb(0x80, 0x78, 0x78)
                                                                                   : Color::Rgb(0x8A, 0x55, 0x46), 0);
            if ((Value & 31u) == 1u && X % 3 == 0)
                Pixel(Canvas, World, X, Y, Color::Rgb(0x4B, 0x3D, 0x48), 0);
        }
    }
}

void PaintGroundDetails(Frame &Canvas, const TumbleWorld &World)
{
    const int Left = std::max(0, static_cast<int>(World.CameraX) - 2);
    const int Right = std::min(World.Width - 1, static_cast<int>(World.CameraX) + World.ViewWidth + 2);
    for (int X = Left; X <= Right; X++) {
        const std::uint32_t Value = Hash(X, 17);
        const int Ground = World.Ground(X);
        if ((Value % 17u) == 0u) Pixel(Canvas, World, X, Ground - 1, Color::Rgb(0x6F, 0x55, 0x3E));
        if ((Value % 31u) == 0u) {
            Pixel(Canvas, World, X, Ground - 1, Color::Rgb(0xDB, 0xBA, 0x7B));
            Pixel(Canvas, World, X + 1, Ground - 1, Color::Rgb(0xDB, 0xBA, 0x7B));
        }
        if ((Value % 47u) == 0u) {
            Pixel(Canvas, World, X, Ground - 1, Color::Rgb(0x7E, 0x6A, 0x4A));
            Pixel(Canvas, World, X + 1, Ground - 2, Color::Rgb(0x7E, 0x6A, 0x4A));
        }
    }
}

void DrawCactus(Frame &Canvas, const TumbleWorld &World, const TumbleCactus &One)
{
    const float Scale = One.Layer == 0 ? 0.62f : 1.0f;
    const int Base = BaseY(World, One.X, One.Layer);
    const int Height = std::max(5, static_cast<int>(One.Height * Scale));
    const Color Ink = One.Layer == 0 ? Color::Mix(One.Ink, World.SkyAt(Base, World.Sky()), 100) : One.Ink;
    const Color Dark = Color::Mix(Ink, Colors::Black, 82);
    const Color Light = Color::Mix(Ink, Colors::White, 45);
    for (int Rise = 0; Rise < Height; Rise++) {
        Pixel(Canvas, World, One.X, Base - Rise, Ink, One.Layer);
        Pixel(Canvas, World, One.X + 1, Base - Rise, Rise % 4 == 1 ? Dark : Light, One.Layer);
    }
    if (One.Arms) {
        const int Left = Height * 44 / 100;
        const int Right = Height * 67 / 100;
        const int Reach = std::max(2, static_cast<int>(4 * Scale));
        Line(Canvas, World, One.X, Base - Left, One.X - Reach, Base - Left, Ink, One.Layer);
        Line(Canvas, World, One.X - Reach, Base - Left, One.X - Reach, Base - Left - std::max(2, static_cast<int>(4 * Scale)), Ink,
             One.Layer);
        Line(Canvas, World, One.X, Base - Right, One.X + Reach, Base - Right, Ink, One.Layer);
        Line(Canvas, World, One.X + Reach, Base - Right, One.X + Reach, Base - Right - std::max(2, static_cast<int>(3 * Scale)), Ink,
             One.Layer);
    }
    if (World.Season == TumbleSeason::Spring && One.Layer == 1)
        Pixel(Canvas, World, One.X, Base - Height - 1, Color::Rgb(0xF0, 0xB0, 0x58), One.Layer);
}

void DrawRock(Frame &Canvas, const TumbleWorld &World, const TumbleRock &One)
{
    const float Scale = One.Layer == 0 ? 0.68f : 1.0f;
    const int Base = BaseY(World, One.X, One.Layer);
    const int Width = std::max(3, static_cast<int>(One.Width * Scale));
    const int Height = std::max(3, static_cast<int>(One.Height * Scale));
    const Color BaseInk = One.Layer == 0 ? Color::Mix(One.Ink, World.SkyAt(Base, World.Sky()), 110) : One.Ink;
    for (int Row = 0; Row < Height; Row++) {
        const float Position = Height <= 1 ? 0.0f : static_cast<float>(Row) / static_cast<float>(Height - 1);
        const int Half = std::max(1, static_cast<int>((1.0f - Position * 0.56f) * Width / 2.0f));
        for (int Col = -Half; Col <= Half; Col++) {
            Color Ink = BaseInk;
            if (Row < Height / 3 && Col < 1) Ink = Color::Mix(Ink, Colors::White, 32);
            if (Col > Half - 2) Ink = Color::Mix(Ink, Colors::Black, 62);
            if (((One.Seed + static_cast<std::uint32_t>(Row * 17 + Col * 5)) & 15u) == 0u)
                Ink = Color::Mix(Ink, Color::Rgb(0x55, 0x45, 0x42), 58);
            Pixel(Canvas, World, One.X + Col, Base - Row, Ink, One.Layer);
        }
    }
    if (One.Layer == 1) {
        Line(Canvas, World, One.X - Width / 3, Base - Height + 1, One.X, Base - Height / 2, Color::Rgb(0x4A, 0x3B, 0x35), One.Layer);
        Pixel(Canvas, World, One.X + Width / 4, Base - Height / 2, Color::Rgb(0xBC, 0x91, 0x62), One.Layer);
    }
}

void DrawRuin(Frame &Canvas, const TumbleWorld &World, const TumbleRuin &One)
{
    const float Scale = One.Layer == 0 ? 0.62f : 1.0f;
    const int Base = BaseY(World, One.X, One.Layer);
    const int Height = std::max(6, static_cast<int>(One.Height * Scale));
    const Color Wood = One.Layer == 0 ? Color::Mix(One.Ink, World.SkyAt(Base, World.Sky()), 100) : One.Ink;
    const Color DarkWood = Color::Mix(Wood, Colors::Black, 72);
    const Color LightWood = Color::Mix(Wood, Colors::White, 35);

    switch (One.Kind) {
    case TumbleStructureKind::Mine: {
        const int Width = std::max(8, static_cast<int>(12 * Scale));
        for (int Rise = 0; Rise < Height; Rise++) {
            Pixel(Canvas, World, One.X - Width / 2, Base - Rise, Wood, One.Layer);
            Pixel(Canvas, World, One.X + Width / 2, Base - Rise, DarkWood, One.Layer);
        }
        Line(Canvas, World, One.X - Width / 2 - 1, Base - Height, One.X, Base - Height - 4, LightWood, One.Layer);
        Line(Canvas, World, One.X, Base - Height - 4, One.X + Width / 2 + 1, Base - Height, DarkWood, One.Layer);
        for (int Row = 0; Row < Height - 2; Row++)
            for (int Col = -Width / 2 + 2; Col < Width / 2 - 1; Col++)
                Pixel(Canvas, World, One.X + Col, Base - Row - 1, Color::Rgb(0x0A, 0x0A, 0x12), One.Layer);
        Line(Canvas, World, One.X - Width / 2 - 2, Base - Height / 2, One.X + Width / 2 + 2, Base - Height / 2 + 1, LightWood,
             One.Layer);
        break;
    }
    case TumbleStructureKind::WaterTower: {
        const int Width = std::max(7, static_cast<int>(14 * Scale));
        for (int Rise = 0; Rise < Height; Rise++) {
            if (Rise < Height - 5 || Rise % 2 == 0) {
                Pixel(Canvas, World, One.X - Width / 2, Base - Rise, DarkWood, One.Layer);
                Pixel(Canvas, World, One.X + Width / 2, Base - Rise, Wood, One.Layer);
            }
        }
        for (int Row = 0; Row < 5; Row++)
            for (int Col = -Width / 2; Col <= Width / 2; Col++) Pixel(Canvas, World, One.X + Col, Base - Height + Row, Wood, One.Layer);
        Line(Canvas, World, One.X - Width / 2 - 2, Base - Height + 1, One.X + Width / 2 + 2, Base - Height + 1, LightWood, One.Layer);
        Line(Canvas, World, One.X, Base - Height - 4, One.X, Base - Height + 1, DarkWood, One.Layer);
        break;
    }
    case TumbleStructureKind::Outpost: {
        const int Width = std::max(10, static_cast<int>(16 * Scale));
        for (int Row = 0; Row < Height / 2; Row++)
            for (int Col = -Width / 2; Col <= Width / 2; Col++) Pixel(Canvas, World, One.X + Col, Base - Row, Wood, One.Layer);
        Line(Canvas, World, One.X - Width / 2 - 2, Base - Height / 2, One.X, Base - Height / 2 - 4, DarkWood, One.Layer);
        Line(Canvas, World, One.X, Base - Height / 2 - 4, One.X + Width / 2 + 2, Base - Height / 2, DarkWood, One.Layer);
        for (int Col = -2; Col <= 2; Col++) Pixel(Canvas, World, One.X + Col, Base - 2, Color::Rgb(0x0A, 0x0A, 0x0C), One.Layer);
        for (int Col = -Width / 2 + 2; Col < Width / 2 - 1; Col += 4)
            Pixel(Canvas, World, One.X + Col, Base - Height / 2 + 2, Color::Rgb(0xF0, 0xB0, 0x45), One.Layer);
        Line(Canvas, World, One.X + Width / 2 + 3, Base - Height / 2 - 1, One.X + Width / 2 + 3, Base - Height / 2 - 8, DarkWood, One.Layer);
        Line(Canvas, World, One.X + Width / 2 + 3, Base - Height / 2 - 8, One.X + Width / 2 + 9, Base - Height / 2 - 8, LightWood, One.Layer);
        break;
    }
    }
}

void DrawDeadTree(Frame &Canvas, const TumbleWorld &World, const TumbleDeadTree &One)
{
    const int Base = BaseY(World, One.X, One.Layer);
    const Color Trunk = One.Layer == 0 ? Color::Mix(One.Ink, World.SkyAt(Base, World.Sky()), 100) : One.Ink;
    const Color Edge = Color::Mix(Trunk, Colors::Black, 70);
    for (int Rise = 0; Rise < One.Height; Rise++) {
        const int Lean = static_cast<int>(std::sin(Rise * 0.18f + One.X) * 0.9f);
        Pixel(Canvas, World, One.X + Lean, Base - Rise, Trunk, One.Layer);
        if (Rise % 2 == 0) Pixel(Canvas, World, One.X + Lean + 1, Base - Rise, Edge, One.Layer);
    }
    const int Crown = Base - One.Height * 58 / 100;
    Line(Canvas, World, One.X, Crown, One.X - 6, Crown - 7, Trunk, One.Layer);
    Line(Canvas, World, One.X - 6, Crown - 7, One.X - 10, Crown - 9, Edge, One.Layer);
    Line(Canvas, World, One.X, Crown + 3, One.X + 7, Crown - 5, Trunk, One.Layer);
    Line(Canvas, World, One.X + 7, Crown - 5, One.X + 11, Crown - 8, Edge, One.Layer);
    Line(Canvas, World, One.X, Crown + 7, One.X - 4, Crown + 1, Trunk, One.Layer);
}

void DrawBone(Frame &Canvas, const TumbleWorld &World, const TumbleBone &One)
{
    const Color Bone = World.Illuminate(One.Ink, One.Y, One.Layer);
    if (One.Skull) {
        Disc(Canvas, World, One.X, One.Y, 2, One.Ink, One.Layer);
        RawPixel(Canvas, World, One.X - 1, One.Y, Colors::Black);
        RawPixel(Canvas, World, One.X + 1, One.Y, Colors::Black);
        Pixel(Canvas, World, One.X, One.Y + 2, Bone, One.Layer);
        Line(Canvas, World, One.X - 1, One.Y + 3, One.X + 2, One.Y + 3, One.Ink, One.Layer);
        return;
    }
    Line(Canvas, World, One.X - 4, One.Y, One.X + 4, One.Y + 1, One.Ink, One.Layer);
    Pixel(Canvas, World, One.X - 4, One.Y - 1, One.Ink, One.Layer);
    Pixel(Canvas, World, One.X + 4, One.Y, One.Ink, One.Layer);
    Pixel(Canvas, World, One.X - 2, One.Y + 1, Bone, One.Layer);
}

void DrawFence(Frame &Canvas, const TumbleWorld &World, const TumbleFence &One)
{
    const int Base = BaseY(World, One.X, One.Layer);
    const Color Wood = World.Illuminate(One.Ink, Base, One.Layer);
    for (int Offset = 0; Offset <= One.Length; Offset += 7) {
        const int Height = 5 + ((Offset / 7) % 3);
        Line(Canvas, World, One.X + Offset, Base, One.X + Offset, Base - Height, Wood, One.Layer);
        Pixel(Canvas, World, One.X + Offset, Base - Height, Color::Mix(Wood, Colors::White, 25), One.Layer);
    }
    Line(Canvas, World, One.X, Base - 3, One.X + One.Length, Base - 3, Wood, One.Layer);
    Line(Canvas, World, One.X, Base - 6, One.X + One.Length, Base - 5, Color::Mix(Wood, Colors::Black, 25), One.Layer);
}

void DrawWeed(Frame &Canvas, const TumbleWorld &World, const TumbleWeed &One)
{
    const int X = static_cast<int>(One.X);
    const int Base = BaseY(World, X, One.Layer);
    const int Y = Base - (One.Layer == 0 ? 2 : 4);
    const int Reach = std::max(2, static_cast<int>((3 + std::abs(std::sin(One.Spin)) * 3) * (One.Layer == 0 ? 0.7f : 1.0f)));
    const Color Ink = One.Layer == 0 ? Color::Mix(One.Ink, World.SkyAt(Base, World.Sky()), 90) : One.Ink;
    Line(Canvas, World, X - Reach, Y, X + Reach, Y, Ink, One.Layer);
    Line(Canvas, World, X - 2, Y - Reach, X + 2, Y + Reach, Color::Mix(Ink, Colors::White, 45), One.Layer);
    Line(Canvas, World, X - 2, Y + Reach, X + 2, Y - Reach, Ink, One.Layer);
    if (One.Layer == 1) {
        Line(Canvas, World, X - Reach, Y + 1, X + Reach, Y + 1, Color::Mix(Ink, Colors::Black, 40), One.Layer);
        Pixel(Canvas, World, X, Y - Reach - 1, Color::Mix(Ink, Colors::White, 55), One.Layer);
    }
}

struct BirdFrame {
    int Width;
    int Height;
    const char *Rows[5];
};

constexpr BirdFrame BirdFrames[] = {
    {9, 5, {"..d...d..", ".dd...dd.", "dddeeddd>", ".dddddd..", "..d..d..."}},
    {9, 5, {".........", "..d...d..", ".dd...dd.", "dddeeddd>", "..dddd..."}},
};

void DrawBird(Frame &Canvas, const TumbleWorld &World, const TumbleBird &One)
{
    const BirdFrame &Sprite = BirdFrames[static_cast<std::size_t>(static_cast<int>(World.Time * 7.0f + One.Phase) & 1)];
    const int X0 = static_cast<int>(One.X);
    const int Y0 = static_cast<int>(One.Y);
    const Color Body = World.Illuminate(One.Ink, Y0, One.Layer);
    for (int Row = 0; Row < Sprite.Height; Row++) {
        for (int Col = 0; Col < Sprite.Width; Col++) {
            const int Source = One.Left ? Sprite.Width - 1 - Col : Col;
            const char Glyph = Sprite.Rows[Row][Source];
            if (Glyph == '.') continue;
            Color Ink = Body;
            if (Glyph == 'w') Ink = Color::Mix(Body, Colors::White, 85);
            if (Glyph == 'e') Ink = Colors::White;
            if (Glyph == '>') Ink = Color::Rgb(0xD8, 0xA2, 0x4D);
            Pixel(Canvas, World, X0 + Col, Y0 + Row, Ink, One.Layer);
        }
    }
}

void PaintLayer(Frame &Canvas, const TumbleWorld &World, int Layer)
{
    for (const TumbleRuin &One : World.Ruins) if (One.Layer == Layer) DrawRuin(Canvas, World, One);
    for (const TumbleDeadTree &One : World.DeadTrees) if (One.Layer == Layer) DrawDeadTree(Canvas, World, One);
    for (const TumbleFence &One : World.Fences) if (One.Layer == Layer) DrawFence(Canvas, World, One);
    for (const TumbleRock &One : World.Rocks) if (One.Layer == Layer) DrawRock(Canvas, World, One);
    for (const TumbleCactus &One : World.Cacti) if (One.Layer == Layer) DrawCactus(Canvas, World, One);
    for (const TumbleWeed &One : World.Weeds) if (One.Layer == Layer) DrawWeed(Canvas, World, One);
    for (const TumbleBone &One : World.Bones) if (One.Layer == Layer) DrawBone(Canvas, World, One);
    for (const TumbleBird &One : World.Birds) if (One.Layer == Layer) DrawBird(Canvas, World, One);
}

void PaintSeason(Frame &Canvas, const TumbleWorld &World)
{
    if (World.Season == TumbleSeason::Winter) {
        for (int Index = 0; Index < World.ViewWidth / 2; Index++) {
            const int X = static_cast<int>((static_cast<std::uint64_t>(Index * 97 + World.Time * 12) * 13u) %
                                           static_cast<std::uint64_t>(std::max(1, World.ViewWidth))) + World.CameraX;
            const int Y = static_cast<int>((static_cast<std::uint64_t>(Index * 53 + World.Time * 7) * 7u) %
                                           static_cast<std::uint64_t>(std::max(1, World.ViewHeight - 4))) + World.CameraY;
            Pixel(Canvas, World, X, Y, Color::Rgb(0xE0, 0xE8, 0xEC), 0);
        }
    } else if (World.Season == TumbleSeason::Spring) {
        for (int Index = 0; Index < World.ViewWidth / 5; Index++) {
            const int X = static_cast<int>((static_cast<std::uint64_t>(Index * 71) * 11u) %
                                           static_cast<std::uint64_t>(std::max(1, World.ViewWidth))) + World.CameraX;
            const int Ground = World.Ground(X);
            Pixel(Canvas, World, X, Ground - 1, Index % 2 == 0 ? Color::Rgb(0xD6, 0x8C, 0xB4) : Color::Rgb(0xF0, 0xC8, 0x58));
        }
    } else if (World.Season == TumbleSeason::Autumn) {
        for (int Index = 0; Index < World.ViewWidth / 6; Index++) {
            const int X = static_cast<int>((static_cast<std::uint64_t>(Index * 113 + World.Time * 9) * 17u) %
                                           static_cast<std::uint64_t>(std::max(1, World.ViewWidth))) + World.CameraX;
            const int Ground = World.Ground(X);
            Pixel(Canvas, World, X, Ground - 1, Index % 2 == 0 ? Color::Rgb(0xC0, 0x72, 0x35) : Color::Rgb(0x8A, 0x4B, 0x2E));
        }
    }
}

void PaintWeather(Frame &Canvas, const TumbleWorld &World)
{
    for (const TumbleDust &One : World.Dusts)
        Pixel(Canvas, World, static_cast<int>(One.X), static_cast<int>(One.Y), One.Ink, 0);
    if (World.Weather != TumbleWeather::Rain && World.Weather != TumbleWeather::Storm) return;
    const int Drops = World.ViewWidth / 3 + (World.Weather == TumbleWeather::Storm ? World.ViewWidth / 3 : 0);
    for (int Drop = 0; Drop < Drops; Drop++) {
        const int X = static_cast<int>((static_cast<std::uint64_t>(Drop * 101 + World.Time * 80) * 17u) %
                                       static_cast<std::uint64_t>(std::max(1, World.ViewWidth))) + World.CameraX;
        const int Y = static_cast<int>((static_cast<std::uint64_t>(Drop * 53 + World.Time * 130) * 13u) %
                                       static_cast<std::uint64_t>(std::max(1, World.ViewHeight))) + World.CameraY;
        Line(Canvas, World, X, Y, X - 1, Y + 3, Color::Rgb(0x72, 0xA9, 0xC8), 0);
    }
}

void PaintBackground(Frame &Canvas, const TumbleWorld &World)
{
    const int Columns = World.ViewWidth;
    const int PixelsHigh = World.ViewHeight;
    std::vector<Color> Pixels(static_cast<std::size_t>(Columns * (PixelsHigh + 2)), Colors::Black);
    const TumbleSky Sky = World.Sky();
    for (int Y = 0; Y < PixelsHigh + 2; Y++) {
        for (int X = 0; X < Columns; X++) {
            const int WorldX = std::clamp(static_cast<int>(World.CameraX) + X, 0, World.Width - 1);
            const int WorldY = static_cast<int>(World.CameraY) + Y;
            Color Ink;
            if (WorldY >= World.Ground(WorldX)) {
                const int Depth = WorldY - World.Ground(WorldX);
                Ink = World.Shade(World.Sand(), Sky.Sunlight + 52 - Depth * 4);
                const int Ripple = static_cast<int>(std::sin(WorldX * 0.21f + WorldY * 0.08f) * 14.0f);
                if (Ripple > 0) Ink = Color::Mix(Ink, Color::Rgb(0xF2, 0xC5, 0x78), static_cast<std::uint8_t>(Ripple));
                if (World.Season == TumbleSeason::Winter && Depth < 2)
                    Ink = Color::Mix(Ink, Color::Rgb(0xE2, 0xE7, 0xE6), 170);
            } else if (WorldY >= World.DistantGround(WorldX)) {
                const int Band = WorldY - World.DistantGround(WorldX);
                const Color Mesa = World.Season == TumbleSeason::Winter ? Color::Rgb(0x6E, 0x6D, 0x76)
                                 : (Band % 11 < 4 ? Color::Rgb(0x8A, 0x55, 0x47) : Color::Rgb(0x65, 0x46, 0x4B));
                Ink = World.Shade(Mesa, Sky.Sunlight + 78 - Band * 2);
            } else {
                Ink = World.SkyAt(WorldY, Sky);
            }
            if (World.Weather == TumbleWeather::Dust || World.Weather == TumbleWeather::Storm)
                Ink = Color::Mix(Ink, Color::Rgb(0xB7, 0x8B, 0x5D), 48);
            if (World.Weather == TumbleWeather::Rain || World.Weather == TumbleWeather::Storm)
                Ink = Color::Mix(Ink, Color::Rgb(0x65, 0x70, 0x79), World.Weather == TumbleWeather::Storm ? 112 : 72);
            Pixels[static_cast<std::size_t>(Y) * Columns + X] = Ink;
        }
    }
    Canvas.PutPixels(Pixels);
    PaintStars(Canvas, World, Sky);
    PaintSun(Canvas, World, Sky);
    PaintMoon(Canvas, World, Sky);
    PaintClouds(Canvas, World, Sky);
    PaintCanyon(Canvas, World);
}

void PaintBar(Frame &Canvas, const TumbleWorld &World)
{
    const int Row = World.ViewHeight / 2;
    const TextStyle Bar{Color::Rgb(0xEE, 0xEE, 0xEE), Color::Rgb(0x32, 0x36, 0x3A)};
    const TextStyle Hint{Color::Rgb(0x9C, 0xA4, 0xA8), Bar.Background};
    Canvas.Fill({0, Row, World.ViewWidth, 1}, ' ', Bar);

    int Col = 1;
    auto Put = [&](std::string_view Text, TextStyle Style, int Gap) {
        if (Col >= World.ViewWidth - 1) return false;
        Col += static_cast<int>(Canvas.Write({Col, Row}, Text, Style));
        Col += Gap;
        return Col < World.ViewWidth;
    };
    Put("tumbleweed", Bar, 2);
    Put(TumbleWeatherName(World.Weather), Hint, 2);
    Put(TumbleSeasonName(World.Season), Hint, 2);
    Put(TumbleLightName(World.Light), Hint, 2);
    if (!World.ShowKeys) return;
    Put("w weather", Hint, 2);
    Put("e season", Hint, 2);
    Put("n light", Hint, 2);
    Put("arrows move", Hint, 2);
    Put("s/c/b spawn", Hint, 2);
    Put("h hide", Hint, 2);
    Put("q leave", Hint, 1);
}

}  // namespace

void TumblePaint(const TumbleWorld &World, Frame &Canvas)
{
    PaintBackground(Canvas, World);
    PaintLayer(Canvas, World, 0);
    PaintLayer(Canvas, World, 1);
    PaintGroundDetails(Canvas, World);
    PaintSeason(Canvas, World);
    PaintWeather(Canvas, World);
    PaintBar(Canvas, World);
}
