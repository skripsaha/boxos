/* skyline.cpp — the clouds, the city, the trees, the bushes and the stars.
 * See skyline.h. Each is drawn from dice that are the same every game, so the
 * city is the player's city: the same houses each time it goes by. */

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

/* A heaped cloud standing on Base about X: one big round, and on either side
 * two smaller ones, lower, each a size down from the one before. */
void Head(Pattern &Into, Dice &Roll, float X, float Base, float Big)
{
    const float R = Roll.Between(Big * 0.85f, Big * 1.15f);
    Into.Rounds.push_back(Round{X, Base - R * 0.55f, R});
    for (const float Side : {-1.0f, 1.0f}) {
        float At = X, Size = R;
        for (int Puff = 0; Puff < 2; Puff++) {
            const float Next = Size * Roll.Between(0.55f, 0.72f);
            At += Side * (Size + Next) * Roll.Between(0.48f, 0.6f);
            Into.Rounds.push_back(Round{At, Base - Next * 0.45f, Next});
            Size = Next;
        }
    }
}

/* A bank of heads along the period, Apart give or take, low rounds between
 * them, and under them all a band down to the ground. */
Pattern Bank(std::uint32_t Seed, float Base, float Apart, float Big)
{
    Pattern Made;
    Dice    Roll(Seed);
    for (float X = Roll.Between(0, Apart / 4); X < Period; X += Roll.Between(Apart * 0.8f, Apart * 1.2f)) {
        Head(Made, Roll, X, Base, Big);
        const float Low = Roll.Between(Big * 0.36f, Big * 0.5f);
        Made.Rounds.push_back(Round{X + Apart / 2, Base - Low * 0.2f, Low});
    }
    Made.Boxes.push_back(Box{0, Base - 2, Period, field::Ground - Base + 2});
    return Made;
}

/* A round tree: a crown of four rounds, the biggest on top, on a trunk. */
void Leafy(Woods &Into, Dice &Roll, float X)
{
    const float Top = Roll.Between(318, 336), R = Roll.Between(11, 14);
    Into.Crowns.Rounds.push_back(Round{X, Top + R, R});
    Into.Crowns.Rounds.push_back(Round{X - 0.78f * R, Top + 1.6f * R, 0.74f * R});
    Into.Crowns.Rounds.push_back(Round{X + 0.8f * R, Top + 1.55f * R, 0.72f * R});
    Into.Crowns.Rounds.push_back(Round{X - 0.08f * R, Top + 2.0f * R, 0.8f * R});
    const float From = Top + 2.2f * R;
    Into.Trunks.Boxes.push_back(Box{X - 1.8f, From, 3.6f, field::Ground - From});
}

/* A fir: three tiers, each narrower and higher than the one under it. */
void Fir(Woods &Into, Dice &Roll, float X)
{
    const float Base = Roll.Between(372, 380), Tall = Roll.Between(50, 64), Wide = Roll.Between(22, 28);
    for (int Tier = 0; Tier < 3; Tier++) {
        const float Up = static_cast<float>(Tier);
        Into.Crowns.Spires.push_back(Spire{X, Base - Up * Tall * 0.26f, Wide * (1 - 0.24f * Up), Tall * (0.46f - 0.06f * Up), false});
    }
    Into.Trunks.Boxes.push_back(Box{X - 1.5f, Base - 3, 3, field::Ground - Base + 3});
}

/* The pattern laid From pixels across, Above the field's height at the top. */
void Lay(painter::core::Path &Into, const Pattern &Once, float Unit, float From, float Above)
{
    for (const Round &One : Once.Rounds) Into.AddCircle({From + One.X * Unit, (One.Y - Above) * Unit}, One.R * Unit);
    for (const Box &One : Once.Boxes) Into.AddRect({From + One.X * Unit, (One.Y - Above) * Unit, One.Wide * Unit, One.Tall * Unit});
    /* Wound as the boxes are, so the shapes of one path add up. */
    for (const Spire &One : Once.Spires) {
        const float Middle = From + One.X * Unit, Half = One.Wide / 2 * Unit, In = One.Wide * 0.14f * Unit;
        const float Base = (One.Base - Above) * Unit, Top = (One.Base - One.Tall - Above) * Unit;
        const float Bend = (One.Base - One.Tall * 0.28f - Above) * Unit;
        Into.MoveTo({Middle - Half, Base}).QuadTo({Middle - In, Bend}, {Middle, Top});
        if (One.Lit) Into.LineTo({Middle + One.Wide * 0.04f * Unit, Base});
        else Into.QuadTo({Middle + In, Bend}, {Middle + Half, Base});
        Into.Close();
    }
}

}  // namespace

