/* flurry.cpp — leaves, confetti and sparks (flurry.h). */

#include "flurry.h"

#include <algorithm>
#include <cmath>

namespace hangman {

namespace {

constexpr std::size_t MostSpecks = 240;
constexpr float Gravity = 30.0f;
constexpr float FloorAt = 86.0f;

constexpr Color Bright[] = {Rgb(255, 99, 99), Rgb(255, 206, 86), Rgb(112, 224, 140), Rgb(96, 180, 255), Rgb(214, 128, 255), Rgb(255, 255, 255)};

}  // namespace

void Flurry::Add(const Speck &One)
{
    if (Specks.size() >= MostSpecks) Specks.erase(Specks.begin());
    Specks.push_back(One);
}

void Flurry::Leaf(Luck &Chance, float X, float Y, Color Ink)
{
    Add({X, Y, Chance.Between(2.0f, 5.0f), Chance.Between(0.5f, 1.5f), Chance.Unit(), Chance.Between(-0.8f, 0.8f), 0.0f,
         Chance.Between(7.0f, 12.0f), Ink, Kind::Leaf});
}

void Flurry::Confetti(Luck &Chance, float X, float Y, int Count)
{
    for (int One = 0; One < Count; One++) {
        const float Way = Chance.Between(-3.1415926f, 0.0f);
        const float Force = Chance.Between(14.0f, 42.0f);
        Add({X, Y, std::cos(Way) * Force, std::sin(Way) * Force, Chance.Unit(), Chance.Between(-2.0f, 2.0f), 0.0f,
             Chance.Between(2.4f, 4.4f), Bright[Chance.Below(std::size(Bright))], Kind::Confetti});
    }
}

void Flurry::Sparks(Luck &Chance, float X, float Y, int Count, Color Ink)
{
    for (int One = 0; One < Count; One++) {
        const float Way = Chance.Between(0.0f, 6.2831853f);
        const float Force = Chance.Between(3.0f, 11.0f);
        Add({X, Y, std::cos(Way) * Force, std::sin(Way) * Force, 0.0f, 0.0f, 0.0f, Chance.Between(0.4f, 0.9f), Ink, Kind::Spark});
    }
}

void Flurry::Step(std::uint32_t ElapsedMs, float Wind)
{
    const float Seconds = static_cast<float>(ElapsedMs) / 1000.0f;
    for (Speck &One : Specks) {
        One.Age += Seconds;
        One.Turn += One.Spin * Seconds;
        One.X += One.Vx * Seconds;
        One.Y += One.Vy * Seconds;
        switch (One.What) {
        case Kind::Leaf:
            One.Vx += (4.0f + 6.0f * Wind - One.Vx) * Seconds * 1.4f;
            One.Vy = 1.8f + 1.2f * Wave(One.Age * 0.7f + One.Spin);
            break;
        case Kind::Confetti:
            One.Vx -= One.Vx * Seconds * 1.6f;
            One.Vy += (Gravity - One.Vy * 2.2f) * Seconds;
            One.Vx += 3.0f * Wind * Seconds;
            break;
        case Kind::Spark:
            One.Vx -= One.Vx * Seconds * 2.5f;
            One.Vy -= One.Vy * Seconds * 2.5f;
            break;
        }
    }
    std::erase_if(Specks, [](const Speck &One) { return One.Age >= One.Life || One.Y > FloorAt; });
}

void Flurry::Paint(painter::tui::FineCanvas &Fine, const View &Where) const
{
    const float Thin = std::max(0.6f, Where.Span(0.9f));
    for (const Speck &One : Specks) {
        const painter::core::PointF At = Where.At(One.X, One.Y);
        const float                 Left = 1.0f - One.Age / One.Life;
        switch (One.What) {
        case Kind::Leaf: {
            const float Reach = Where.Span(1.1f);
            const float Lean = std::cos(One.Turn * 6.2831853f) * Reach, Rise = std::sin(One.Turn * 6.2831853f) * Reach;
            Fine.Line({At.X - Lean, At.Y - Rise}, {At.X + Lean, At.Y + Rise}, Thin, One.Ink);
            break;
        }
        case Kind::Confetti: {
            const float Flutter = std::cos(One.Turn * 6.2831853f);
            const float Reach = Where.Span(1.3f);
            Fine.Line({At.X - Reach * Flutter, At.Y}, {At.X + Reach * Flutter, At.Y}, Thin, One.Ink, Clamped(Left * 3.0f));
            break;
        }
        case Kind::Spark:
            Fine.Disc(At.X, At.Y, std::max(0.4f, Where.Span(0.9f * Left)), One.Ink, Clamped(Left * 1.6f));
            break;
        }
    }
}

}  // namespace hangman
