#include "tumbleweed.h"

#include <algorithm>
#include <cmath>

namespace {

using Color = TumbleColor;

}

int TumbleWorld::GroundHeight(int X) const
{
    const float Broad = std::sin(X * 0.019f + 1.1f) * 5.0f;
    const float Shoulder = std::sin(X * 0.047f + 2.7f) * 2.2f;
    const float Wash = std::sin(X * 0.009f + 0.4f) * 3.5f;
    return Height - 9 - static_cast<int>(Broad + Shoulder + Wash);
}

void TumbleWorld::MakeGround()
{
    Floor.resize(static_cast<std::size_t>(Width));
    Mesa.resize(static_cast<std::size_t>(Width));
    for (int X = 0; X < Width; X++) {
        Floor[static_cast<std::size_t>(X)] = GroundHeight(X);
        const float Ridge = std::sin(X * 0.016f + 0.7f) * 4.5f + std::sin(X * 0.039f) * 2.0f;
        const float Cut = std::sin(X * 0.006f + 2.1f) * 5.0f + std::sin(X * 0.028f) * 2.0f;
        const int Top = Height * 58 / 100 + static_cast<int>(Ridge + Cut);
        Mesa[static_cast<std::size_t>(X)] = std::clamp(Top, Height - 30, Height - 13);
    }
}

Color TumbleWorld::SceneInk()
{
    return Color::Rgb(static_cast<std::uint8_t>(Dice.Count(80, 150)),
                      static_cast<std::uint8_t>(Dice.Count(65, 120)),
                      static_cast<std::uint8_t>(Dice.Count(45, 88)));
}

void TumbleWorld::MakeScene()
{
    for (int Made = Width / 18; Made > 0; Made--) {
        const int X = Dice.Count(4, Width - 5);
        const int Layer = Dice.Chance(0.28f) ? 0 : 1;
        Cacti.push_back({X, Dice.Count(Layer == 0 ? 7 : 10, Layer == 0 ? 13 : 22), Dice.Chance(0.72f), Layer,
                         Dice.Chance(0.5f) ? Color::Rgb(0x49, 0x9B, 0x4B) : Color::Rgb(0x77, 0xA6, 0x45)});
    }
    for (int Made = Width / 20; Made > 0; Made--) {
        const int Layer = Dice.Chance(0.35f) ? 0 : 1;
        Rocks.push_back({Dice.Count(3, Width - 8), Dice.Count(4, 12), Dice.Count(2, 7), Layer,
                         Layer == 0 ? Color::Rgb(0x71, 0x68, 0x6B) : SceneInk()});
    }
    for (int Made = Width / 72; Made > 0; Made--) {
        const int Layer = Dice.Chance(0.5f) ? 0 : 1;
        Ruins.push_back({Dice.Count(8, Width - 10), Dice.Count(Layer == 0 ? 6 : 10, Layer == 0 ? 12 : 20), Layer,
                         Layer == 0 ? Color::Rgb(0x6B, 0x5E, 0x63) : Color::Rgb(0x78, 0x5A, 0x48)});
    }
    for (int Made = Width / 45; Made > 0; Made--)
        AddBird(Dice.Between(0, static_cast<float>(Width)), Dice.Between(8, 30));
    for (int Made = Width / 55; Made > 0; Made--)
        AddWeed(Dice.Between(0, static_cast<float>(Width)));
}

void TumbleWorld::AddWeed(float X)
{
    if (Weeds.size() >= 90) return;
    const int Layer = Dice.Chance(0.25f) ? 0 : 1;
    const float Direction = Dice.Chance(0.5f) ? 1.0f : -1.0f;
    Weeds.push_back({std::clamp(X, 2.0f, static_cast<float>(Width - 3)), Direction * Dice.Between(2.5f, 8.0f),
                     Dice.Between(0, 6.28f), Layer, Color::Rgb(0x9B, 0x72, 0x40)});
    Spawned++;
}

void TumbleWorld::AddCactus(float X)
{
    if (Cacti.size() >= 100) return;
    Cacti.push_back({std::clamp(static_cast<int>(X), 4, Width - 5), Dice.Count(10, 22), Dice.Chance(0.72f), 1,
                     Color::Rgb(0x5F, 0xA1, 0x4A)});
    Spawned++;
}

void TumbleWorld::AddBird(float X, float Y)
{
    if (Birds.size() >= 80) return;
    Birds.push_back({X, Y, Dice.Between(4.0f, 10.0f) * (Dice.Chance(0.5f) ? 1.0f : -1.0f), Dice.Between(0, 6.28f),
                     Dice.Chance(0.45f) ? 0 : 1, Color::Rgb(0xD8, 0xD0, 0xB8)});
}
