/* world.cpp — the grid, the brush and the weather. */

#include "world.h"

namespace sand {

namespace {

// Heat enough that whatever burns catches at once.
constexpr int LightsAtOnce = 1000;

std::uint8_t StartLife(Kind What, Dice &Luck)
{
    switch (What) {
    case Kind::Fire:  return static_cast<std::uint8_t>(14 + Luck.Below(28));
    case Kind::Smoke: return static_cast<std::uint8_t>(90 + Luck.Below(120));
    case Kind::Steam: return static_cast<std::uint8_t>(80 + Luck.Below(100));
    default:          return 0;
    }
}

}  // namespace

void World::Resize(int Wide, int High)
{
    Width  = Wide;
    Height = High;
    Cells.assign(static_cast<std::size_t>(Wide) * High, Cell{});
}

void World::Clear()
{
    Cells.assign(Cells.size(), Cell{});
}

void World::Make(Cell &Target, Kind What)
{
    Target = Cell{What, Kind::Air, static_cast<std::uint8_t>(Luck.Below(256)), StartLife(What, Luck), Phase};
}

void World::Place(int X, int Y, Kind What)
{
    if (Inside(X, Y)) Make(Edit(X, Y), What);
}

void World::Stamp(int X, int Y, int Size, Kind What, int Percent)
{
    for (int Dy = -Size; Dy <= Size; Dy++) {
        for (int Dx = -Size; Dx <= Size; Dx++) {
            if (!InBrush(Dx, Dy, Size)) continue;
            if (!Inside(X + Dx, Y + Dy)) continue;
            Cell &Target = Edit(X + Dx, Y + Dy);
            if (What == Kind::Air) Make(Target, Kind::Air);
            else if (static_cast<int>(Luck.Below(100)) >= Percent) continue;
            else if (Target.Type == Kind::Air) Make(Target, What);
            else if (What == Kind::Fire) Kindle(Target, LightsAtOnce);
        }
    }
}

void World::Weather(Kind What, int Rarity)
{
    const int Top = Fall > 0 ? 0 : Height - 1;
    for (int X = 0; X < Width; X++)
        if (Luck.OneIn(static_cast<std::uint32_t>(Rarity)) && At(X, Top).Type == Kind::Air) Make(Edit(X, Top), What);
}

}  // namespace sand
