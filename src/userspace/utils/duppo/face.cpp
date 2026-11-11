#include "face.h"

#include <algorithm>

namespace duppo {

namespace {

constexpr float Snap      = 22.0f;    /* radians a second of the gaze's spring */
constexpr float Damping   = 0.75f;
constexpr float MaxStep   = 0.016f;
constexpr float Ahead     = 5.0f;     /* units a second at which it looks fully ahead */
constexpr float Most      = 0.95f;
constexpr float Near      = 12.0f;    /* logo units away at which a glance is full */

constexpr float Closing = 0.08f;      /* seconds a blink takes to shut, stay shut and open */
constexpr float Holding = 0.04f;
constexpr float Opening = 0.16f;
constexpr float Twice   = 0.18f;      /* the share of blinks that come in a pair */

}  // namespace

void Face::Begin(painter::core::Size Cells)
{
    Glass = {static_cast<float>(Cells.Width), static_cast<float>(Cells.Height) * 2.0f};
}

void Face::Tune(float Speed, float BlinkRate)
{
    Pace   = Speed;
    Blinks = BlinkRate;
    if (Blinks <= 0.0f) {
        Age   = -1.0f;
        Shut  = 0.0f;
    }
}

float Face::Lids(float Age)
{
    if (Age < Closing) return Smooth(Age / Closing);
    if (Age < Closing + Holding) return 1.0f;
    if (Age < Closing + Holding + Opening) return 1.0f - Smooth((Age - Closing - Holding) / Opening);
    return 0.0f;
}

void Face::Blink(float Seconds, Random &Dice)
{
    if (Blinks <= 0.0f) return;
    if (Age >= 0.0f) {
        Age += Seconds * Pace;
        if (Age >= Closing + Holding + Opening) {
            Age     = -1.0f;
            ToBlink = Dice.Chance(Twice) ? 0.12f : Dice.Between(2.2f, 6.5f) / Blinks;
        }
    } else {
        ToBlink -= Seconds;
        if (ToBlink <= 0.0f) Age = 0.0f;
    }
    Shut = Age >= 0.0f ? Lids(Age) : 0.0f;
}

/* A look somewhere on the glass, or at the place it is bound for. */
void Face::Glance(Random &Dice, const Flight &Body)
{
    ToGlance = Dice.Between(0.6f, 2.2f) / Pace;
    Looking  = Dice.Chance(0.3f) ? Body.Goal : Vec{Dice.Between(0.0f, Glass.X), Dice.Between(2.0f, Glass.Y)};
}

void Face::Move(float Seconds, const Flight &Body)
{
    Vec Want;
    if (Body.Travelling()) {
        Want = Normalised(Body.Velocity) * (std::min(Length(Body.Velocity) / Ahead, 1.0f) * Most);
    } else {
        Vec Toward = Looking - Body.Center;
        Want       = Normalised(Toward) * (std::min(Length(Toward) / (Body.Scale * Near), 1.0f) * Most);
    }
    for (float Left = Seconds; Left > 0.0f; Left -= MaxStep) {
        const float Part = std::min(Left, MaxStep);
        Quickness = Quickness + ((Want - Gaze) * (Snap * Snap) - Quickness * (2.0f * Damping * Snap)) * Part;
        Gaze      = Limited(Gaze + Quickness * Part, 1.0f);
    }
}

void Face::Step(float Seconds, Random &Dice, const Flight &Body)
{
    ToGlance -= Seconds;
    if (ToGlance <= 0.0f) Glance(Dice, Body);
    Move(Seconds, Body);
    Blink(Seconds, Dice);
}

}  // namespace duppo
