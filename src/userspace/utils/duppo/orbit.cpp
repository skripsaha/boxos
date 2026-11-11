#include "orbit.h"

#include <algorithm>
#include <cmath>

namespace duppo {

namespace {

constexpr float MaxStep = 0.016f;
constexpr float Damping = 0.4f;

/* How much of Duppo's own turn a bubble goes with, and how near the glass's
 * plane the nearest and farthest of them look larger and smaller. */
constexpr float Follow      = 0.55f;
constexpr float Nearness    = 0.10f;

/* Each bubble's own way: the spring's pitch, how far it swings round the
 * upright, across and in the plane, how fast, how much it breathes, where in
 * its swing it starts, and how far in front of the ball's middle it rests (the
 * medium one lies on the ball's edge, in front of it, as the logo has it). */
struct Manner {
    float Spring;
    float Yaw, Pitch, Roll;
    float Rate;
    float Breath;
    float Phase;
    float Front;
};

constexpr Manner Manners[Logo::MoonCount] = {
    {6.5f, 0.35f, 0.20f, 0.06f, 0.9f, 0.05f, 0.0f, 12.0f},
    {8.5f, 0.45f, 0.25f, 0.09f, 1.3f, 0.05f, 2.1f, 0.0f},
    {10.5f, 0.55f, 0.30f, 0.11f, 1.1f, 0.05f, 4.0f, 0.0f},
};

}  // namespace

Orbit::Home Orbit::Of(int Which, Vec Center, float Size, const Spin &Turning) const
{
    const Manner &Way  = Manners[Which];
    const Vec     Rest = Logo::Moons[Which].Center - Logo::Body.Center;
    const float   Wave = Clock * Way.Rate;
    const Rotation Swing = Rotation::Of(Follow * Turning.Yaw + Way.Yaw * std::sin(Wave + Way.Phase),
                                        Follow * Turning.Pitch + Way.Pitch * std::sin(0.8f * Wave + 1.7f * Way.Phase),
                                        Follow * Turning.Roll + Way.Roll * std::sin(1.3f * Wave + Way.Phase));
    const float   Breath = 1.0f + Way.Breath * std::sin(0.7f * Wave + Way.Phase);
    const Vec3    Place  = Swing.Turned({Rest.X * Breath, Rest.Y * Breath, Way.Front});
    const float   Near   = 1.0f + Nearness * Place.Z / Logo::Body.Radius;
    return Home{Center + Vec{Place.X, Place.Y} * (Size * Near), Place.Z * Size, Near};
}

void Orbit::Begin(Vec Center, float Size)
{
    const Spin Still;
    for (int At = 0; At < Logo::MoonCount; At++) {
        const Home Rest = Of(At, Center, Size, Still);
        Moons[At]       = Moon{Rest.Center, {}, Rest.Depth, Rest.Grow};
    }
}

void Orbit::Tune(float Speed, int Wanted)
{
    Pace  = Speed;
    Count = std::clamp(Wanted, 0, Logo::MoonCount);
}

void Orbit::Step(float Seconds, Vec Center, float Size, const Spin &Turning)
{
    Clock += Seconds * Pace;
    Home Homes[Logo::MoonCount];
    for (int At = 0; At < Logo::MoonCount; At++) Homes[At] = Of(At, Center, Size, Turning);

    for (float Left = Seconds; Left > 0.0f; Left -= MaxStep) {
        const float Part = std::min(Left, MaxStep);
        for (int At = 0; At < Logo::MoonCount; At++) {
            const float Spring = Manners[At].Spring;
            Moon       &One    = Moons[At];
            Vec Pull = (Homes[At].Center - One.Center) * (Spring * Spring) - One.Velocity * (2.0f * Damping * Spring);
            One.Velocity = One.Velocity + Pull * Part;
            One.Center   = One.Center + One.Velocity * Part;
        }
    }
    for (int At = 0; At < Logo::MoonCount; At++) {
        const float Ease = std::min(1.0f, Seconds * Manners[At].Spring);
        Moons[At].Depth += (Homes[At].Depth - Moons[At].Depth) * Ease;
        Moons[At].Grow   = Homes[At].Grow * (1.0f + 0.05f * std::sin(Clock * 1.7f + Manners[At].Phase));
    }
}

}  // namespace duppo
