/* skyline.cpp — the clouds, the city, the bushes and the stars. See skyline.h.
 * Each is drawn from dice that are the same every game, so the city is the
 * player's city: the same houses each time it goes by. */

#include "skyline.h"

#include <algorithm>
#include <cstdint>

#include "world.h"

namespace flappybird::skyline {

namespace {

class Dice {
public:
    explicit Dice(std::uint32_t Seed) : State(Seed) {}
    float Between(float Least, float Most)
    {
        State ^= State << 13;
        State ^= State >> 17;
        State ^= State << 5;
        return Least + (Most - Least) * static_cast<float>(State % 10000u) / 10000.0f;
    }

private:
    std::uint32_t State;
};

/* Rounds along the period, each Step apart give or take, their tops about
 * Top; the last one rounds off into the first of the next period. */
void Puffs(Pattern &Into, Dice &Roll, float Top, float Step, float Least, float Most)
{
    for (float X = 0; X < Period; X += Roll.Between(Step * 0.7f, Step * 1.3f)) {
        const float R = Roll.Between(Least, Most);
        Into.Rounds.push_back(Round{X, Top + R + Roll.Between(-6, 6), R});
    }
    Into.Rounds.push_back(Round{Period, Into.Rounds.front().Y, Into.Rounds.front().R});
}

}  // namespace

Pattern Clouds()
{
    Pattern Made;
    Dice    Roll(0x5EEDC10Du);
    Puffs(Made, Roll, 300, 34, 18, 30);
    Made.Boxes.push_back(Box{0, 336, Period, field::Ground - 336});
    return Made;
}

Pattern City()
{
    Pattern Made;
    Dice    Roll(0xC17E5u);
    for (float X = 0; X < Period;) {
        const float Wide = std::min(Roll.Between(16, 32), Period - X);
        const float Top  = Roll.Between(326, 364);
        Made.Boxes.push_back(Box{X, Top, Wide, field::Ground - Top});
        X += Wide;
    }
    return Made;
}

Pattern Windows(const Pattern &City)
{
    Pattern Made;
    Dice    Roll(0x1176u);
    for (const Box &House : City.Boxes)
        for (float Y = House.Y + 4; Y + 3 < field::Ground - 8; Y += 6)
            for (float X = House.X + 3; X + 2.5f < House.X + House.Wide - 2; X += 5)
                if (Roll.Between(0, 1) < 0.6f) Made.Boxes.push_back(Box{X, Y, 2.5f, 3});
    return Made;
}

Pattern Bushes()
{
    Pattern Made;
    Dice    Roll(0xB005u);
    Puffs(Made, Roll, 368, 20, 11, 17);
    Made.Boxes.push_back(Box{0, 388, Period, field::Ground - 388});
    return Made;
}

Pattern Stars(float Wide, float Low)
{
    Pattern Made;
    Dice    Roll(0x57A25u);
    const int Count = static_cast<int>(Wide * Low / 1400);
    for (int I = 0; I < Count; I++) Made.Rounds.push_back(Round{Roll.Between(0, Wide), Roll.Between(4, Low), Roll.Between(0.5f, 1.3f)});
    return Made;
}

painter::core::Path Tiled(const Pattern &Once, float Unit, int Count)
{
    painter::core::Path Made;
    for (int Turn = 0; Turn < Count; Turn++) {
        const float From = static_cast<float>(Turn) * Period;
        for (const Round &One : Once.Rounds) Made.AddCircle({(From + One.X) * Unit, One.Y * Unit}, One.R * Unit);
        for (const Box &One : Once.Boxes) Made.AddRect({(From + One.X) * Unit, One.Y * Unit, One.Wide * Unit, One.Tall * Unit});
    }
    return Made;
}

}  // namespace flappybird::skyline
