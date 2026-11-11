/* paint.cpp — the forest, back to front: the sky and the hills, the far trees,
 * the meadow; then everything that stands on it by its feet, so a fox walks
 * behind the bush it is behind and in front of the one it has passed; then the
 * birds in the air; last the weather. */

#include <algorithm>

#include "paint.h"

namespace forest {

namespace {

enum class Thing : std::uint8_t { Tree, Rock, Bush, Log, Tuft, Animal };

struct Item {
    float       Foot;
    Thing       What;
    std::size_t At;
};

std::vector<Item> StandingThings(const World &Shown)
{
    std::vector<Item> Items;
    const Scenery &Place = Shown.Place;
    for (std::size_t At = 0; At < Place.Trees.size(); At++)
        if (Place.Trees[At].Layer > 0) Items.push_back({Place.Trees[At].Foot, Thing::Tree, At});
    for (std::size_t At = 0; At < Place.Rocks.size(); At++) Items.push_back({Place.Rocks[At].Foot, Thing::Rock, At});
    for (std::size_t At = 0; At < Place.Bushes.size(); At++) Items.push_back({Place.Bushes[At].Foot, Thing::Bush, At});
    for (std::size_t At = 0; At < Place.Logs.size(); At++) Items.push_back({Place.Logs[At].Foot, Thing::Log, At});
    for (std::size_t At = 0; At < Place.Tufts.size(); At++) Items.push_back({Place.Tufts[At].Foot, Thing::Tuft, At});
    for (std::size_t At = 0; At < Shown.Herd.Animals.size(); At++)
        if (OnGround(Shown.Herd.Animals[At])) Items.push_back({Shown.Herd.Animals[At].Y, Thing::Animal, At});
    std::stable_sort(Items.begin(), Items.end(), [](const Item &A, const Item &B) { return A.Foot < B.Foot; });
    return Items;
}

void Paint(const World &Shown, Canvas &Picture, const Item &Each)
{
    const Scenery &Place = Shown.Place;
    switch (Each.What) {
    case Thing::Tree:   PaintTree(Shown, Picture, Place.Trees[Each.At]); break;
    case Thing::Rock:   PaintRock(Shown, Picture, Place.Rocks[Each.At]); break;
    case Thing::Bush:   PaintBush(Shown, Picture, Place.Bushes[Each.At]); break;
    case Thing::Log:    PaintLog(Shown, Picture, Place.Logs[Each.At]); break;
    case Thing::Tuft:   PaintTuft(Shown, Picture, Place.Tufts[Each.At]); break;
    case Thing::Animal: PaintAnimal(Shown, Picture, Shown.Herd.Animals[Each.At]); break;
    }
}

}  // namespace

Color Lit(const World &Shown, Color Ink, float Distance)
{
    float Fog    = Shown.Weather.Aloft.Fog * (0.3f + 0.6f * Distance);
    float Weight = std::min(0.94f, Distance + (1.0f - Distance) * Fog);
    return Mixed(Tinted(Ink, Shown.Sky.Light), Shown.Sky.Horizon, static_cast<int>(Weight * 256.0f));
}

void PaintForest(const World &Shown, Canvas &Picture)
{
    PaintSky(Shown, Picture);
    PaintHills(Shown, Picture);
    for (const Tree &Plant : Shown.Place.Trees)
        if (Plant.Layer == 0) PaintTree(Shown, Picture, Plant);
    PaintGround(Shown, Picture);
    for (const Item &Each : StandingThings(Shown)) Paint(Shown, Picture, Each);
    for (const Animal &Beast : Shown.Herd.Animals)
        if (!OnGround(Beast)) PaintAnimal(Shown, Picture, Beast);
    PaintRays(Shown, Picture);
    PaintAir(Shown, Picture);
    PaintFog(Shown, Picture);
    PaintFlash(Shown, Picture);
}

}  // namespace forest
