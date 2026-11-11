/* toss.cpp — the coin's flight. */

#include "toss.h"

#include <algorithm>
#include <cmath>

namespace coinflip {

namespace {

constexpr double Pi = 3.14159265358979323846;
// Each bounce keeps this part of the speed it fell with: so much of the time
// in the air, and its square of the height.
constexpr double Keeps = 0.55;
// Two seconds from the hand to rest, at a speed of 100.
constexpr double WholeSeconds = 2.0;

double Weight(int Arc)
{
    return std::pow(Keeps, Arc);
}

}  // namespace

Toss::Toss(Face Result, const Look &Shown, bool Quick)
    : Arcs(Shown.Bounces + 1)
{
    double Sum = 0.0;
    for (int Arc = 0; Arc < Arcs; Arc++) Sum += Weight(Arc);
    const double Faster = static_cast<double>(Shown.Speed) / 100.0 * (Quick ? static_cast<double>(Shown.Quicker) / 100.0 : 1.0);
    ArcSeconds          = WholeSeconds / Sum / Faster;
    Total               = WholeSeconds / Faster;
    const int Spins     = Quick ? std::max(2, Shown.Spins / 2) : Shown.Spins;
    Turns               = 2.0 * Pi * Spins + (Result == Face::Tails ? Pi : 0.0);
}

Pose Toss::At(double Seconds) const
{
    if (Seconds >= Total) return Pose{0.0, Turns, true};
    const double Done = std::max(Seconds, 0.0) / Total;
    const double Turn = Turns * (1.0 - (1.0 - Done) * (1.0 - Done));

    double Left = std::max(Seconds, 0.0);
    for (int Arc = 0; Arc < Arcs; Arc++) {
        const double Length = ArcSeconds * Weight(Arc);
        if (Left < Length || Arc == Arcs - 1) {
            const double Along = std::min(Left / Length, 1.0);
            const double Peak  = Weight(Arc) * Weight(Arc);
            return Pose{Peak * 4.0 * Along * (1.0 - Along), Turn, false};
        }
        Left -= Length;
    }
    return Pose{0.0, Turns, true};
}

}  // namespace coinflip
