/* radar.cpp — the radar's picture (radar.h). */

#include "radar.h"

#include <algorithm>
#include <cmath>

namespace seabattle {

namespace {

constexpr float Pi = 3.14159265f;
constexpr float TwoPi = 6.2831853f;
constexpr float Tail = 1.5f;        // radians of fading light behind the beam
constexpr int   Echoes = 5;

// How far behind the beam a bearing is, 0 to a whole turn.
float Behind(float Beam, float Bearing)
{
    float Gap = Beam - Bearing;
    while (Gap < 0.0f) Gap += TwoPi;
    while (Gap >= TwoPi) Gap -= TwoPi;
    return Gap;
}

}  // namespace

void PaintRadar(Canvas &Out, float X, float Y, float Radius, Color Tone, std::uint64_t Ms, std::uint32_t Seed)
{
    const float Beam = static_cast<float>(Ms % 6000) / 6000.0f * TwoPi;
    const int   Reach = static_cast<int>(Radius) + 2;
    for (int Row = -Reach; Row <= Reach; Row++) {
        for (int Col = -Reach; Col <= Reach; Col++) {
            const float Across = static_cast<float>(Col) + 0.5f, Down = static_cast<float>(Row) + 0.5f;
            const float Far = std::sqrt(Across * Across + Down * Down);
            const int   PixelX = static_cast<int>(std::floor(X)) + Col, PixelY = static_cast<int>(std::floor(Y)) + Row;
            Out.Cover(PixelX, PixelY, Darker(Tone, 0.82f), Covered(Far - Radius) * 0.9f);
            if (Far >= Radius) continue;
            const float Edge = Radius - Far;
            if (Edge < 1.0f) Out.Cover(PixelX, PixelY, Tone, 0.8f);
            for (int Ring = 1; Ring <= 2; Ring++)
                Out.Cover(PixelX, PixelY, Tone, Covered(std::fabs(Far - Radius * static_cast<float>(Ring) / 3.0f) - 0.35f) * 0.35f);
            if (Col == 0 || Row == 0) Out.Cover(PixelX, PixelY, Tone, 0.22f);
            const float Gap = Behind(Beam, std::atan2(Down, Across) + Pi);
            if (Gap < Tail) Out.Cover(PixelX, PixelY, Lighter(Tone, 0.25f), (1.0f - Gap / Tail) * (1.0f - Gap / Tail) * 0.6f);
        }
    }
    for (int Echo = 0; Echo < Echoes; Echo++) {
        const float Bearing = Grain(static_cast<int>(Seed), Echo, 1) * TwoPi - Pi;
        const float Range = (0.25f + Grain(static_cast<int>(Seed), Echo, 2) * 0.65f) * Radius;
        const float Gap = Behind(Beam, Bearing + Pi);
        if (Gap > 4.5f) continue;
        Out.Cover(static_cast<int>(std::floor(X + std::cos(Bearing) * Range)), static_cast<int>(std::floor(Y + std::sin(Bearing) * Range)),
                  Rgb(0xFF, 0xFF, 0xFF), 1.0f - Gap / 4.5f);
    }
}

}  // namespace seabattle
