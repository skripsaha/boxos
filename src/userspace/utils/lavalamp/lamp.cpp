/* lamp.cpp — the wax warming, rising, cooling and sinking. See lamp.h.
 *
 * Warmth is what moves a blob, as it is in the lamp on a desk: in the bottom
 * of the lamp a blob takes the bulb's warmth, under the cap it gives it up,
 * and in between it keeps what it has. Warmer than the liquid it is lifted,
 * cooler it sinks, and the liquid holds it back the faster it goes — so a
 * blob rises the whole height once it is warm, rests under the cap while it
 * cools, and sinks the whole way back, and how long each takes is its own. */

#include "lamp.h"

#include <algorithm>
#include <cmath>

#include "box/cpu.h"

namespace lavalamp {

namespace {

constexpr float BlobRadius = 0.095f;   // of the height, at size 100
constexpr float Buoyancy   = 0.13f;    // what full warmth lifts by, a second, a second
constexpr float Drag       = 1.6f;     // how the liquid holds a blob back, a second
constexpr float Wander     = 0.025f;   // how far a blob strays sideways, a second
constexpr float PoolDepth  = 0.065f;
constexpr float Longest    = 1.0f / 30;   // the longest step the wax is moved in
constexpr float Pi2        = 6.2831853f;

float Smooth(float Edge0, float Edge1, float X)
{
    const float T = std::clamp((X - Edge0) / (Edge1 - Edge0), 0.0f, 1.0f);
    return T * T * (3 - 2 * T);
}

/* The flow, 1 to 9, as times the lamp's own pace: 5 is it, each step a
 * quarter faster or slower. */
float FlowOf(int Flow)
{
    return std::pow(1.28f, static_cast<float>(Flow - 5));
}

}  // namespace

Lamp::Lamp(float Aspect, const Composition &Made) : Wide(Aspect), State(cpu_rdtsc() * 0x9E3779B97F4A7C15ull | 1u)
{
    Tune(Made);
}

float Lamp::Roll(float Least, float Most)
{
    State ^= State << 13;
    State ^= State >> 7;
    State ^= State << 17;
    return Least + (Most - Least) * static_cast<float>(static_cast<std::int64_t>(State >> 40)) / 16777216.0f;
}

/* A blob anywhere in the lamp, or warming in the pool at the bottom. */
void Lamp::Seed(Blob &One, bool FromPool)
{
    One.Size  = Roll(0.65f, 1.35f);
    One.Warms = Roll(3.5f, 8.0f);
    One.Lift  = Roll(0.8f, 1.25f);
    One.Phase = Roll(0, Pi2);
    const float R = Radius * One.Size;
    One.X     = Roll(std::min(R, Wide / 2), std::max(Wide - R, Wide / 2));
    One.Y     = FromPool ? 1.0f - R * 0.3f : Roll(0.15f, 0.9f);
    One.Heat  = FromPool ? 0.3f : Roll(0, 1);
    One.Vx    = Roll(-1, 1) * Wander;
    One.Vy    = 0;
    Shape(One);
}

void Lamp::Tune(const Composition &Made)
{
    Radius = BlobRadius * static_cast<float>(Made.Size) / 100.0f;
    Flow   = FlowOf(Made.Flow);
    const std::size_t Want = static_cast<std::size_t>(Made.Blobs);
    while (Wax.size() < Want) {
        Wax.emplace_back();
        Seed(Wax.back(), Time > 0);
    }
    Wax.resize(Want);
    for (Blob &One : Wax) Shape(One);
}

void Lamp::Reshape(float Aspect)
{
    for (Blob &One : Wax) One.X *= Aspect / Wide;
    Wide = Aspect;
}

float Lamp::PoolTop(float X) const noexcept
{
    const float Wave = 0.3f * std::sin(X * 5.1f + Time * 0.5f) + 0.2f * std::sin(X * 11.3f - Time * 0.8f);
    return 1.0f - PoolDepth * (1.0f + Wave);
}

/* Its radii now: stretched along the way it goes the faster it goes, its area
 * kept, and breathing slowly. */
void Lamp::Shape(Blob &One) const
{
    const float Breath  = 1.0f + 0.06f * std::sin(Time * 0.7f + One.Phase);
    const float Stretch = 1.0f + 0.55f * std::min(std::fabs(One.Vy) / (Buoyancy / Drag), 1.0f);
    const float R       = Radius * One.Size * Breath;
    One.Down   = R * std::sqrt(Stretch);
    One.Across = R / std::sqrt(Stretch);
}

void Lamp::Move(Blob &One, float Seconds)
{
    const float Bottom = Smooth(0.62f, 0.92f, One.Y), Top = Smooth(0.38f, 0.1f, One.Y);
    One.Heat = std::clamp(One.Heat + (Bottom * (1 - One.Heat) - Top * One.Heat) * 2 * Seconds / One.Warms, 0.0f, 1.0f);

    const float Lifted = -One.Lift * Buoyancy * std::tanh(4 * (One.Heat - 0.5f));
    One.Vy += (Lifted - Drag * One.Vy) * Seconds;
    const float Stray = Wander * std::sin(Time * 0.23f + One.Phase * 3) + 0.04f * (Wide / 2 - One.X) / Wide;
    One.Vx += (Stray - One.Vx) * 0.8f * Seconds;
    One.X += One.Vx * Seconds;
    One.Y += One.Vy * Seconds;

    /* Kept in the glass: against a side it turns back, under the cap and on
     * the bottom it rests. */
    const float Left = One.Across * 0.8f, Right = Wide - One.Across * 0.8f;
    if (One.X < Left) One.X = Left, One.Vx = std::fabs(One.Vx);
    if (One.X > Right) One.X = Right, One.Vx = -std::fabs(One.Vx);
    const float High = One.Down * 0.55f, Low = 1.0f - One.Down * 0.3f;
    if (One.Y < High) One.Y = High, One.Vy = std::max(One.Vy, 0.0f);
    if (One.Y > Low) One.Y = Low, One.Vy = std::min(One.Vy, 0.0f);
    Shape(One);
}

void Lamp::Advance(float Seconds)
{
    float Left = std::min(Seconds, 0.25f) * Flow;
    while (Left > 0) {
        const float Step = std::min(Left, Longest);
        Left -= Step;
        Time += Step;
        for (Blob &One : Wax) Move(One, Step);
    }
}

}  // namespace lavalamp
