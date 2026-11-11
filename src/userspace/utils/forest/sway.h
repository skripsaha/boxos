/* sway.h — how far the wind moves a tree at a height: the crown more than the
 * trunk, the near ones as much as the far ones, each in its own time. */
#ifndef FOREST_SWAY_H
#define FOREST_SWAY_H

#include "nature.h"
#include "numbers.h"
#include "scenery.h"

namespace forest {

// In pixels, to the right, at `Up` pixels above the foot.
inline float Sway(const Nature &Weather, const Tree &Plant, float Up)
{
    float Reach    = std::clamp(Up / Plant.Height, 0.0f, 1.0f);
    float Strength = (0.3f + 2.4f * Weather.Aloft.Wind) * std::max(0.5f, Plant.Height / 48.0f);
    float Motion   = 0.7f * Wave(Weather.Clock * 0.21f + Plant.Phase) + 0.3f * Wave(Weather.Clock * 0.57f + Plant.Phase * 3.0f);
    return Reach * Reach * Strength * Motion;
}

}  // namespace forest

#endif
