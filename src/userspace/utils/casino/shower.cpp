/* shower.cpp — falling coins (shower.h). */

#include "shower.h"

#include <algorithm>
#include <cmath>

#include "trig.h"

namespace casino {

namespace {

constexpr float Gravity = 70.0f;

}  // namespace

void Shower::Begin(int Count, float Seconds)
{
    Left += static_cast<float>(Count);
    Rate = Left / std::max(Seconds, 0.1f);
}

void Shower::Step(float Seconds, std::int32_t Wide, std::int32_t High, Luck &Chance)
{
    Owed += Rate * Seconds;
    while (Owed >= 1.0f && Left >= 1.0f) {
        Coins.push_back(Coin{Chance.Between(0.0f, static_cast<float>(Wide)), -3.0f, Chance.Between(-6.0f, 6.0f),
                             Chance.Between(5.0f, 20.0f), Chance.Between(0.0f, TwoPi),
                             Chance.Between(1.8f, 3.2f) * std::max(1.0f, static_cast<float>(High) / 60.0f)});
        Left -= 1.0f;
        Owed -= 1.0f;
    }
    if (Left < 1.0f) {
        Left = 0.0f;
        Owed = 0.0f;
    }
    for (Coin &One : Coins) {
        One.Fall += Gravity * Seconds;
        One.Down += One.Fall * Seconds;
        One.Across += One.Drift * Seconds;
        One.Turn += 9.0f * Seconds;
    }
    std::erase_if(Coins, [&](const Coin &One) { return One.Down > static_cast<float>(High) + 4.0f; });
}

void Shower::Paint(Canvas &Draw, Color Gold) const
{
    for (const Coin &One : Coins) {
        const float Face = std::fabs(Cosine(One.Turn));
        Draw.Ellipse(One.Across, One.Down, std::max(0.6f, One.Size * Face), One.Size, Shade(Gold, 0.55f + 0.2f * Face));
        if (Face > 0.4f) Draw.Ellipse(One.Across, One.Down, std::max(0.4f, One.Size * Face * 0.65f), One.Size * 0.65f, Shade(Gold, 1.25f));
    }
}

}  // namespace casino
