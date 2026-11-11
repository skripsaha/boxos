// sky.cpp — gravity, touches and what leaves.

#include "sky.h"

#include <algorithm>
#include <cmath>

namespace orbits {

double RadiusOf(double Mass)
{
    return 0.01 * std::cbrt(Mass);
}

bool Sky::Add(Vec At, Vec Speed, double Mass, double Tone)
{
    if (Bodies.size() >= MostBodies) return false;
    Bodies.push_back(Body{NextId++, At, Speed, Mass, RadiusOf(Mass), Tone, At});
    PullKnown = false;
    return true;
}

void Sky::Clear()
{
    Bodies.clear();
    Collisions.clear();
    Merged    = 0;
    Escaped   = 0;
    PullKnown = false;
}

void Sky::SetGravity(double Strength)
{
    Gravity   = Strength;
    PullKnown = false;
}

// The pull on every body: each pair once, softened so that a pass through a
// body costs a bounded kick.
void Sky::Pull()
{
    Pulled.assign(Bodies.size(), Vec{});
    const double Softer = Softening * Softening;
    for (std::size_t First = 0; First < Bodies.size(); First++) {
        for (std::size_t Second = First + 1; Second < Bodies.size(); Second++) {
            const Vec    Between = Bodies[Second].At - Bodies[First].At;
            const double Square  = Between.X * Between.X + Between.Y * Between.Y + Softer;
            const double Factor  = Gravity / (Square * std::sqrt(Square));
            Pulled[First]  = Pulled[First] + Between * (Bodies[Second].Mass * Factor);
            Pulled[Second] = Pulled[Second] - Between * (Bodies[First].Mass * Factor);
        }
    }
    PullKnown = true;
}

void Sky::Kick(double Seconds)
{
    for (std::size_t At = 0; At < Bodies.size(); At++) Bodies[At].Speed = Bodies[At].Speed + Pulled[At] * Seconds;
}

void Sky::Advance(double Seconds)
{
    if (!PullKnown) Pull();
    Kick(Seconds / 2);
    for (Body &One : Bodies) One.At = One.At + One.Speed * Seconds;
    Pull();
    Kick(Seconds / 2);
    Merge();
    Leave();
}

// Two bodies that touch become one at their centre of mass, with their
// momentum: the heavier gives it its tone and its name.
void Sky::Join(std::size_t Kept, std::size_t Taken)
{
    const Body  &Heavy = Bodies[Kept].Mass >= Bodies[Taken].Mass ? Bodies[Kept] : Bodies[Taken];
    const Body  &Light = &Heavy == &Bodies[Kept] ? Bodies[Taken] : Bodies[Kept];
    const double Mass  = Heavy.Mass + Light.Mass;
    Collisions.push_back(Collision{Light.At, Light.Mass, Heavy.Tone, Heavy.IsStar()});
    Body Joined   = Heavy;
    Joined.At     = (Heavy.At * Heavy.Mass + Light.At * Light.Mass) * (1.0 / Mass);
    Joined.Speed  = (Heavy.Speed * Heavy.Mass + Light.Speed * Light.Mass) * (1.0 / Mass);
    Joined.Mass   = Mass;
    Joined.Radius = RadiusOf(Mass);
    Bodies[Kept]  = Joined;
    Bodies.erase(Bodies.begin() + static_cast<std::ptrdiff_t>(Taken));
    Merged++;
    PullKnown = false;
}

void Sky::Merge()
{
    for (std::size_t First = 0; First < Bodies.size(); First++) {
        for (std::size_t Second = First + 1; Second < Bodies.size();) {
            const Vec    Between = Bodies[Second].At - Bodies[First].At;
            const double Reach   = Bodies[First].Radius + Bodies[Second].Radius;
            if (Between.X * Between.X + Between.Y * Between.Y > Reach * Reach) {
                Second++;
                continue;
            }
            Join(First, Second);
            Second = First + 1;
        }
    }
}

void Sky::Leave()
{
    const std::size_t Before = Bodies.size();
    std::erase_if(Bodies, [](const Body &One) {
        return One.At.X * One.At.X + One.At.Y * One.At.Y > EscapeRadius * EscapeRadius;
    });
    if (Bodies.size() == Before) return;
    Escaped += Before - Bodies.size();
    PullKnown = false;
}

void Sky::Settle()
{
    double Mass = 0.0;
    Vec    Centre, Speed;
    for (const Body &One : Bodies) {
        Mass += One.Mass;
        Centre = Centre + One.At * One.Mass;
        Speed  = Speed + One.Speed * One.Mass;
    }
    if (Mass <= 0.0) return;
    Centre = Centre * (1.0 / Mass);
    Speed  = Speed * (1.0 / Mass);
    for (Body &One : Bodies) {
        One.At    = One.At - Centre;
        One.Speed = One.Speed - Speed;
        One.Drawn = One.At;
    }
    PullKnown = false;
}

double Sky::CircularSpeed(double Distance, double Mass) const
{
    const double Square = Distance * Distance + Softening * Softening;
    return std::sqrt(Gravity * Mass * Distance * Distance / (Square * std::sqrt(Square)));
}

double Sky::StarsMass() const
{
    double Mass = 0.0;
    for (const Body &One : Bodies)
        if (One.IsStar()) Mass += One.Mass;
    return Mass;
}

Forecast Sky::Foretell(Vec At, Vec Speed, double Mass, std::size_t Places, int Steps, double Seconds) const
{
    Forecast Ahead;
    Sky      Copy = *this;
    if (!Copy.Add(At, Speed, Mass, 0.0)) return Ahead;
    const std::uint32_t Thrown = Copy.Bodies.back().Id;
    for (std::size_t Place = 0; Place < Places; Place++) {
        for (int Step = 0; Step < Steps; Step++) Copy.Advance(Seconds);
        const auto Found = std::find_if(Copy.Bodies.begin(), Copy.Bodies.end(),
                                        [&](const Body &One) { return One.Id == Thrown; });
        if (Found == Copy.Bodies.end()) {
            Ahead.Ends = true;
            break;
        }
        Ahead.Places.push_back(Found->At);
    }
    return Ahead;
}

}  // namespace orbits
