#include "tumbleweed.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

namespace {

using Color = TumbleColor;
namespace Colors = painter::core::Colors;
using painter::tui::Frame;
using painter::tui::TextStyle;
constexpr float Pi = 3.14159265358979323846f;

void Pixel(Frame &Canvas, const TumbleWorld &World, int X, int Y, Color Ink)
{
    Canvas.PutPixel({X - static_cast<int>(World.CameraX), Y - static_cast<int>(World.CameraY)}, Ink);
}

void Line(Frame &Canvas, const TumbleWorld &World, int X0, int Y0, int X1, int Y1, Color Ink)
{
    const int Steps = std::max(std::abs(X1 - X0), std::abs(Y1 - Y0));
    for (int At = 0; At <= Steps; At++) {
        const float T = Steps == 0 ? 0.0f : static_cast<float>(At) / Steps;
        Pixel(Canvas, World, static_cast<int>(X0 + (X1 - X0) * T), static_cast<int>(Y0 + (Y1 - Y0) * T), Ink);
    }
}

void Disc(Frame &Canvas, const TumbleWorld &World, int X, int Y, int Radius, Color Ink)
{
    for (int Dy = -Radius; Dy <= Radius; Dy++)
        for (int Dx = -Radius; Dx <= Radius; Dx++)
            if (Dx * Dx + Dy * Dy <= Radius * Radius) Pixel(Canvas, World, X + Dx, Y + Dy, Ink);
}

int BaseY(const TumbleWorld &World, int X, int Layer)
{
    return Layer == 0 ? World.DistantGround(X) : World.Ground(X);
}

void DrawCactus(Frame &Canvas, const TumbleWorld &World, const TumbleCactus &One)
{
    const float Scale = One.Layer == 0 ? 0.62f : 1.0f;
    const int Base = BaseY(World, One.X, One.Layer);
    const int Height = std::max(4, static_cast<int>(One.Height * Scale));
    const Color Ink = One.Layer == 0 ? Color::Mix(One.Ink, World.SkyAt(Base, World.Sun()), 90) : One.Ink;
    const Color Dark = Color::Mix(Ink, Colors::Black, 75);
    for (int Rise = 0; Rise < Height; Rise++) {
        Pixel(Canvas, World, One.X, Base - Rise, Ink);
        Pixel(Canvas, World, One.X + 1, Base - Rise, Rise % 3 == 1 ? Dark : Ink);
    }
    if (!One.Arms) return;
    const int Left = Height / 2 + 1;
    const int Right = Height * 2 / 3;
    const int Reach = std::max(2, static_cast<int>(4 * Scale));
    Line(Canvas, World, One.X, Base - Left, One.X - Reach, Base - Left, Ink);
    Line(Canvas, World, One.X - Reach, Base - Left, One.X - Reach, Base - Left - std::max(2, static_cast<int>(4 * Scale)), Ink);
    Line(Canvas, World, One.X, Base - Right, One.X + Reach, Base - Right, Ink);
    Line(Canvas, World, One.X + Reach, Base - Right, One.X + Reach, Base - Right - std::max(2, static_cast<int>(3 * Scale)), Ink);
}

void DrawRock(Frame &Canvas, const TumbleWorld &World, const TumbleRock &One)
{
    const float Scale = One.Layer == 0 ? 0.65f : 1.0f;
    const int Base = BaseY(World, One.X, One.Layer);
    const int Width = std::max(2, static_cast<int>(One.Width * Scale));
    const int Height = std::max(2, static_cast<int>(One.Height * Scale));
    const Color Ink = One.Layer == 0 ? Color::Mix(One.Ink, World.SkyAt(Base, World.Sun()), 110) : One.Ink;
    for (int Row = 0; Row < Height; Row++) {
        const int Half = std::max(1, Width / 2 - Row / 2);
        for (int Col = -Half; Col <= Half; Col++) Pixel(Canvas, World, One.X + Col, Base - Row, Ink);
    }
    if (One.Layer == 1) Line(Canvas, World, One.X - Width / 3, Base - Height + 1, One.X + Width / 4, Base - Height / 2,
                              Color::Mix(Ink, Colors::White, 38));
}

void DrawRuin(Frame &Canvas, const TumbleWorld &World, const TumbleRuin &One)
{
    const float Scale = One.Layer == 0 ? 0.62f : 1.0f;
    const int Base = BaseY(World, One.X, One.Layer);
    const int Height = std::max(4, static_cast<int>(One.Height * Scale));
    const Color Ink = One.Layer == 0 ? Color::Mix(One.Ink, World.SkyAt(Base, World.Sun()), 100) : One.Ink;
    for (int Rise = 0; Rise < Height; Rise++) {
        const int Width = std::max(1, static_cast<int>((7 - Rise / 4) * Scale));
        for (int Col = -Width; Col <= Width; Col++)
            if ((Col + Rise) % 4 != 0) Pixel(Canvas, World, One.X + Col, Base - Rise, Ink);
    }
    if (One.Layer == 1) {
        const int Door = std::max(2, static_cast<int>(Height * 0.35f));
        for (int Rise = 0; Rise < Door; Rise++) Pixel(Canvas, World, One.X, Base - Rise, Colors::Black);
    }
}

void DrawWeed(Frame &Canvas, const TumbleWorld &World, const TumbleWeed &One)
{
    const int X = static_cast<int>(One.X);
    const int Base = BaseY(World, X, One.Layer);
    const int Y = Base - (One.Layer == 0 ? 2 : 4);
    const int Reach = std::max(2, static_cast<int>((3 + std::abs(std::sin(One.Spin)) * 3) * (One.Layer == 0 ? 0.7f : 1.0f)));
    const Color Ink = One.Layer == 0 ? Color::Mix(One.Ink, World.SkyAt(Base, World.Sun()), 90) : One.Ink;
    const Color Light = Color::Mix(Ink, Colors::White, 45);
    Line(Canvas, World, X - Reach, Y, X + Reach, Y, Ink);
    Line(Canvas, World, X - 2, Y - Reach, X + 2, Y + Reach, Light);
    Line(Canvas, World, X - 2, Y + Reach, X + 2, Y - Reach, Ink);
    if (One.Layer == 1) {
        Line(Canvas, World, X - Reach, Y + 1, X + Reach, Y + 1, Color::Mix(Ink, Colors::Black, 35));
        Pixel(Canvas, World, X, Y - Reach - 1, Light);
    }
}

void DrawBird(Frame &Canvas, const TumbleWorld &World, const TumbleBird &One)
{
    const float Scale = One.Layer == 0 ? 0.72f : 1.0f;
    const int X = static_cast<int>(One.X);
    const int Y = static_cast<int>(One.Y);
    const int Wing = static_cast<int>(std::sin(World.Time * 8.0f + One.Phase) * 2.0f * Scale);
    const Color Ink = One.Layer == 0 ? Color::Mix(One.Ink, World.SkyAt(Y, World.Sun()), 120) : One.Ink;
    const Color WingInk = Color::Mix(Ink, Colors::Black, 45);
    Line(Canvas, World, X - 3, Y, X + 3, Y, Ink);
    Pixel(Canvas, World, X + 4, Y - 1, Ink);
    Pixel(Canvas, World, X + 5, Y - 1, Colors::Amber);
    Line(Canvas, World, X - 1, Y, X - 8, Y - 4 - Wing, WingInk);
    Line(Canvas, World, X - 1, Y, X - 6, Y + 2, WingInk);
    Line(Canvas, World, X + 1, Y, X + 8, Y - 4 + Wing, WingInk);
    Line(Canvas, World, X + 1, Y, X + 6, Y + 2, WingInk);
    Pixel(Canvas, World, X - 4, Y + 1, Ink);
}

void PaintSun(Frame &Canvas, const TumbleWorld &World, int Columns)
{
    const int Sun = World.Sun();
    const float Orbit = std::fmod(World.Time * 0.018f, 1.0f);
    const float Arc = std::sin(Orbit * Pi);
    const int SunX = static_cast<int>(World.Width * (0.36f + Orbit * 0.30f));
    const int SunY = 8 + static_cast<int>((1.0f - Arc) * 16.0f);
    if (Sun > 0) {
        const Color Sky = World.SkyAt(SunY, Sun);
        const Color SunInk = World.Shade(Color::Rgb(0xFF, 0xD0, 0x62), Sun);
        for (int Radius = 12; Radius >= 6; Radius--) {
            const std::uint8_t Weight = static_cast<std::uint8_t>(24 + (12 - Radius) * 28);
            Disc(Canvas, World, SunX, SunY, Radius, Color::Mix(Sky, SunInk, Weight));
        }
        Disc(Canvas, World, SunX, SunY, 6, SunInk);
        if (World.Sunbeam) {
            const int End = World.Ground(static_cast<int>(World.CameraX) + Columns / 2);
            for (int Ray = -4; Ray <= 4; Ray++)
                Line(Canvas, World, SunX + Ray, SunY + 5, static_cast<int>(World.CameraX) + Columns / 2 + Ray * 3,
                     End, Color::Mix(Colors::Amber, Colors::White, 70));
        }
    } else {
        Disc(Canvas, World, SunX, SunY, 5, Color::Rgb(0xC0, 0xC8, 0xE0));
        for (int Star = 0; Star < Columns / 2; Star++) {
            const int X = static_cast<int>((static_cast<std::uint64_t>(Star) * 7919u) % static_cast<std::uint64_t>(Columns));
            const int Y = static_cast<int>((static_cast<std::uint64_t>(Star) * 3571u) % 24u) + 4;
            Pixel(Canvas, World, static_cast<int>(World.CameraX) + X, static_cast<int>(World.CameraY) + Y,
                  Star % 5 == 0 ? Colors::White : Color::Rgb(0xB0, 0xC0, 0xD8));
        }
    }
}

void PaintClouds(Frame &Canvas, const TumbleWorld &World)
{
    const bool Heavy = World.Weather == TumbleWeather::Rain || World.Weather == TumbleWeather::Storm;
    const bool Broken = World.Weather == TumbleWeather::Wind || World.Weather == TumbleWeather::Dust;
    const int Count = Heavy ? 9 : Broken ? 4 : 2;
    const Color Cloud = Heavy ? Color::Rgb(0x5A, 0x69, 0x78) : Color::Rgb(0xA7, 0xB4, 0xB8);
    for (int Index = 0; Index < Count; Index++) {
        const float Drift = World.Time * (Heavy ? 1.2f : 0.45f) + Index * 37.0f;
        const int X = static_cast<int>(std::fmod(Index * 71.0f + Drift * 8.0f,
                                                 static_cast<float>(World.Width + 40))) - 20;
        const int Y = 12 + (Index * 17) % 20;
        const int Width = 8 + (Index % 4) * 3;
        for (int Puff = 0; Puff < Width; Puff++) {
            const int At = X + Puff * 2;
            const int Lift = (Puff % 3 == 1) ? 2 : (Puff % 4 == 0 ? 1 : 0);
            Disc(Canvas, World, At, Y - Lift, 2 + (Puff % 2), Cloud);
        }
    }
}

void PaintSeason(Frame &Canvas, const TumbleWorld &World)
{
    if (World.Season == TumbleSeason::Winter) {
        for (int Index = 0; Index < World.ViewWidth / 2; Index++) {
            const int X = static_cast<int>((static_cast<std::uint64_t>(Index * 97 + World.Time * 24) * 13u) %
                                           static_cast<std::uint64_t>(World.ViewWidth));
            const int Y = static_cast<int>((static_cast<std::uint64_t>(Index * 53 + World.Time * 17) * 7u) %
                                           static_cast<std::uint64_t>(std::max(1, World.ViewHeight - 4)));
            Pixel(Canvas, World, static_cast<int>(World.CameraX) + X, static_cast<int>(World.CameraY) + Y,
                  Color::Rgb(0xE3, 0xEA, 0xEC));
        }
        return;
    }
    if (World.Season == TumbleSeason::Spring) {
        for (int Index = 0; Index < World.ViewWidth / 5; Index++) {
            const int X = static_cast<int>((static_cast<std::uint64_t>(Index * 71) * 11u) %
                                           static_cast<std::uint64_t>(World.ViewWidth));
            const int Ground = World.Ground(static_cast<int>(World.CameraX) + X);
            Pixel(Canvas, World, static_cast<int>(World.CameraX) + X, Ground - 1,
                  Index % 2 == 0 ? Color::Rgb(0xD6, 0x8C, 0xB4) : Color::Rgb(0xF0, 0xC8, 0x58));
        }
    }
    if (World.Season == TumbleSeason::Autumn) {
        for (int Index = 0; Index < World.ViewWidth / 7; Index++) {
            const int X = static_cast<int>((static_cast<std::uint64_t>(Index * 113 + World.Time * 9) * 17u) %
                                           static_cast<std::uint64_t>(World.ViewWidth));
            const int Ground = World.Ground(static_cast<int>(World.CameraX) + X);
            Pixel(Canvas, World, static_cast<int>(World.CameraX) + X, Ground - 1,
                  Index % 2 == 0 ? Color::Rgb(0xC0, 0x72, 0x35) : Color::Rgb(0x8A, 0x4B, 0x2E));
        }
    }
}

void PaintBackground(Frame &Canvas, const TumbleWorld &World)
{
    const int Columns = World.ViewWidth;
    const int PixelsHigh = World.ViewHeight;
    std::vector<Color> Pixels(static_cast<std::size_t>(Columns * (PixelsHigh + 2)), Colors::Black);
    const int Sun = World.Sun();
    for (int Y = 0; Y < PixelsHigh + 2; Y++) {
        for (int X = 0; X < Columns; X++) {
            const int WorldX = std::clamp(static_cast<int>(World.CameraX) + X, 0, World.Width - 1);
            const int WorldY = static_cast<int>(World.CameraY) + Y;
            Color Ink;
            if (WorldY >= World.Ground(WorldX)) {
                const int Depth = WorldY - World.Ground(WorldX);
                Ink = World.Season == TumbleSeason::Winter && Depth < 2
                          ? Color::Rgb(0xE2, 0xE8, 0xE8)
                          : World.Shade(World.Sand(), Sun + 30 - Depth * 5);
                const int Ripple = static_cast<int>(std::sin(WorldX * 0.21f + WorldY * 0.08f) * 10.0f);
                Ink = Color::Mix(Ink, Color::Rgb(0xF0, 0xBE, 0x6C), static_cast<std::uint8_t>(std::max(0, Ripple)));
            } else if (WorldY >= World.DistantGround(WorldX)) {
                const int Band = WorldY - World.DistantGround(WorldX);
                if (World.Season == TumbleSeason::Winter && Band < 2) {
                    Pixels[static_cast<std::size_t>(Y) * Columns + X] = Color::Rgb(0xD8, 0xE0, 0xE2);
                    continue;
                }
                const Color Mesa = World.Night() ? Color::Rgb(0x32, 0x32, 0x58)
                                                 : (Band % 9 < 3 ? Color::Rgb(0x8B, 0x57, 0x4E)
                                                                 : Color::Rgb(0x6E, 0x48, 0x50));
                Ink = World.Shade(Mesa, Sun + 80 - Band * 2);
            } else {
                Ink = World.SkyAt(WorldY, Sun);
            }
            if (World.Weather == TumbleWeather::Dust || World.Weather == TumbleWeather::Storm)
                Ink = Color::Mix(Ink, Color::Rgb(0xB7, 0x8B, 0x5D), 55);
            if (World.Weather == TumbleWeather::Rain || World.Weather == TumbleWeather::Storm)
                Ink = Color::Mix(Ink, Color::Rgb(0x65, 0x70, 0x79), World.Weather == TumbleWeather::Storm ? 120 : 78);
            Pixels[static_cast<std::size_t>(Y) * Columns + X] = Ink;
        }
    }
    Canvas.PutPixels(Pixels);
    PaintSun(Canvas, World, Columns);
    PaintClouds(Canvas, World);
}

void PaintLayer(Frame &Canvas, const TumbleWorld &World, int Layer)
{
    for (const TumbleRuin &One : World.Ruins) if (One.Layer == Layer) DrawRuin(Canvas, World, One);
    for (const TumbleRock &One : World.Rocks) if (One.Layer == Layer) DrawRock(Canvas, World, One);
    for (const TumbleCactus &One : World.Cacti) if (One.Layer == Layer) DrawCactus(Canvas, World, One);
    for (const TumbleWeed &One : World.Weeds) if (One.Layer == Layer) DrawWeed(Canvas, World, One);
    for (const TumbleBird &One : World.Birds) if (One.Layer == Layer) DrawBird(Canvas, World, One);
}

void PaintWeather(Frame &Canvas, const TumbleWorld &World)
{
    for (const TumbleDust &One : World.Dusts)
        Pixel(Canvas, World, static_cast<int>(One.X), static_cast<int>(One.Y), One.Ink);
    if (World.Weather != TumbleWeather::Rain && World.Weather != TumbleWeather::Storm) return;
    const int Drops = World.ViewWidth / 3 + (World.Weather == TumbleWeather::Storm ? World.ViewWidth / 3 : 0);
    for (int Drop = 0; Drop < Drops; Drop++) {
        const int X = static_cast<int>((static_cast<std::uint64_t>(Drop * 101 + World.Time * 80) * 17u) % static_cast<std::uint64_t>(World.ViewWidth));
        const int Y = static_cast<int>((static_cast<std::uint64_t>(Drop * 53 + World.Time * 130) * 13u) % static_cast<std::uint64_t>(World.ViewHeight));
        Line(Canvas, World, static_cast<int>(World.CameraX) + X, static_cast<int>(World.CameraY) + Y,
             static_cast<int>(World.CameraX) + X - 1, static_cast<int>(World.CameraY) + Y + 3,
             Color::Rgb(0x72, 0xA9, 0xC8));
    }
}

void PaintBar(Frame &Canvas, const TumbleWorld &World)
{
    const int Row = World.ViewHeight / 2;
    const TextStyle Bar{Color::Rgb(0xEE, 0xEE, 0xEE), Color::Rgb(0x3A, 0x3A, 0x3A)};
    const TextStyle Hint{Color::Rgb(0x8C, 0x8C, 0x8C), Bar.Background};
    Canvas.Fill({0, Row, World.ViewWidth, 1}, ' ', Bar);
    int Col = 1;
    Col += static_cast<int>(Canvas.Write({Col, Row}, "tumbleweed", Bar));
    Col += static_cast<int>(Canvas.Write({Col + 1, Row}, TumbleWeatherName(World.Weather), Hint)) + 2;
    Col += static_cast<int>(Canvas.Write({Col + 1, Row}, TumbleSeasonName(World.Season), Hint)) + 2;
    Col += static_cast<int>(Canvas.Write({Col + 1, Row}, TumbleLightName(World.Light), Hint)) + 2;
    if (World.Sunbeam) Col += static_cast<int>(Canvas.Write({Col + 1, Row}, "beam", Bar)) + 2;
    const std::string Count = std::format("{} spawned", World.Spawned);
    if (Col + static_cast<int>(Count.size()) + 2 < World.ViewWidth)
        Col += static_cast<int>(Canvas.Write({Col + 1, Row}, Count, Hint)) + 2;
    if (World.ShowKeys)
        Canvas.Write({Col + 1, Row}, "arrows move  space beam  w weather  e season  n light  s/c/b spawn  h hide  q leave", Hint);
}

}

void TumblePaint(const TumbleWorld &World, painter::tui::Frame &Canvas)
{
    PaintBackground(Canvas, World);
    PaintLayer(Canvas, World, 0);
    PaintLayer(Canvas, World, 1);
    PaintSeason(Canvas, World);
    PaintWeather(Canvas, World);
    PaintBar(Canvas, World);
}