Pattern FarClouds()
{
    return Bank(0xFA2C10u, 322, 60, 24);
}

Pattern Clouds()
{
    return Bank(0x5EEDC10Du, 344, 84, 26);
}

/* Two small clouds a period, high in the sky: a flat foot of little rounds
 * and a head on it. */
Pattern Floating()
{
    Pattern Made;
    Dice    Roll(0xF10A7u);
    for (int One = 0; One < 2; One++) {
        const float X = static_cast<float>(One) * Period / 2 + Roll.Between(10, 90);
        const float Base = Roll.Between(110, 230), Wide = Roll.Between(36, 60);
        for (float At = X; At <= X + Wide; At += 6) Made.Rounds.push_back(Round{At, Base - 6.5f, 6.5f});
        const float Middle = X + Wide * Roll.Between(0.35f, 0.6f), R = Roll.Between(11, 15);
        Made.Rounds.push_back(Round{Middle, Base - R * 0.9f, R});
        for (const float Side : {-1.0f, 1.0f}) {
            const float Small = R * Roll.Between(0.6f, 0.72f);
            Made.Rounds.push_back(Round{Middle + Side * R * 0.95f, Base - Small * 0.8f, Small});
        }
    }
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

/* Four trees a period, each in its own quarter of it, two round to every
 * fir or so. */
Woods Trees()
{
    Woods Made;
    Dice  Roll(0x7EE5u);
    for (int Quarter = 0; Quarter < 4; Quarter++) {
        const float X = (static_cast<float>(Quarter) + Roll.Between(0.25f, 0.75f)) * Period / 4;
        if (Roll.Between(0, 1) < 0.45f) Fir(Made, Roll, X);
        else Leafy(Made, Roll, X);
    }
    return Made;
}

/* Bushes shoulder to shoulder, each four rounds heaped, their feet in a band
 * down to the ground. */
Pattern Bushes()
{
    Pattern Made;
    Dice    Roll(0xB005u);
    for (float X = 0; X < Period; X += Roll.Between(22, 34)) {
        const float Top = Roll.Between(364, 374), R = Roll.Between(8, 11);
        Made.Rounds.push_back(Round{X, Top + R, R});
        Made.Rounds.push_back(Round{X - 0.9f * R, Top + 1.5f * R, 0.75f * R});
        Made.Rounds.push_back(Round{X + 0.95f * R, Top + 1.45f * R, 0.72f * R});
        Made.Rounds.push_back(Round{X + 0.1f * R, Top + 1.9f * R, 0.85f * R});
    }
    Made.Boxes.push_back(Box{0, 384, Period, field::Ground - 384});
    return Made;
}

/* Up and to the left inside each round by as much as stays inside it. A lit
 * half has no lights of its own. */
Pattern Lights(const Pattern &Of, float Share)
{
    Pattern Made;
    for (const Round &One : Of.Rounds) {
        const float By = (1 - Share) * 0.9f * One.R;
        Made.Rounds.push_back(Round{One.X - 0.6f * By, One.Y - 0.8f * By, Share * One.R});
    }
    for (const Spire &One : Of.Spires)
        if (!One.Lit) Made.Spires.push_back(Spire{One.X, One.Base, One.Wide, One.Tall, true});
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

float Top(const Pattern &Of)
{
    float Most = field::Ground;
    for (const Round &One : Of.Rounds) Most = std::min(Most, One.Y - One.R);
    for (const Box &One : Of.Boxes) Most = std::min(Most, One.Y);
    for (const Spire &One : Of.Spires) Most = std::min(Most, One.Base - One.Tall);
    return Most;
}

float Bottom(const Pattern &Of)
{
    float Least = 0;
    for (const Round &One : Of.Rounds) Least = std::max(Least, One.Y + One.R);
    for (const Box &One : Of.Boxes) Least = std::max(Least, One.Y + One.Tall);
    for (const Spire &One : Of.Spires) Least = std::max(Least, One.Base);
    return Least;
}

painter::core::Path Placed(const Pattern &Once, float Unit)
{
    painter::core::Path Made;
    Lay(Made, Once, Unit, 0, 0);
    return Made;
}

painter::core::Path Tiled(const Pattern &Once, float Unit, int Count, float Apart, float Above)
{
    painter::core::Path Made;
    for (int Turn = -1; Turn < Count; Turn++) Lay(Made, Once, Unit, static_cast<float>(Turn) * Apart, Above);
    return Made;
}

}  // namespace flappybird::skyline
