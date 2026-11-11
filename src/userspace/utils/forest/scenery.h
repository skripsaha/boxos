/* scenery.h — what stands still in the forest, laid once for the size of the
 * glass: the hills, the ground, the trees of three depths, the bushes, rocks
 * and flowers, the stars and the clouds. Positions are in pixels, Y downward;
 * a tree's own parts are measured from its foot, upward. */
#ifndef FOREST_SCENERY_H
#define FOREST_SCENERY_H

#include <cstdint>
#include <vector>

#include "numbers.h"

namespace forest {

enum class TreeKind : std::uint8_t { Oak, Birch, Spruce };

struct Branch {
    float X0, Y0, X1, Y1;
    float Width;
};

struct Blob {
    float X, Y, Radius;
};

// A point: where a bird can sit, from a tree's foot; a bend of a bolt.
struct Spot {
    float X, Y;
};

// Layer 0 is far on the hills, 1 in the middle of the forest, 2 near the eye.
struct Tree {
    TreeKind            Kind;
    int                 Layer;
    float               X, Foot;
    float               Height;
    float               Trunk;       // its width at the foot
    float               Phase;       // its own place in the wind
    int                 Variant;     // which of its kind's colours
    bool                Blossoms;
    std::vector<Branch> Branches;
    std::vector<Blob>   Crown;
    std::vector<Spot>   Perches;
};

struct Rock {
    float X, Foot;
    int   Width, Height;
    std::uint32_t Seed;
};

struct Bush {
    float X, Foot;
    int   Width, Height;
    std::uint32_t Seed;
};

struct Log {
    float X, Foot;
    int   Length;
};

struct Tuft {
    float X, Foot;
    int   Height;
    int   Variant;
};

struct Flower {
    int   X, Y;
    int   Variant;
    float Threshold;     // blooms when the land's flowers pass it
};

struct Star {
    int   X, Y;
    float Phase;
    float Bright;
};

struct Cloud {
    float X, Y;
    float Scale;
    float Speed;         // pixels a second
    float Threshold;     // there when the sky's cloud passes it
    std::vector<Blob> Puffs;
};

struct Layout {
    int Width  = 0;
    int Height = 0;
    int Ground = 0;      // the row the meadow begins at
    int Front  = 0;      // the last row an animal's feet reach
};

struct Scenery {
    Layout              Size;
    std::vector<int>    Far;         // the far hills' height above the ground row, per column
    std::vector<int>    Near;
    std::vector<int>    Contour;     // the ground's top row, per column
    std::vector<Tree>   Trees;
    std::vector<Rock>   Rocks;
    std::vector<Bush>   Bushes;
    std::vector<Log>    Logs;
    std::vector<Tuft>   Tufts;
    std::vector<Flower> Flowers;
    std::vector<Star>   Stars;
    std::vector<Cloud>  Clouds;
};

Scenery MakeScenery(int Width, int Height, Dice &Chance);

// A tree of the kind and depth, standing at X with its foot on row Foot
// (scenery_trees.cpp).
Tree MakeTree(TreeKind Kind, int Layer, float X, float Foot, float Height, Dice &Chance);

// The row of the ground's top at column X.
inline int GroundAt(const Scenery &Place, int X)
{
    return Place.Contour[static_cast<std::size_t>(std::clamp(X, 0, Place.Size.Width - 1))];
}

}  // namespace forest

#endif
