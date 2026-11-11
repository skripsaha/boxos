/* scenery.cpp — the forest laid out for the size of the glass: the same
 * hills and meadow however big it is, with as many trees, flowers and stars
 * as its area holds. */

#include "scenery.h"

#include <cmath>

namespace forest {

namespace {

TreeKind KindOfTree(Dice &Chance)
{
    float Roll = Chance.Unit();
    return Roll < 0.42f ? TreeKind::Oak : Roll < 0.68f ? TreeKind::Birch : TreeKind::Spruce;
}

void LayGround(Scenery &Place, Dice &Chance)
{
    const Layout &Size = Place.Size;
    float         A = Chance.Unit(), B = Chance.Unit(), C = Chance.Unit(), D = Chance.Unit();
    auto          Width = static_cast<std::size_t>(Size.Width);
    Place.Contour.resize(Width);
    Place.Far.resize(Width);
    Place.Near.resize(Width);
    for (int X = 0; X < Size.Width; X++) {
        auto  At = static_cast<std::size_t>(X);
        float Across = static_cast<float>(X);
        Place.Contour[At] = Size.Ground + static_cast<int>(std::lround(1.4f * Wave(Across / 57.0f + A) + 0.8f * Wave(Across / 21.0f + B)));
        float H = static_cast<float>(Size.Height);
        Place.Far[At]  = static_cast<int>(H * (0.20f + 0.06f * Wave(Across / 140.0f + C) + 0.025f * Wave(Across / 47.0f + D)));
        Place.Near[At] = static_cast<int>(H * (0.075f + 0.03f * Wave(Across / 83.0f + D) + 0.012f * Wave(Across / 29.0f + A)));
    }
}

void PlantFarTrees(Scenery &Place, Dice &Chance)
{
    const Layout &Size = Place.Size;
    float         Scale = static_cast<float>(Size.Height) / 96.0f;
    for (int Row = 0; Row < 2; Row++) {
        const std::vector<int> &Ridge = Row == 0 ? Place.Far : Place.Near;
        int Count = std::max(4, Size.Width / (Row == 0 ? 6 : 8));
        for (int Each = 0; Each < Count; Each++) {
            int   X      = Chance.Count(0, Size.Width - 1);
            float Height = Chance.Between(9.0f, 17.0f) * std::max(0.7f, Scale) * (Row == 0 ? 0.85f : 1.0f);
            float Foot   = static_cast<float>(Size.Ground - Ridge[static_cast<std::size_t>(X)] + 1);
            TreeKind Kind = Chance.Chance(0.65f) ? TreeKind::Spruce : TreeKind::Oak;
            Place.Trees.push_back(MakeTree(Kind, 0, static_cast<float>(X), Foot, Height, Chance));
        }
    }
}

void PlantTrees(Scenery &Place, Dice &Chance)
{
    const Layout &Size = Place.Size;
    int           Slots = std::max(3, Size.Width / 20);
    float         Slot  = static_cast<float>(Size.Width) / static_cast<float>(Slots);
    for (int At = 0; At < Slots; At++) {
        float X      = (static_cast<float>(At) + 0.5f + Chance.Between(-0.35f, 0.35f)) * Slot;
        float Foot   = static_cast<float>(Size.Ground) + Chance.Between(1.0f, static_cast<float>(Size.Front - Size.Ground) * 0.14f + 1.0f);
        float Height = static_cast<float>(Size.Height) * Chance.Between(0.44f, 0.6f);
        Place.Trees.push_back(MakeTree(KindOfTree(Chance), 1, X, Foot, Height, Chance));
    }
    for (int Side = 0; Side < 2; Side++) {
        float X      = static_cast<float>(Size.Width) * (Side == 0 ? Chance.Between(0.04f, 0.1f) : Chance.Between(0.9f, 0.96f));
        float Height = static_cast<float>(Size.Height) * Chance.Between(1.05f, 1.25f);
        TreeKind Kind = Side == 0 ? TreeKind::Oak : (Chance.Chance(0.5f) ? TreeKind::Birch : TreeKind::Spruce);
        Place.Trees.push_back(MakeTree(Kind, 2, X, static_cast<float>(Size.Front + 2), Height, Chance));
    }
}

float LaneFoot(const Layout &Size, Dice &Chance)
{
    return static_cast<float>(Size.Ground + 3) + Chance.Unit() * static_cast<float>(Size.Front - Size.Ground - 4);
}

void PlantGround(Scenery &Place, Dice &Chance)
{
    const Layout &Size  = Place.Size;
    float         Scale = std::max(0.6f, static_cast<float>(Size.Height) / 96.0f);
    auto          Wide  = static_cast<float>(Size.Width);

    for (int At = 0; At < std::max(2, Size.Width / 30); At++)
        Place.Bushes.push_back({Chance.Between(0.0f, Wide), LaneFoot(Size, Chance),
                                static_cast<int>(Chance.Between(9.0f, 15.0f) * Scale), static_cast<int>(Chance.Between(5.0f, 8.0f) * Scale),
                                static_cast<std::uint32_t>(Chance.Next())});
    for (int At = 0; At < std::max(2, Size.Width / 40); At++)
        Place.Rocks.push_back({Chance.Between(0.0f, Wide), LaneFoot(Size, Chance), static_cast<int>(Chance.Between(6.0f, 12.0f) * Scale),
                               static_cast<int>(Chance.Between(3.0f, 6.0f) * Scale), static_cast<std::uint32_t>(Chance.Next())});
    Place.Logs.push_back({Chance.Between(Wide * 0.3f, Wide * 0.7f), LaneFoot(Size, Chance),
                          static_cast<int>(Chance.Between(15.0f, 22.0f) * Scale)});

    for (int At = 0; At < Size.Width * 2 / 5; At++) {
        float Depth = std::sqrt(Chance.Unit());
        float Foot  = static_cast<float>(Size.Ground + 2) + Depth * static_cast<float>(Size.Front - Size.Ground - 1);
        Place.Tufts.push_back({Chance.Between(0.0f, Wide), Foot, static_cast<int>(2.0f + 3.0f * Depth * Scale), Chance.Count(0, 2)});
    }
    for (int At = 0; At < Size.Width * 3 / 2; At++)
        Place.Flowers.push_back({Chance.Count(0, Size.Width - 1), Chance.Count(Size.Ground + 2, Size.Front), Chance.Count(0, 4), Chance.Unit()});
}

Cloud MakeCloud(const Layout &Size, int Index, int Count, Dice &Chance)
{
    Cloud Puffed;
    float Scale  = Chance.Between(0.75f, 1.35f);
    float Radius = static_cast<float>(Size.Height) * 0.055f * Scale;
    Puffed.X         = Chance.Between(-0.1f, 1.1f) * static_cast<float>(Size.Width);
    Puffed.Y         = Chance.Between(0.05f, 0.3f) * static_cast<float>(Size.Ground);
    Puffed.Scale     = Scale;
    Puffed.Speed     = Chance.Between(1.5f, 4.5f);
    Puffed.Threshold = (static_cast<float>(Index) + 0.5f) / static_cast<float>(Count);
    int Puffs        = Chance.Count(4, 6);
    for (int At = 0; At < Puffs; At++) {
        float Along  = static_cast<float>(At) - static_cast<float>(Puffs - 1) / 2.0f;
        float Middle = 1.0f - std::fabs(Along) / static_cast<float>(Puffs);
        Puffed.Puffs.push_back({Along * Radius * 1.15f, -Middle * Radius * 0.6f + Chance.Between(-0.2f, 0.2f) * Radius,
                                Radius * (0.55f + 0.6f * Middle) * Chance.Between(0.85f, 1.1f)});
    }
    return Puffed;
}

}  // namespace

Scenery MakeScenery(int Width, int Height, Dice &Chance)
{
    Scenery Place;
    Place.Size = {Width, Height, Height * 58 / 100, Height - 3};
    LayGround(Place, Chance);
    PlantFarTrees(Place, Chance);
    PlantTrees(Place, Chance);
    PlantGround(Place, Chance);

    int Stars = Width * Place.Size.Ground / 90;
    for (int At = 0; At < Stars; At++)
        Place.Stars.push_back({Chance.Count(0, Width - 1), Chance.Count(0, Place.Size.Ground - 6), Chance.Unit(), Chance.Between(0.4f, 1.0f)});
    const int Clouds = 8;
    for (int At = 0; At < Clouds; At++) Place.Clouds.push_back(MakeCloud(Place.Size, At, Clouds, Chance));
    return Place;
}

}  // namespace forest
