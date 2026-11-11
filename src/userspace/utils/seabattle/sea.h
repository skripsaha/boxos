/* sea.h — the water: shaded from the surface colour into the deep by depth,
 * crossed by swells that drift, and sparkling where a crest catches the light. */
#ifndef SEABATTLE_SEA_H
#define SEABATTLE_SEA_H

#include <cstdint>

#include "canvas.h"

namespace seabattle {

struct SeaPatch {
    int           X, Y, Width, Height;     // the pixels the sea fills
    Color         Surface;
    float         Depth = 0.55f;           // how dark it is at the bottom, 0..1
    float         Swell = 1.0f;            // how strong the waves are; 0 is flat water
    bool          Flat = false;            // a text page: bands of two colours, no waves
};

void PaintSea(Canvas &Out, const SeaPatch &Sea, std::uint64_t Ms);
// The whole glass as deep, quiet water: what every page is laid on.
void PaintBackdrop(Canvas &Out, Color Sea, bool Flat, std::uint64_t Ms);

}  // namespace seabattle

#endif
