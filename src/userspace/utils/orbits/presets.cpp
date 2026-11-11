// presets.cpp — the five skies.

#include "presets.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace orbits {

namespace {

constexpr const char *Names[PresetCount] = {"solar", "binary", "eight", "chaos", "lone"};

constexpr double SunMass = 1000.0;

// A small deterministic dice: the same seed makes the same sky.
class Dice {
public:
    explicit Dice(std::uint64_t Seed) : State(Seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}

    // From 0 up to, not including, 1.
    double Next()
    {
        State ^= State << 13;
        State ^= State >> 7;
        State ^= State << 17;
        return static_cast<double>(State >> 11) / 9007199254740992.0;
    }

    double Between(double Low, double High) { return Low + (High - Low) * Next(); }

private:
    std::uint64_t State;
};

double Fraction(double Value)
{
    return Value - std::floor(Value);
}

// The tone of the Nth body: spread so that neighbours differ.
double ToneOf(std::size_t N)
{
    return Fraction(0.12 + 0.618034 * static_cast<double>(N));
}

Vec Round(double Angle)
{
    return {std::cos(Angle), std::sin(Angle)};
}

// Counter-clockwise from a direction.
Vec Around(Vec Direction)
{
    return {-Direction.Y, Direction.X};
}

// A body Distance from Centre at Angle, in a circular orbit round Mass, itself
// going at Along; Turn is 1 counter-clockwise, -1 the other way.
void Orbiting(Sky &Into, Vec Centre, Vec Along, double Mass, double Distance, double Angle, double Own, double Tone,
              double Turn = 1.0, double Speed = 1.0)
{
    const Vec Out = Round(Angle);
    Into.Add(Centre + Out * Distance, Along + Around(Out) * (Into.CircularSpeed(Distance, Mass) * Speed * Turn), Own,
             Tone);
}

void MakeSolar(Sky &Into, Dice &Roll)
{
    constexpr double Masses[] = {0.4, 0.8, 1.0, 0.5, 2.0, 1.2};
    Into.Add({}, {}, SunMass, 0.0);
    double Distance = 0.30;
    for (std::size_t Planet = 0; Planet < std::size(Masses); Planet++, Distance *= 1.42)
        Orbiting(Into, {}, {}, SunMass, Distance, Roll.Between(0, 2 * std::numbers::pi), Masses[Planet],
                 ToneOf(Planet), 1.0, Roll.Between(0.99, 1.01));

    // A moon of the giant, well inside the reach of its own pull.
    const Body Giant = Into.Bodies[5];
    Orbiting(Into, Giant.At, Giant.Speed, Giant.Mass, 0.04, Roll.Between(0, 2 * std::numbers::pi), 0.02, 0.9);

    // A comet from far out and back round the sun the wrong way: at the far
    // end of its ellipse, the speed that brings it to 0.15 from the sun.
    const double Far = 1.5, Near = 0.15;
    const double Pull = Into.CircularSpeed(1.0, SunMass);
    const double Speed = Pull * std::sqrt(2.0 * Near / (Far * (Far + Near)));
    const Vec    Out   = Round(Roll.Between(0, 2 * std::numbers::pi));
    Into.Add(Out * Far, Around(Out) * -Speed, 0.005, 0.5);
}

void MakeBinary(Sky &Into, Dice &Roll)
{
    constexpr double Apart = 0.6;
    const double     Pace = Into.CircularSpeed(Apart, SunMass) / 2;
    const Vec        First{-Apart / 2, 0}, Second{Apart / 2, 0};
    const Vec        FirstGoes{0, -Pace}, SecondGoes{0, Pace};
    Into.Add(First, FirstGoes, SunMass / 2, 0.0);
    Into.Add(Second, SecondGoes, SunMass / 2, 0.55);
    // One that keeps to the first star, and two that go round them both.
    Orbiting(Into, First, FirstGoes, SunMass / 2, 0.16, Roll.Between(0, 2 * std::numbers::pi), 1.0, 0.3);
    Orbiting(Into, {}, {}, SunMass, 1.3, Roll.Between(0, 2 * std::numbers::pi), 1.0, 0.7);
    Orbiting(Into, {}, {}, SunMass, 1.75, Roll.Between(0, 2 * std::numbers::pi), 3.0, 0.9);
}

// Three equal bodies chasing one another along one figure-eight: the orbit
// Chenciner and Montgomery proved in 2000, in its published starting state,
// scaled to a size and a mass that suit the sky.
void MakeEight(Sky &Into)
{
    constexpr double Size = 0.8, Product = 0.6;
    const double     Mass = Product / Into.Strength();
    const double     Pace = std::sqrt(Product / Size);
    const Vec        Place{0.97000436, -0.24308753};
    const Vec        Goes{0.466203685, 0.43236573};
    Into.Add(Place * -Size, Goes * Pace, Mass, 0.0);
    Into.Add(Place * Size, Goes * Pace, Mass, 0.4);
    Into.Add({}, Goes * (-2.0 * Pace), Mass, 0.75);
}

// A star and a crowd of heavy bodies on orbits that cross, a few of them
// backwards: they scatter one another, fall into the star, merge and are
// thrown out.
void MakeChaos(Sky &Into, Dice &Roll)
{
    constexpr double Heart = 600.0;
    Into.Add({}, {}, Heart, 0.0);
    for (std::size_t N = 0; N < 16; N++) {
        const double Turn = Roll.Next() < 0.8 ? 1.0 : -1.0;
        Orbiting(Into, {}, {}, Heart, Roll.Between(0.3, 1.6), Roll.Between(0, 2 * std::numbers::pi),
                 std::exp(Roll.Next() * std::log(6.0)), ToneOf(N), Turn, Roll.Between(0.82, 1.2));
    }
}

}  // namespace

const char *PresetName(std::size_t Which)
{
    return Names[Which];
}

std::size_t PresetNamed(const char *Name)
{
    for (std::size_t Which = 0; Which < PresetCount; Which++)
        if (std::strcmp(Name, Names[Which]) == 0) return Which;
    return PresetCount;
}

void Prepare(Sky &Into, std::size_t Which, std::uint64_t Seed)
{
    Dice Roll(Seed);
    Into.Clear();
    switch (Which) {
    case 0: MakeSolar(Into, Roll); break;
    case 1: MakeBinary(Into, Roll); break;
    case 2: MakeEight(Into); break;
    case 3: MakeChaos(Into, Roll); break;
    default: Into.Add({}, {}, SunMass, 0.0); break;
    }
    Into.Settle();
}

}  // namespace orbits
