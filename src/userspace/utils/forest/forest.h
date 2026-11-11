/* forest.h — the whole forest in one place: what the person set, the weather
 * and the day, the sky they make, what stands still, what falls, and who lives
 * there; one step of time at a time, and the picture of it all.
 *
 * Only forest.cpp (the words on the line), show.cpp and bar.cpp (the clock, the
 * keys, the loop, the line at the bottom) and look.cpp (the settings) know
 * BoxOS's screen or settings; the rest takes numbers and gives pixels, so the
 * forest can be looked at anywhere.
 */
#ifndef FOREST_FOREST_H
#define FOREST_FOREST_H

#include <cstdint>

#include "air.h"
#include "canvas.h"
#include "fauna.h"
#include "look.h"
#include "nature.h"
#include "scenery.h"
#include "sky.h"

namespace forest {

struct World {
    Look     Chosen;
    Dice     Chance;
    Nature   Weather;
    SkyNow   Sky;
    Scenery  Place;
    Falling  Air;
    Fauna    Herd;

    // A forest for a glass of Width x Height pixels, at the machine's day and
    // minute, with its animals standing about.
    World(const Look &Wanted, int Width, int Height, std::uint64_t Seed, const Date &Day, float Minute);

    // `Seconds` of life: the weather, the sky it makes, what falls, who lives.
    void Step(float Seconds);

    // The settings changed: colours and drift at once, the animals as many as
    // wanted.
    void Apply(const Look &Now);

    Habitat Here() { return Habitat{Weather, Sky, Place, Air, Chance}; }
};

// The picture of the world, Picture.Width x Picture.Height (paint.cpp).
void PaintForest(const World &Shown, Canvas &Picture);

}  // namespace forest

#endif
