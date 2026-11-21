/* world.cpp — the bird's flight, the pipes and the score. See world.h. */

#include "world.h"

#include <algorithm>
#include <cmath>

#include "box/cpu.h"

namespace flappybird {

namespace {

constexpr float Step      = 1.0f / 240;   // the world moves in steps this long, whatever the frames
constexpr float Gravity   = 1500;         // units a second, a second
constexpr float Lift      = -410;         // the speed one beat of the wings gives
constexpr float Fastest   = 620;          // falling no faster than this
constexpr float NoseUp    = -22;          // degrees, just after a beat
constexpr float NoseHeld  = 0.36f;        // seconds the nose stays up after a beat
constexpr float NoseDrops = 430;          // degrees a second it then turns down at
constexpr float Diving    = 90;
constexpr float Beats     = 8.5f;         // wing beats a second
constexpr float Hover     = 236;          // where the bird hovers, waiting
constexpr float Bob       = 5;            // how far it bobs about it
constexpr float FirstPipe = 300;          // the first pair, this far ahead of the bird
constexpr float Margin    = 56;           // the least room above and below a gap
constexpr float MostStep  = 150;          // a gap no further than this from the one before
constexpr float Pi2       = 6.2831853f;

}  // namespace

World::World(float Wide) : Width(Wide), Seed(cpu_rdtsc() * 0x9E3779B97F4A7C15ull | 1u)
{
    Begin();
}

void World::Begin()
{
    At         = Stage::Ready;
    Hero       = Bird{};
    Hero.X     = std::clamp(Width * 0.3f, 64.0f, 300.0f);
    Hero.Y     = Hover;
    Row.clear();
    Previous   = 0;
    Points     = 0;
    Time       = 0;
    Rest       = 0;
    Raised     = 0;
}

/* Where a gap's middle lies: anywhere with room above and below it, and not
 * so far from the one before that no flight could reach it. */
float World::NewMiddle()
{
    Seed ^= Seed << 13;
    Seed ^= Seed >> 7;
    Seed ^= Seed << 17;
    const float Unit  = static_cast<float>(static_cast<std::int64_t>(Seed >> 40)) / 16777216.0f;
    const float Least = Margin + field::Gap / 2, Most = field::Ground - Margin - field::Gap / 2;
    float       Middle = Least + (Most - Least) * Unit;
    if (Previous > 0) Middle = std::clamp(Middle, Previous - MostStep, Previous + MostStep);
    Previous = std::clamp(Middle, Least, Most);
    return Previous;
}

void World::Flap()
{
    if (At == Stage::Ready) {
        /* The pairs laid at once, the first not far ahead and the rest every
         * spacing on to past the right edge: the field may be wider than
         * the time anyone would wait for the first. */
        At = Stage::Flying;
        for (float X = Hero.X + FirstPipe; X < Width + 2 * field::Spacing; X += field::Spacing) {
            const float Middle = NewMiddle();
            Row.push_back(Pipe{X, Middle, false});
        }
    }
    if (At != Stage::Flying) return;
    Hero.Speed = Lift;
    Hero.Tilt  = NoseUp;
    Raised     = 0;
}

Happened World::Advance(float Seconds)
{
    Happened What;
    Rest += std::min(Seconds, 0.1f);
    while (Rest >= Step) {
        Rest -= Step;
        const Happened Once = Tick(Step);
        What.Scored |= Once.Scored;
        What.Struck |= Once.Struck;
        What.Landed |= Once.Landed;
        if (At == Stage::Over) break;
    }
    return What;
}

Happened World::Tick(float Seconds)
{
    Happened What;
    if (At == Stage::Over) return What;
    Time += Seconds;
    if (At != Stage::Falling) Hero.Wing = std::fmod(Hero.Wing + Seconds * Beats, 1.0f);
    if (At == Stage::Ready) {
        Hero.Y = Hover + Bob * std::sin(Time * Pi2 * 1.4f);
        Distance += field::Speed * Seconds;
        return What;
    }
    MoveBird(Seconds);
    if (At == Stage::Flying) {
        Distance += field::Speed * Seconds;
        MovePipes(Seconds, What);
        if (Strikes()) {
            What.Struck = true;
            At          = Stage::Falling;
        }
    }
    const float Floor = field::Ground - field::BirdTall / 2;
    if (Hero.Y >= Floor) {
        Hero.Y = Floor;
        if (At == Stage::Flying) What.Struck = true;
        What.Landed = true;
        At          = Stage::Over;
    }
    return What;
}

void World::MoveBird(float Seconds)
{
    Hero.Speed = std::min(Hero.Speed + Gravity * Seconds, Fastest);
    Hero.Y += Hero.Speed * Seconds;
    if (Hero.Y < field::BirdTall / 2) {
        Hero.Y     = field::BirdTall / 2;
        Hero.Speed = std::max(Hero.Speed, 0.0f);
    }
    Raised += Seconds;
    /* Struck, the bird turns straight down at once; flying, it holds its
     * nose up a moment after each beat and then dives. */
    const float Drop = At == Stage::Falling ? 3 * NoseDrops : Raised > NoseHeld ? NoseDrops : 0;
    Hero.Tilt        = std::min(Hero.Tilt + Drop * Seconds, Diving);
}

void World::MovePipes(float Seconds, Happened &What)
{
    const float Laid = static_cast<float>(Row.size()) * field::Spacing;
    for (Pipe &One : Row) {
        One.X -= field::Speed * Seconds;
        if (!One.Passed && One.X + field::PipeWide / 2 < Hero.X) {
            One.Passed  = true;
            What.Scored = true;
            Points++;
        }
    }
    /* A pair gone past the left edge comes round again behind the last. */
    for (Pipe &One : Row)
        if (One.X + field::CapWide < 0) One = Pipe{One.X + Laid, NewMiddle(), false};
}

/* Whether the bird's round touches a pipe: each pipe a body down to its gap
 * and a cap, a little wider, at the gap's edge. */
bool World::Strikes() const
{
    const auto Touches = [&](float Left, float Top, float Right, float Bottom) {
        const float Nearest   = std::clamp(Hero.X, Left, Right);
        const float NearestY  = std::clamp(Hero.Y, Top, Bottom);
        const float Dx = Hero.X - Nearest, Dy = Hero.Y - NearestY;
        return Dx * Dx + Dy * Dy < field::BirdReach * field::BirdReach;
    };
    const float Overhang = (field::CapWide - field::PipeWide) / 2;
    for (const Pipe &One : Row) {
        const float Upper = One.Middle - field::Gap / 2, Lower = One.Middle + field::Gap / 2;
        if (Touches(One.X, -field::Tall, One.X + field::PipeWide, Upper) ||
            Touches(One.X - Overhang, Upper - field::CapTall, One.X + field::PipeWide + Overhang, Upper) ||
            Touches(One.X, Lower, One.X + field::PipeWide, field::Ground) ||
            Touches(One.X - Overhang, Lower, One.X + field::PipeWide + Overhang, Lower + field::CapTall))
            return true;
    }
    return false;
}

}  // namespace flappybird
