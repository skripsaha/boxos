/* air.cpp — the visitors and the fall. */

#include "air.h"

#include <algorithm>
#include <cmath>

#include "growth.h"

namespace garden {

namespace {

using painter::core::Colors::Black;
using painter::core::Colors::White;

constexpr Ink Glow  = Ink::Rgb(0xFF, 0xE9, 0x7A);
constexpr Ink Honey = Ink::Rgb(0xF2, 0xB8, 0x2B);

std::int32_t Round(double Value) noexcept
{
    return static_cast<std::int32_t>(std::lround(Value));
}

std::uint8_t Part(double Share) noexcept
{
    return static_cast<std::uint8_t>(std::clamp(Share, 0.0, 1.0) * 255.0);
}

// A number of the plant's own, 0 to 1, for each thing about its visitors.
double Own(const Haunt &Over, std::uint32_t Which) noexcept
{
    return Unit(Scatter(Over.Seed + Which * 101u));
}

void Butterfly(const Pen &Draw, const Palette &Colors, const Haunt &Over, double Time)
{
    const double Beat = Time * (0.6 + 0.4 * Own(Over, 1)) + Own(Over, 2) * 20.0;
    const std::int32_t X = Over.Top.X + Round(std::sin(Beat * 1.1) * 7.0 + std::sin(Beat * 2.3) * 2.0);
    const std::int32_t Y = Over.Top.Y - 3 + Round(std::sin(Beat * 1.7 + 1.0) * 4.0 + std::cos(Beat * 0.7) * 2.0);
    const Ink Wing = Colors.Bloom[static_cast<std::uint32_t>(Own(Over, 3) * 3.0) % 3];
    Draw.Dot(X, Y, Black);
    if (static_cast<std::int64_t>(Time * 7.0 + Own(Over, 4) * 8.0) % 2 == 0) {
        Draw.Dot(X - 1, Y - 1, Wing);
        Draw.Dot(X + 1, Y - 1, Wing);
    } else {
        Draw.Dot(X, Y - 1, Wing);
    }
}

void Bee(const Pen &Draw, const Haunt &Over, double Time)
{
    const double Beat = Time + Own(Over, 5) * 30.0;
    const std::int32_t X = Over.Top.X + Round(std::cos(Beat * 3.1) * 5.0 + std::sin(Beat * 5.3) * 1.5);
    const std::int32_t Y = Over.Top.Y + Round(std::sin(Beat * 2.3) * 4.0) - 1;
    Draw.Dot(X, Y, Honey);
    Draw.Dot(X + 1, Y, Black);
    Draw.Veil(X, Y - 1, White, 170);
}

void Firefly(const Pen &Draw, const Haunt &Over, std::uint32_t Which, double Time)
{
    const double Beat = Time + Own(Over, 10 + Which) * 40.0;
    const std::int32_t X = Over.Top.X + Round(std::sin(Beat * 0.5) * 9.0);
    const std::int32_t Y = Over.Top.Y + 2 + Round(std::sin(Beat * 0.37 + Which) * 6.0);
    const double Pulse = 0.5 + 0.5 * std::sin(Beat * 2.1);
    if (Pulse < 0.2) return;
    Draw.Dot(X, Y, Ink::Between(Glow, White, Part(Pulse * 0.5)));
    for (const std::int32_t Step : {-1, 1}) {
        Draw.Veil(X + Step, Y, Glow, Part(Pulse * 0.35));
        Draw.Veil(X, Y + Step, Glow, Part(Pulse * 0.35));
    }
}

}  // namespace

void PaintVisitors(const Pen &Draw, const Palette &Colors, std::span<const Haunt> Over, double Time)
{
    const bool ByDay = Colors.Lit.Day > 0.45;
    for (const Haunt &One : Over) {
        if (ByDay) {
            Butterfly(Draw, Colors, One, Time);
            if (One.First) Bee(Draw, One, Time);
        } else {
            Firefly(Draw, One, 0, Time);
            Firefly(Draw, One, 1, Time);
        }
    }
}

void PaintFall(const Pen &Draw, const Palette &Colors, double Time)
{
    if (Colors.Lit.Year == Season::Summer) return;
    const Scene &Area  = Draw.Of();
    const double Width = Area.Width;
    const double Fall  = std::max(1, Area.Ground - Area.Top);
    for (std::uint32_t Piece = 0; Piece < static_cast<std::uint32_t>(Area.Width / 9); Piece++) {
        const double Speed = 2.5 + 3.0 * Unit(Scatter(Piece * 3 + 3));
        const double X = std::fmod(Unit(Scatter(Piece * 3 + 1)) * Width + std::sin(Time * 0.8 + Piece) * 3.0 + Time * 1.5, Width);
        const double Y = std::fmod(Unit(Scatter(Piece * 3 + 2)) * Fall + Time * Speed, Fall);
        const std::int32_t Across = Round(X);
        const std::int32_t Down   = Area.Top + Round(Y);
        Draw.Dot(Across, Down, Colors.Drift);
        if (Colors.Lit.Year == Season::Autumn) Draw.Dot(Across + 1, Down, Colors.Drift);
    }
}

}  // namespace garden
