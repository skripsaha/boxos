#include "tumbleweed.h"

#include <algorithm>
#include <cmath>

namespace {

using Color = TumbleColor;

}  // namespace

int TumbleWorld::GroundHeight(int X) const
{
    const float Broad = std::sin(X * 0.017f + 0.8f) * 4.5f;
    const float Dune = std::sin(X * 0.043f + 2.2f) * 2.2f;
    const float Wash = std::sin(X * 0.008f + 0.4f) * 3.0f;
    const float Cut = std::sin(X * 0.091f + 1.3f) * 0.8f;
    return Height - 10 - static_cast<int>(Broad + Dune + Wash + Cut);
}
void TumbleWorld::MakeGround()
{
    Floor.resize(static_cast<std::size_t>(Width));
    Mesa.resize(static_cast<std::size_t>(Width));
    for (int X = 0; X < Width; X++) {
        Floor[static_cast<std::size_t>(X)] = GroundHeight(X);
        const float Ridge = std::sin(X * 0.014f + 0.7f) * 6.0f + std::sin(X * 0.032f) * 2.3f;
        const float Canyon = std::sin(X * 0.0045f + 1.8f) * 7.0f + std::sin(X * 0.023f + 0.2f) * 2.0f;
        const int Top = Height * 52 / 100 + static_cast<int>(Ridge + Canyon);
        Mesa[static_cast<std::size_t>(X)] = std::clamp(Top, Height - 37, Height - 18);
    }
}

Color TumbleWorld::SceneInk()
{
    return Color::Rgb(static_cast<std::uint8_t>(Dice.Count(76, 148)),
                      static_cast<std::uint8_t>(Dice.Count(60, 116)),
                      static_cast<std::uint8_t>(Dice.Count(45, 92)));
}

void TumbleWorld::MakeScene()
{
    Stars.reserve(static_cast<std::size_t>(Width / 2));
    for (int Made = Width / 2; Made > 0; Made--)
        Stars.push_back({Dice.Count(0, Width - 1), Dice.Count(4, Height * 45 / 100), Dice.Chance(0.18f) ? 2 : 1,
                         Dice.Between(0, 6.28f)});

    for (int Made = Width / 24; Made > 0; Made--) {
        const int X = Dice.Count(5, Width - 6);
        Cacti.push_back({X, Dice.Count(14, 29), Dice.Chance(0.72f), Dice.Chance(0.2f) ? 0 : 1,
                         Dice.Chance(0.5f) ? Color::Rgb(0x4D, 0x9A, 0x4D) : Color::Rgb(0x78, 0xA6, 0x45)});
    }
    for (int Made = Width / 18; Made > 0; Made--) {
        const int X = Dice.Count(4, Width - 9);
        Rocks.push_back({X, Dice.Count(5, 13), Dice.Count(3, 8), Dice.Chance(0.3f) ? 0 : 1,
                         static_cast<std::uint32_t>(Dice.Next()), SceneInk()});
    }
    for (int Made = Width / 72; Made > 0; Made--) {
        const int Layer = Dice.Chance(0.45f) ? 0 : 1;
        Ruins.push_back({Dice.Count(12, Width - 14), Dice.Count(Layer == 0 ? 9 : 13, Layer == 0 ? 16 : 24), Layer,
                         static_cast<TumbleStructureKind>(Dice.Count(0, 2)),
                         Layer == 0 ? Color::Rgb(0x68, 0x5E, 0x60) : Color::Rgb(0x78, 0x52, 0x38)});
    }
    for (int Made = Width / 52; Made > 0; Made--) {
        const int Layer = Dice.Chance(0.35f) ? 0 : 1;
        DeadTrees.push_back({Dice.Count(5, Width - 7), Dice.Count(Layer == 0 ? 9 : 14, Layer == 0 ? 16 : 27), Layer,
                             Layer == 0 ? Color::Rgb(0x5A, 0x50, 0x4E) : Color::Rgb(0x65, 0x4C, 0x3D)});
    }
    for (int Made = Width / 64; Made > 0; Made--) {
        const int X = Dice.Count(6, Width - 28);
        Fences.push_back({X, Ground(X), Dice.Count(12, 28), Dice.Chance(0.3f) ? 0 : 1,
                          Color::Rgb(0x86, 0x61, 0x3D)});
    }
    for (int Made = Width / 17; Made > 0; Made--) {
        const int X = Dice.Count(3, Width - 4);
        Bones.push_back({X, Ground(X) - Dice.Count(1, 2), Dice.Chance(0.25f) ? 0 : 1, Dice.Chance(0.25f),
                         Color::Rgb(0xD1, 0xBA, 0x8C)});
    }
    for (int Made = Width / 42; Made > 0; Made--)
        AddBird(Dice.Between(0, static_cast<float>(Width)), Dice.Between(8.0f, Height * 0.42f));
    for (int Made = Width / 26; Made > 0; Made--)
        AddWeed(Dice.Between(0, static_cast<float>(Width)));
}
