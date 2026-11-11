/* air.h — what moves over the garden besides the wind in the leaves: the
 * butterflies and bees over the plants in bloom by day and the fireflies by
 * night, and what the season lets fall: petals, leaves, snow. Every place is
 * worked out from the time alone, so nothing here is kept from one frame to
 * the next. */
#ifndef GARDEN_AIR_H
#define GARDEN_AIR_H

#include <cstdint>
#include <span>

#include "flora.h"
#include "scene.h"
#include "sky.h"

namespace garden {

// A plant they gather over: where its top is, and whether it is the first of the ranking.
struct Haunt {
    Crest         Top;
    std::uint32_t Seed = 0;
    bool          First = false;
};

void PaintVisitors(const Pen &Draw, const Palette &Colors, std::span<const Haunt> Over, double Time);
void PaintFall(const Pen &Draw, const Palette &Colors, double Time);

}  // namespace garden

#endif
