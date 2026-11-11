/* flora.h — one plant drawn in pixels: a sprout, a seedling, a young plant, a
 * flowering one, a grown one, a tree. The program's name gives it its shape
 * and flowers; its starts how far it has grown; how lately it was used how
 * well it stands — green and in bloom, or yellowed, bent and bare. */
#ifndef GARDEN_FLORA_H
#define GARDEN_FLORA_H

#include <cstdint>

#include "growth.h"
#include "scene.h"
#include "sky.h"

namespace garden {

struct PlantPose {
    std::uint32_t Seed = 0;           // of its name
    Stage         Reached = Stage::Sprout;
    double        Height = 0;         // as drawn, in pixels: it grows toward its starts' height
    double        Fresh = 1;
    std::int32_t  X = 0;              // the column it is rooted in
    std::int32_t  Pulse = 0;          // frames left of the sparkle for a start just heard
};

// Where the top of a plant is: for what flies around it and what marks it.
struct Crest {
    std::int32_t X = 0;
    std::int32_t Y = 0;
};

// The plant drawn, swaying in the wind of Time; where its top is.
Crest PaintPlant(const Pen &Draw, const Palette &Colors, const PlantPose &Pose, double Time);

}  // namespace garden

#endif
