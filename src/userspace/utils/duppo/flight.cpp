#include "flight.h"

#include <algorithm>
#include <cmath>

#include "logo.h"

namespace duppo {

namespace {

/* How much room it keeps beyond the logo's reach, for the bubbles' swing. */
constexpr float Margin = 1.12f;

/* The first row belongs to the display's clock and load. */
constexpr float TopRow = 2.0f;

constexpr float Hardest   = 26.0f;   /* units a second, a second */
constexpr float Eagerness = 0.85f;   /* the spring toward its goal, radians a second */
constexpr float Softness  = 0.95f;   /* its damping, 1 being none to spare */
constexpr float Arrived   = 3.0f;    /* units from the goal that count as there */
constexpr float Moving    = 2.5f;    /* units a second that count as underway */
constexpr float MaxStep   = 0.016f;

constexpr float Jelly         = 10.0f;    /* radians a second of its wobble */
constexpr float JellyDamping  = 0.35f;
constexpr float StretchGoing  = 0.03f;
constexpr float StretchTurn   = 0.04f;
constexpr float MostStretch   = 0.06f;

Vec Clamped(Vec At, Vec Low, Vec High)
{
    return {std::clamp(At.X, Low.X, High.X), std::clamp(At.Y, Low.Y, High.Y)};
}

}  // namespace

float Flight::Largest(painter::core::Size Cells)
{
    float Across = static_cast<float>(Cells.Width) / ((Logo::ReachLeft + Logo::ReachRight) * Margin);
    float Down   = (static_cast<float>(Cells.Height) * 2.0f - TopRow) / ((Logo::ReachUp + Logo::ReachDown) * Margin);
    return std::min(Across, Down);
}

void Flight::Begin(painter::core::Size Cells, float Size, Random &Dice)
{
    Glass = {static_cast<float>(Cells.Width), static_cast<float>(Cells.Height) * 2.0f};
    Fit(Size);
    Place    = {Dice.Between(Low.X, High.X), Dice.Between(Low.Y, High.Y)};
    Center   = Place;
    Velocity = {};
    Choose(Dice);
}

void Flight::Fit(float Size)
{
    Scale = Size;
    Low   = {Logo::ReachLeft * Size * Margin, TopRow + Logo::ReachUp * Size * Margin};
    High  = {Glass.X - Logo::ReachRight * Size * Margin, Glass.Y - Logo::ReachDown * Size * Margin};
    if (High.X < Low.X) Low.X = High.X = (Low.X + High.X) / 2.0f;
    if (High.Y < Low.Y) Low.Y = High.Y = (Low.Y + High.Y) / 2.0f;
    Place = Clamped(Place, Low, High);
    Goal  = Clamped(Goal, Low, High);
}

bool Flight::Travelling() const
{
    return Length(Velocity) > Moving;
}

void Flight::Choose(Random &Dice)
{
    const float Wide = 0.3f * Length(High - Low);
    Vec         Best = Place;
    float       Far  = -1.0f;
    for (int Try = 0; Try < 6 && Far < Wide; Try++) {
        Vec   Candidate{Dice.Between(Low.X, High.X), Dice.Between(Low.Y, High.Y)};
        float Away = Length(Candidate - Place);
        if (Away > Far) {
            Far  = Away;
            Best = Candidate;
        }
    }
    Goal    = Best;
    Leg     = Dice.Between(3.0f, 7.0f) / Pace;
    Resting = false;
}

/* A goal reached is held a while or passed by; one not reached in time is
 * given up for another, so the way bends and is not a line from place to place. */
void Flight::Settle(Random &Dice)
{
    if (Resting) {
        if (Dwell <= 0.0f) Choose(Dice);
        return;
    }
    if (Leg <= 0.0f) {
        Choose(Dice);
    } else if (Length(Goal - Place) < Arrived) {
        if (Dice.Chance(0.6f)) {
            Resting = true;
            Dwell   = Dice.Between(1.0f, 3.5f) / Pace;
        } else {
            Choose(Dice);
        }
    }
}

/* One small step of the spring that draws it to its goal, and of the jelly
 * that follows the pull: stretched along its way by its speed and along the
 * pull by how hard it is turned, and quivering back to round. */
void Flight::Give(float Seconds)
{
    const float Eager = Eagerness * Pace;
    Vec Pull = (Goal - Place) * (Eager * Eager) - Velocity * (2.0f * Softness * Eager);
    Pull     = Limited(Pull, Hardest * Pace);

    Velocity = Limited(Velocity + Pull * Seconds, Fastest * Pace);
    Place    = Clamped(Place + Velocity * Seconds, Low, High);

    const float Going = std::min(Length(Velocity) / (Fastest * Pace), 1.0f) * StretchGoing;
    const float Turn  = std::min(Length(Pull) / (Hardest * Pace), 1.0f) * StretchTurn;
    const Vec   Along = Normalised(Velocity), Bent = Normalised(Pull);
    const float Want[3] = {Going * Along.X * Along.X + Turn * Bent.X * Bent.X,
                           Going * Along.X * Along.Y + Turn * Bent.X * Bent.Y,
                           Going * Along.Y * Along.Y + Turn * Bent.Y * Bent.Y};
    for (std::size_t At = 0; At < Squash.size(); At++) {
        Quiver[At] += (Jelly * Jelly * (Want[At] - Squash[At]) - 2.0f * JellyDamping * Jelly * Quiver[At]) * Seconds;
        Squash[At]  = std::clamp(Squash[At] + Quiver[At] * Seconds, -MostStretch, MostStretch);
    }
}

/* The body's squash, taken back: the inverse of (1 + stretch) kept to the same
 * area, so what is stretched one way is narrowed the other. */
void Flight::Shape()
{
    const float Xx = 1.0f + Squash[0], Yy = 1.0f + Squash[2], Xy = Squash[1];
    const float Root = std::sqrt(std::max(Xx * Yy - Xy * Xy, 0.5f));
    Warp = {Yy / Root, -Xy / Root, -Xy / Root, Xx / Root};
}

void Flight::Step(float Seconds, Random &Dice)
{
    Clock += Seconds * Pace;
    Leg   -= Seconds;
    if (Resting) Dwell -= Seconds;

    for (float Left = Seconds; Left > 0.0f; Left -= MaxStep) Give(std::min(Left, MaxStep));
    Settle(Dice);
    Shape();
    Center = Place + Vec{std::sin(Clock * 0.9f + 1.3f) * 0.6f, std::sin(Clock * 1.4f) * 1.3f} * Scale;
}

}  // namespace duppo
