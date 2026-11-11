/* wheel.cpp — the pockets and the ball's flight (wheel.h). */

#include "wheel.h"

#include <algorithm>
#include <cmath>

#include "trig.h"

namespace casino {

namespace {

constexpr int Order[Pockets] = {0,  32, 15, 19, 4,  21, 2,  25, 17, 34, 6,  27, 13, 36, 11, 30, 8, 23,
                                10, 5,  24, 16, 33, 1,  20, 14, 31, 9,  22, 18, 29, 7,  28, 12, 35, 3, 26};

constexpr std::uint64_t RedNumbers = (1ull << 1) | (1ull << 3) | (1ull << 5) | (1ull << 7) | (1ull << 9) | (1ull << 12) |
                                     (1ull << 14) | (1ull << 16) | (1ull << 18) | (1ull << 19) | (1ull << 21) |
                                     (1ull << 23) | (1ull << 25) | (1ull << 27) | (1ull << 30) | (1ull << 32) |
                                     (1ull << 34) | (1ull << 36);

constexpr float PocketWidth = TwoPi / Pockets;

// Where the ball runs the rim, and where it falls to: fractions of the radius.
constexpr float TrackRadius  = 0.83f;
constexpr float TrackInward  = 0.03f;
constexpr float PocketRadius = 0.46f;

// The drop is cut at these points of its length: the ball leaves the cone for
// the pockets, then lands, bounces, and lies. Each bounce is smaller.
constexpr float FallsAt           = 0.20f;
constexpr float Impacts[8]        = {0.0f, 0.20f, 0.38f, 0.54f, 0.68f, 0.80f, 0.91f, 1.0f};
constexpr float BounceHeights[6]  = {0.9f, 0.6f, 0.38f, 0.22f, 0.12f, 0.05f};
constexpr float KickSizes[6]      = {0.45f, 0.32f, 0.22f, 0.14f, 0.08f, 0.04f};

float Smooth(float From)
{
    const float Clamped = std::clamp(From, 0.0f, 1.0f);
    return Clamped * Clamped * (3.0f - 2.0f * Clamped);
}

}  // namespace

int NumberAt(int Slot)
{
    return Order[((Slot % Pockets) + Pockets) % Pockets];
}

int SlotOf(int Number)
{
    for (int Slot = 0; Slot < Pockets; Slot++)
        if (Order[Slot] == Number) return Slot;
    return 0;
}

Hue HueOf(int Number)
{
    if (Number == 0) return Hue::Green;
    return (RedNumbers >> Number) & 1 ? Hue::Red : Hue::Black;
}

std::string_view HueName(Hue Of)
{
    return Of == Hue::Green ? "green" : Of == Hue::Red ? "red" : "black";
}

Spin::Spin(int Result, Luck &Chance)
    : Number(Result),
      TrackSeconds(Chance.Between(4.6f, 5.6f)),
      DropSeconds(Chance.Between(3.0f, 3.4f)),
      StopSeconds(TrackSeconds + DropSeconds + 1.6f),
      WheelSpeed(Chance.Between(1.1f, 1.6f)),
      PocketAngle(SlotOf(Result) * PocketWidth)
{
    DropSpeed = Chance.Between(3.6f, 4.6f);
    const float WheelThen = WheelSpeed * (1.0f - TrackSeconds / StopSeconds);
    DropReach             = (DropSpeed + WheelThen) * DropSeconds / 2.0f;

    // The ball, turning against the wheel, must be at DropReach short of its
    // pocket (on the wheel) when the drop begins: the whole turns it makes on
    // the track are what is left free to choose, and its speed follows.
    const float Needed = -(WheelAt(TrackSeconds) + PocketAngle + DropReach);
    const float Base   = Needed - TwoPi * std::floor(Needed / TwoPi);
    const float Wanted = (Chance.Between(13.0f, 17.0f) + DropSpeed) * TrackSeconds / 2.0f;
    const float Turns  = std::round((Wanted - Base) / TwoPi);
    TrackSpeed         = 2.0f * (Base + TwoPi * Turns) / TrackSeconds - DropSpeed;

    const float Side = Chance.Unit() < 0.5f ? -1.0f : 1.0f;
    for (int Bounce = 0; Bounce < 6; Bounce++)
        Kick[Bounce] = Side * ((Bounce & 1) ? -1.0f : 1.0f) * KickSizes[Bounce] * Chance.Between(0.7f, 1.3f);
}

float Spin::WheelAt(float Seconds) const
{
    const float Time = std::min(Seconds, StopSeconds);
    return WheelSpeed * (Time - Time * Time / (2.0f * StopSeconds));
}

// How far the ball is off its pocket's place at this point of the drop: it
// lands somewhere new at each impact and moves between them without a jerk.
float Spin::Rattle(float Along) const
{
    for (int Part = 0; Part < 7; Part++) {
        if (Along > Impacts[Part + 1]) continue;
        const float From = Part == 0 ? 0.0f : Kick[Part - 1];
        const float To   = Part == 6 ? 0.0f : Kick[Part];
        return From + (To - From) * Smooth((Along - Impacts[Part]) / (Impacts[Part + 1] - Impacts[Part]));
    }
    return 0.0f;
}

// How high the ball is above the pockets: a parabola between two impacts.
float Spin::Hop(float Along) const
{
    for (int Bounce = 0; Bounce < 6; Bounce++) {
        const float Begin = Impacts[Bounce + 1];
        const float End   = Bounce == 5 ? 1.0f : Impacts[Bounce + 2];
        if (Along < Begin) return 0.0f;
        if (Along > End) continue;
        const float Within = (Along - Begin) / (End - Begin);
        return BounceHeights[Bounce] * 4.0f * Within * (1.0f - Within);
    }
    return 0.0f;
}

Flight Spin::At(float Seconds) const
{
    Flight Now;
    Now.WheelAngle = WheelAt(Seconds);
    if (Seconds < TrackSeconds) {
        const float Along = Seconds / TrackSeconds;
        Now.BallAngle     = -(TrackSpeed * Seconds - (TrackSpeed - DropSpeed) * Seconds * Seconds / (2.0f * TrackSeconds));
        Now.BallRadius    = TrackRadius - TrackInward * Along * Along * Along;
        return Now;
    }
    Now.OnTrack = false;
    if (Seconds >= TrackSeconds + DropSeconds) {
        Now.BallAngle  = Now.WheelAngle + PocketAngle;
        Now.BallRadius = PocketRadius;
        Now.Landed     = true;
        return Now;
    }
    const float Along = (Seconds - TrackSeconds) / DropSeconds;
    const float Left  = 1.0f - Along;
    Now.BallAngle     = Now.WheelAngle + PocketAngle + DropReach * Left * Left + Rattle(Along);
    Now.BallRadius    = (TrackRadius - TrackInward) + (PocketRadius - TrackRadius + TrackInward) * Smooth(Along / FallsAt);
    Now.Lift          = Hop(Along);
    return Now;
}

}  // namespace casino
