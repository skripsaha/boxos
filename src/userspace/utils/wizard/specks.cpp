/* specks.cpp — how specks move and are drawn. */

#include "specks.h"

#include <algorithm>

namespace wizard {

void Specks::Add(const Speck &One)
{
    constexpr std::size_t Most = 220;
    if (All.size() < Most) All.push_back(One);
}

void Specks::Step(std::uint32_t ElapsedMs)
{
    const float Seconds = static_cast<float>(ElapsedMs) / 1000.0f;
    for (Speck &One : All) {
        One.AgeMs += ElapsedMs;
        One.Vy += One.Gravity * Seconds;
        One.X += One.Vx * Seconds;
        One.Y += One.Vy * Seconds;
    }
    std::erase_if(All, [](const Speck &One) { return One.AgeMs >= One.LifeMs; });
}

void Specks::Draw(Canvas &Out) const
{
    for (const Speck &One : All) {
        const float Left = 1.0f - static_cast<float>(One.AgeMs) / static_cast<float>(One.LifeMs);
        const int X = static_cast<int>(One.X), Y = static_cast<int>(One.Y);
        switch (One.Kind) {
        case SpeckKind::Spark:
            Out.Cover(X, Y, Mixed(One.Ink, Color::Rgb(0xFF, 0xFF, 0xE0), 0.35f * Left), std::min(1.0f, Left * 2.2f));
            Out.Cover(X, Y - 1, One.Ink, Left * 0.35f);
            break;
        case SpeckKind::Steam: {
            const float Spread = 0.5f + (1.0f - Left) * 1.6f;
            Out.Cover(X, Y, One.Ink, Left * 0.30f);
            Out.Cover(X + 1, Y, One.Ink, Left * 0.18f * Spread);
            Out.Cover(X - 1, Y, One.Ink, Left * 0.18f * Spread);
            Out.Cover(X, Y - 1, One.Ink, Left * 0.14f * Spread);
            break;
        }
        case SpeckKind::Drop:
            Out.Cover(X, Y, One.Ink, std::min(1.0f, Left * 3.0f));
            Out.Cover(X, Y - 1, Lighter(One.Ink, 80), Left * 0.5f);
            break;
        }
    }
}

}  // namespace wizard
