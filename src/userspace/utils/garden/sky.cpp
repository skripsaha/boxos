/* sky.cpp — the hour and the season, the colors they give, and the sky, the
 * hills and the grass drawn in them. */

#include "sky.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "box/clock.h"

#include "growth.h"

namespace garden {

namespace {

using painter::core::Colors::Black;
using painter::core::Colors::White;

constexpr Ink SunInk   = Ink::Rgb(0xFF, 0xE2, 0x7A);
constexpr Ink SetInk   = Ink::Rgb(0xFF, 0x8A, 0x3C);
constexpr Ink MoonInk  = Ink::Rgb(0xE6, 0xEA, 0xF2);
constexpr Ink CloudInk = Ink::Rgb(0xF2, 0xF6, 0xF8);
constexpr Ink Pale     = Ink::Rgb(0xBF, 0xE3, 0xF0);
constexpr Ink Ember    = Ink::Rgb(0xFF, 0xD8, 0xA0);
constexpr std::int32_t StarCount  = 90;
constexpr std::int32_t CloudCount = 4;

std::uint8_t Part(double Share) noexcept
{
    return static_cast<std::uint8_t>(std::clamp(Share, 0.0, 1.0) * 255.0);
}

double Smooth(double From, double To, double At) noexcept
{
    const double Share = std::clamp((At - From) / (To - From), 0.0, 1.0);
    return Share * Share * (3.0 - 2.0 * Share);
}

Ink Between(Ink A, Ink B, double Share) noexcept
{
    return Ink::Mix(A, B, Part(Share));
}

Season SeasonOfMonth(int Month) noexcept
{
    if (Month >= 3 && Month <= 5) return Season::Spring;
    if (Month >= 6 && Month <= 8) return Season::Summer;
    if (Month >= 9 && Month <= 11) return Season::Autumn;
    return Season::Winter;
}

// A body that crosses the sky in twelve hours, left to right: where it is
// when Progress is 0 to 1; false when it is not over the horizon.
bool Arc(const Scene &Area, double Progress, std::int32_t &X, std::int32_t &Y) noexcept
{
    if (Progress <= -0.08 || Progress >= 1.08) return false;
    const double Rise = std::sin(std::numbers::pi * std::clamp(Progress, 0.0, 1.0));
    X = static_cast<std::int32_t>(Progress * Area.Width);
    Y = Area.Horizon() - static_cast<std::int32_t>(Rise * (Area.Horizon() - Area.Top - 5));
    return true;
}

void PaintGradient(const Pen &Draw, const Light &Lit)
{
    const Scene &Area = Draw.Of();
    const double Span = std::max(1, Area.Ground - Area.Top);
    for (std::int32_t Y = Area.Top; Y < Area.Ground; Y++) {
        const double Down = (Y - Area.Top) / Span;
        const Ink    Row  = Between(Lit.Zenith, Lit.Low, Down * Down);
        for (std::int32_t X = 0; X < Area.Width; X++) Draw.Dot(X, Y, Row);
    }
}

void PaintStars(const Pen &Draw, const Light &Lit, double Time)
{
    const double Night = 1.0 - Lit.Day;
    if (Night < 0.3) return;
    const Scene &Area = Draw.Of();
    const std::int32_t Height = std::max(1, Area.Horizon() - Area.Top - 3);
    for (std::int32_t Star = 0; Star < StarCount; Star++) {
        const auto Place = static_cast<std::uint32_t>(Star);
        const std::int32_t X = static_cast<std::int32_t>(Scatter(Place * 2 + 1) % static_cast<std::uint32_t>(Area.Width));
        const std::int32_t Y = Area.Top + static_cast<std::int32_t>(Scatter(Place * 2 + 2) % static_cast<std::uint32_t>(Height));
        const double Glint = 0.5 + 0.5 * std::sin(Time * 1.7 + Star);
        Draw.Veil(X, Y, White, Part(Night * (0.25 + 0.75 * Glint)));
    }
}

void PaintSun(const Pen &Draw, const Light &Lit)
{
    std::int32_t X = 0, Y = 0;
    if (Lit.Day < 0.05 || !Arc(Draw.Of(), (Lit.Hours - 6.0) / 12.0, X, Y)) return;
    const Ink Body = Between(SunInk, SetInk, Lit.Twilight);
    for (std::int32_t Dy = -6; Dy <= 6; Dy++)
        for (std::int32_t Dx = -6; Dx <= 6; Dx++) {
            const std::int32_t Away = Dx * Dx + Dy * Dy;
            if (Away <= 9) Draw.Dot(X + Dx, Y + Dy, Body);
            else if (Away <= 36) Draw.Veil(X + Dx, Y + Dy, Body, Part(0.5 * (1.0 - std::sqrt(Away) / 6.5)));
        }
}

void PaintMoon(const Pen &Draw, const Light &Lit)
{
    const double Night = 1.0 - Lit.Day;
    std::int32_t X = 0, Y = 0;
    const double Progress = Lit.Hours >= 12 ? (Lit.Hours - 18.0) / 12.0 : (Lit.Hours + 6.0) / 12.0;
    if (Night < 0.2 || !Arc(Draw.Of(), Progress, X, Y)) return;
    for (std::int32_t Dy = -4; Dy <= 4; Dy++)
        for (std::int32_t Dx = -4; Dx <= 4; Dx++) {
            const bool Disc = Dx * Dx + Dy * Dy <= 16;
            const bool Bite = (Dx - 3) * (Dx - 3) + (Dy + 1) * (Dy + 1) <= 12;
            if (Disc && !Bite) Draw.Veil(X + Dx, Y + Dy, MoonInk, Part(Night));
        }
}

void PaintClouds(const Pen &Draw, const Light &Lit, double Time)
{
    const Scene &Area = Draw.Of();
    const Ink    Body = Between(Between(Ink::Rgb(0x2C, 0x34, 0x52), CloudInk, Lit.Day), Lit.Low, Lit.Twilight * 0.6);
    const std::int32_t Room = std::max(1, (Area.Horizon() - Area.Top) / 2);
    for (std::int32_t Cloud = 0; Cloud < CloudCount; Cloud++) {
        const auto   Place = static_cast<std::uint32_t>(Cloud);
        const double Path  = Area.Width + 24.0;
        const double Along = std::fmod(Unit(Scatter(Place + 77)) * Path + Time * (0.7 + 0.25 * Cloud), Path) - 12.0;
        const std::int32_t X = static_cast<std::int32_t>(Along);
        const std::int32_t Y = Area.Top + 2 + static_cast<std::int32_t>(Scatter(Place + 99) % static_cast<std::uint32_t>(Room));
        for (std::int32_t Dx = 2; Dx <= 5; Dx++) Draw.Dot(X + Dx, Y, Body);
        for (std::int32_t Dx = 1; Dx <= 7; Dx++) Draw.Dot(X + Dx, Y + 1, Body);
        for (std::int32_t Dx = 0; Dx <= 8; Dx++) Draw.Dot(X + Dx, Y + 2, Between(Body, Lit.Zenith, 0.25));
    }
}

}  // namespace

Light LightNow(const Look &Shown)
{
    BoxTime Now{};
    const bool Known = clock_boxtime_local(&Now) == 0;
    Light      Lit;
    switch (Shown.Sky) {
    case Hour::Day:   Lit.Hours = 12.0; break;
    case Hour::Dusk:  Lit.Hours = 18.25; break;
    case Hour::Night: Lit.Hours = 0.5; break;
    case Hour::Clock: Lit.Hours = Known ? Now.hour + Now.minute / 60.0 + Now.second / 3600.0 : 12.0; break;
    }
    Lit.Year     = Shown.Year != Season::Clock ? Shown.Year : Known ? SeasonOfMonth(Now.month) : Season::Summer;
    Lit.Sun      = std::sin(2.0 * std::numbers::pi * (Lit.Hours - 6.0) / 24.0);
    Lit.Day      = Smooth(-0.2, 0.3, Lit.Sun);
    Lit.Twilight = Smooth(0.0, 1.0, 1.0 - std::abs(Lit.Sun + 0.05) / 0.35);
    Lit.Zenith   = Between(Between(Shown.Night, Shown.Day, Lit.Day), Shown.Dusk, Lit.Twilight * 0.5);
    const Ink Glow = Between(Between(Shown.Night, Pale, Lit.Day), Between(Shown.Dusk, Ember, 0.5), Lit.Twilight * 0.9);
    Lit.Low        = Between(Lit.Zenith, Glow, 0.6 + 0.25 * Lit.Twilight);
    return Lit;
}

Palette PaletteIn(const Look &Shown, const Light &Lit)
{
    Ink Grass = Shown.Grass;
    Ink Leaf  = Shown.Leaf;
    Ink Drift = White;
    switch (Lit.Year) {
    case Season::Spring: Leaf = Between(Leaf, Ink::Rgb(0xA6, 0xE3, 0x6B), 0.25); Drift = Ink::Rgb(0xFF, 0xB7, 0xD1); break;
    case Season::Autumn: Leaf = Between(Leaf, Ink::Rgb(0xD9, 0x8A, 0x2B), 0.4);  Drift = Ink::Rgb(0xD9, 0x82, 0x2B); break;
    case Season::Winter: Leaf = Between(Leaf, Ink::Rgb(0x8F, 0xB3, 0xA8), 0.45);
                         Grass = Between(Grass, Ink::Rgb(0xE8, 0xF0, 0xF2), 0.45); break;
    case Season::Summer:
    case Season::Clock:  break;
    }
    // By night everything darkens toward the night, which has hardly any color left,
    // so a plant keeps its hue — and its 16 colors, where the glass has no more.
    const double Dark     = (1.0 - Lit.Day) * 0.55;
    const Ink    Dim      = Between(Shown.Night, Black, 0.65);
    const auto   Darkened = [&](Ink Color) { return Between(Color, Dim, Dark); };
    return Palette{.Grass    = Darkened(Grass),
                   .GrassTop = Darkened(Between(Grass, White, 0.2)),
                   .Soil     = Darkened(Shown.Soil),
                   .Stem     = Darkened(Between(Shown.Leaf, Black, 0.4)),
                   .Leaf     = Darkened(Leaf),
                   .Shade    = Darkened(Between(Leaf, Black, 0.4)),
                   .Wilted   = Darkened(Shown.Wilted),
                   .Fruit    = Darkened(Shown.Fruit),
                   .Bloom    = {Darkened(Shown.BloomA), Darkened(Shown.BloomB), Darkened(Shown.BloomC)},
                   .Drift    = Lit.Year == Season::Winter ? White : Darkened(Drift),
                   .Lit      = Lit};
}

void PaintSky(const Pen &Draw, const Light &Lit, double Time)
{
    PaintGradient(Draw, Lit);
    PaintStars(Draw, Lit, Time);
    PaintSun(Draw, Lit);
    PaintMoon(Draw, Lit);
    PaintClouds(Draw, Lit, Time);
}

void PaintGrass(const Pen &Draw, const Palette &Colors)
{
    const Scene &Area = Draw.Of();
    const Ink    Far  = Between(Colors.Grass, Colors.Lit.Low, 0.35);
    const Ink    Near = Between(Colors.Grass, Colors.Lit.Low, 0.15);
    for (std::int32_t X = 0; X < Area.Width; X++) {
        const double Across = X;
        const auto   FarTop = static_cast<std::int32_t>(Area.Ground - 6 - 3.0 * std::sin(Across * 0.09 + 0.7) - 2.0 * std::sin(Across * 0.23));
        const auto   NearTop = static_cast<std::int32_t>(Area.Ground - 2 - 1.5 * std::sin(Across * 0.13 + 2.0));
        for (std::int32_t Y = FarTop; Y < Area.Ground; Y++) Draw.Dot(X, Y, Far);
        for (std::int32_t Y = NearTop; Y < Area.Ground; Y++) Draw.Dot(X, Y, Near);
        Draw.Dot(X, Area.Ground, Colors.GrassTop);
        for (std::int32_t Y = Area.Ground + 1; Y < Area.SoilTop; Y++) Draw.Dot(X, Y, Colors.Grass);
        if (Scatter(static_cast<std::uint32_t>(X) * 31 + 5) % 4 == 0) Draw.Dot(X, Area.Ground - 1, Colors.GrassTop);
    }
}

}  // namespace garden
