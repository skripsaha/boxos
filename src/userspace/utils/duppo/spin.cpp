#include "spin.h"

#include <algorithm>
#include <cmath>

namespace duppo {

namespace {

constexpr float Snap    = 5.5f;     /* radians a second of the turn's spring */
constexpr float Damping = 0.55f;
constexpr float MaxStep = 0.016f;

constexpr float MostYaw   = 0.95f;  /* radians: about 54 degrees, the eye still on the face */
constexpr float MostPitch = 0.6f;
constexpr float MostRoll  = 0.25f;

}  // namespace

/* A turn to look at something: a held yaw and pitch, and then back. */
void Spin::Look(Random &Dice)
{
    LookYaw   = Dice.Between(-0.65f, 0.65f);
    LookPitch = Dice.Between(-0.4f, 0.4f);
    Holding   = Dice.Between(0.8f, 1.8f);
    ToLook    = Dice.Between(3.0f, 8.0f);
}

void Spin::Step(float Seconds, Random &Dice, const Flight &Body)
{
    const float Alive = Seconds * Pace;
    Clock += Alive;
    ToLook -= Alive;
    Holding -= Alive;
    if (ToLook <= 0.0f) Look(Dice);
    if (Holding <= 0.0f) LookYaw = LookPitch = 0.0f;

    const Vec Going = Limited(Body.Velocity / Flight::Fastest, 1.0f);
    const float Want[3] = {
        std::clamp(0.40f * Going.X + LookYaw + 0.24f * std::sin(0.37f * Clock) + 0.14f * std::sin(0.91f * Clock + 2.0f), -MostYaw, MostYaw),
        std::clamp(0.40f * Going.Y + LookPitch + 0.16f * std::sin(0.53f * Clock + 1.7f) + 0.10f * std::sin(1.13f * Clock), -MostPitch, MostPitch),
        std::clamp(0.12f * std::sin(0.43f * Clock + 0.6f) - 0.10f * Going.X, -MostRoll, MostRoll),
    };
    float *Angle[3] = {&Yaw, &Pitch, &Roll};
    for (float Left = Seconds; Left > 0.0f; Left -= MaxStep) {
        const float Part = std::min(Left, MaxStep);
        for (int At = 0; At < 3; At++) {
            Rate[At] += ((Want[At] - *Angle[At]) * (Snap * Snap) - Rate[At] * (2.0f * Damping * Snap)) * Part;
            *Angle[At] += Rate[At] * Part;
        }
    }
    Pose = Rotation::Of(Yaw, Pitch, Roll);
}

}  // namespace duppo
