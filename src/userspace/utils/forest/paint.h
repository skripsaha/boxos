/* paint.h — what the parts of the picture share: how light and haze change a
 * colour, and each part that paints one layer of the forest. paint.cpp lays
 * the layers in order, back to front. */
#ifndef FOREST_PAINT_H
#define FOREST_PAINT_H

#include "forest.h"

namespace forest {

// A colour as the light of the hour and the weather leaves it, and faded
// toward the horizon's colour by `Distance` (0 near .. 1 at the horizon) and
// by the fog.
Color Lit(const World &Shown, Color Ink, float Distance = 0.0f);

// The leaves of autumn, in the colours the trees turn (paint_land.cpp).
Color AutumnInk(const Look &Chosen, int Which);

// The sky, the sun and the moon, the stars and the clouds, and the hills
// (paint_sky.cpp).
void PaintSky(const World &Shown, Canvas &Picture);
void PaintHills(const World &Shown, Canvas &Picture);

// The meadow, the flowers on it, and what stands on it (paint_land.cpp).
void PaintGround(const World &Shown, Canvas &Picture);
void PaintRock(const World &Shown, Canvas &Picture, const Rock &Stone);
void PaintBush(const World &Shown, Canvas &Picture, const Bush &Shrub);
void PaintLog(const World &Shown, Canvas &Picture, const Log &Trunk);
void PaintTuft(const World &Shown, Canvas &Picture, const Tuft &Grass);

// A tree, with its shadow (paint_trees.cpp).
void PaintTree(const World &Shown, Canvas &Picture, const Tree &Plant);

// An animal (paint_fauna.cpp); the ones that only drift glow, and are painted
// with the air.
void PaintAnimal(const World &Shown, Canvas &Picture, const Animal &Beast);

// Whether an animal is painted among the things on the ground, by its feet, or
// over everything.
bool OnGround(const Animal &Beast);

// Rain, snow, leaves, fog, light through the trees, lightning, and the
// glow of fireflies (paint_air.cpp).
void PaintRays(const World &Shown, Canvas &Picture);
void PaintAir(const World &Shown, Canvas &Picture);
void PaintFog(const World &Shown, Canvas &Picture);
void PaintFlash(const World &Shown, Canvas &Picture);

}  // namespace forest

#endif
