/* scenery_trees.cpp — a tree grown from nothing: the oak and the birch fork
 * from their trunks into boughs that end in tufts of crown, so in winter what
 * is left is a bare, believable tree; the spruce is a trunk and what the paint
 * makes of its height. */

#include <cmath>

#include "scenery.h"

namespace forest {

namespace {

struct Growth {
    float Crown;        // the radius of a tuft of crown at a bough's end
    float Shorten;      // how much shorter each fork's boughs are
    float Spread;       // how far a fork's boughs part, in radians
    int   Forks;
};

void Grow(Tree &Plant, Dice &Chance, const Growth &Way, float X, float Y, float Angle, float Length, float Width,
          int Forks)
{
    float EndX = X + std::sin(Angle) * Length;
    float EndY = Y + std::cos(Angle) * Length;
    Plant.Branches.push_back({X, Y, EndX, EndY, Width});
    if (Forks == 0) {
        Plant.Crown.push_back({EndX, EndY, Way.Crown * Chance.Between(0.8f, 1.15f)});
        if (EndY > Plant.Height * 0.5f) Plant.Perches.push_back({EndX, EndY});
        return;
    }
    int Boughs = Chance.Chance(0.3f) ? 3 : 2;
    for (int Bough = 0; Bough < Boughs; Bough++) {
        float Side  = Bough % 2 == 0 ? -1.0f : 1.0f;
        float Turn  = Bough == 2 ? Chance.Between(-0.15f, 0.15f) : Side * Chance.Between(Way.Spread * 0.4f, Way.Spread);
        Grow(Plant, Chance, Way, EndX, EndY, Angle + Turn, Length * Chance.Between(Way.Shorten - 0.1f, Way.Shorten + 0.1f),
             std::max(1.0f, Width * 0.62f), Forks - 1);
    }
    if (Forks == 1) Plant.Crown.push_back({EndX, EndY, Way.Crown * 0.8f});
}

void GrowBroadleaf(Tree &Plant, Dice &Chance)
{
    bool            Oak   = Plant.Kind == TreeKind::Oak;
    const Growth    Way   = Oak ? Growth{Plant.Height * 0.15f, 0.72f, 0.95f, 3} : Growth{Plant.Height * 0.105f, 0.78f, 0.55f, 4};
    float           Trunk = Plant.Height * (Oak ? 0.34f : 0.44f);
    float           Lean  = Chance.Between(-0.12f, 0.12f);
    Plant.Branches.push_back({0.0f, 0.0f, std::sin(Lean) * Trunk, std::cos(Lean) * Trunk, Plant.Trunk});
    float Reach = Plant.Height * (Oak ? 0.27f : 0.2f);
    Grow(Plant, Chance, Way, std::sin(Lean) * Trunk, std::cos(Lean) * Trunk, Lean, Reach, std::max(1.0f, Plant.Trunk * 0.6f),
         Way.Forks);
    Plant.Perches.push_back({std::sin(Lean) * Trunk, std::cos(Lean) * Trunk});
}

}  // namespace

Tree MakeTree(TreeKind Kind, int Layer, float X, float Foot, float Height, Dice &Chance)
{
    Tree Plant;
    Plant.Kind     = Kind;
    Plant.Layer    = Layer;
    Plant.X        = X;
    Plant.Foot     = Foot;
    Plant.Height   = Height;
    Plant.Phase    = Chance.Unit();
    Plant.Variant  = Chance.Count(0, 3);
    Plant.Blossoms = Kind == TreeKind::Oak && Chance.Chance(0.55f);
    Plant.Trunk    = std::max(1.0f, Height * (Kind == TreeKind::Birch ? 0.04f : Kind == TreeKind::Oak ? 0.075f : 0.05f));
    if (Layer == 2) Plant.Trunk = std::max(Plant.Trunk, Height * 0.07f);

    if (Kind == TreeKind::Spruce) {
        Plant.Perches.push_back({0.0f, Height});
        Plant.Perches.push_back({Height * 0.1f, Height * 0.45f});
    } else {
        GrowBroadleaf(Plant, Chance);
    }
    return Plant;
}

}  // namespace forest
