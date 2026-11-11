/* sky.h — the light over the garden: the hour from the machine's clock, the
 * season from its calendar, the sky they make and the colors the plants wear
 * in it; and the sky, the hills and the grass drawn. */
#ifndef GARDEN_SKY_H
#define GARDEN_SKY_H

#include <cstdint>

#include "look.h"
#include "scene.h"

namespace garden {

struct Light {
    double Hours = 12;     // of the day, 0 to 24
    double Sun = 1;        // how high the sun stands: 1 at noon, -1 at midnight
    double Day = 1;        // 0 by night to 1 by day
    double Twilight = 0;   // 1 as the sun rises or sets, 0 far from it
    Season Year = Season::Summer;   // never Clock: the season now
    Ink    Zenith;
    Ink    Low;            // the sky at the horizon
};

// The colors of what stands in the light: the same plants, darker by night
// and tinted by the season.
struct Palette {
    Ink          Grass;
    Ink          GrassTop;
    Ink          Soil;
    Ink          Stem;
    Ink          Leaf;
    Ink          Shade;    // a leaf in the shadow of the others
    Ink          Wilted;
    Ink          Fruit;
    Ink          Bloom[3];
    Ink          Drift;    // what the air carries in this season: petals, leaves, snow
    Light        Lit;
};

Light   LightNow(const Look &Shown);
Palette PaletteIn(const Look &Shown, const Light &Lit);

// Sky, sun and moon, stars and clouds, then the hills and the grass.
void PaintSky(const Pen &Draw, const Light &Lit, double Time);
void PaintGrass(const Pen &Draw, const Palette &Colors);

}  // namespace garden

#endif
