/* light.cpp — the lamps' maps and the sum of their light. */

#include "light.h"

#include <algorithm>
#include <cmath>

namespace wizard {

namespace {

constexpr int Full = 256;

std::uint8_t RoundLight(const Lamp &Of, int X, int Y)
{
    const float Dx = static_cast<float>(X) + 0.5f - Of.X, Dy = (static_cast<float>(Y) + 0.5f - Of.Y) * 1.15f;
    const float Near = 1.0f - std::sqrt(Dx * Dx + Dy * Dy) / Of.Reach;
    if (Near <= 0.0f) return 0;
    return static_cast<std::uint8_t>(std::min(255.0f, Near * Near * 255.0f));
}

std::uint8_t ShaftLight(const Lamp &Of, int X, int Y)
{
    const float Down = static_cast<float>(Y) + 0.5f - Of.Y;
    if (Down < 0.0f || Down > Of.Reach) return 0;
    const float Middle = Of.X + Of.Aim * Down;
    const float Half = Of.Width * 0.5f + Down * 0.18f;
    const float Across = std::fabs(static_cast<float>(X) + 0.5f - Middle) / Half;
    if (Across >= 1.0f) return 0;
    const float Edge = 1.0f - Across * Across * Across;
    const float Fade = 1.0f - Down / Of.Reach;
    return static_cast<std::uint8_t>(Edge * Fade * 255.0f);
}

}  // namespace

void Lighting::Build(int Wide, int High, const std::vector<Lamp> &Lamps, Tint Dim)
{
    Width = Wide;
    Height = High;
    Ambient = Dim;
    Shades.clear();
    Levels.assign(Lamps.size(), Full);
    Weights.assign(Lamps.size(), Tint{});
    Maps.assign(Lamps.size(), std::vector<std::uint8_t>(static_cast<std::size_t>(Wide) * High));
    for (std::size_t Which = 0; Which < Lamps.size(); Which++) {
        const Lamp &Of = Lamps[Which];
        Shades.push_back(Tint{Of.Shade.Red(), Of.Shade.Green(), Of.Shade.Blue()});
        for (int Y = 0; Y < High; Y++)
            for (int X = 0; X < Wide; X++)
                Maps[Which][static_cast<std::size_t>(Y) * Wide + X] = Of.Kind == LampKind::Round ? RoundLight(Of, X, Y) : ShaftLight(Of, X, Y);
        Set(Which, Full);
    }
}

void Lighting::Set(std::size_t Which, int Level)
{
    Levels[Which] = Level;
    Weights[Which] = Tint{Shades[Which].Red * Level >> 8, Shades[Which].Green * Level >> 8, Shades[Which].Blue * Level >> 8};
}

Tint Lighting::At(int X, int Y) const
{
    X = std::clamp(X, 0, Width - 1);
    Y = std::clamp(Y, 0, Height - 1);
    const std::size_t Place = static_cast<std::size_t>(Y) * Width + X;
    Tint Sum = Ambient;
    for (std::size_t Which = 0; Which < Maps.size(); Which++) {
        const int Near = Maps[Which][Place];
        Sum.Red += Near * Weights[Which].Red >> 8;
        Sum.Green += Near * Weights[Which].Green >> 8;
        Sum.Blue += Near * Weights[Which].Blue >> 8;
    }
    return Sum;
}

void Lighting::Apply(const Canvas &Under, Canvas &Out) const
{
    const std::span<const Color> From = Under.All();
    for (int Y = 0; Y < Height; Y++) {
        for (int X = 0; X < Width; X++) {
            const Tint Light = At(X, Y);
            Out.At(X, Y) = Scaled(From[static_cast<std::size_t>(Y) * Width + X], Light.Red, Light.Green, Light.Blue);
        }
    }
}

}  // namespace wizard
