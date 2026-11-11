/* cauldron.h — the iron pot over its fire: logs, flames that lick round it, a
 * body of riveted iron lit from below, and a liquid whose color is the brew's,
 * boiling, with rings spreading from whatever has just gone in. */
#ifndef WIZARD_CAULDRON_H
#define WIZARD_CAULDRON_H

#include "canvas.h"
#include "light.h"
#include "scene.h"

namespace wizard {

struct Spot {
    float X, Y;
};

void DrawCauldron(Canvas &Out, const Scene &Where, const Show &Now, const Lighting &Lights);

// The middle of the liquid's surface: bubbles rise from it and things fall into it.
Spot SurfaceOf(const Scene &Where);
// The top of the fire between the pot's legs: sparks rise from it.
Spot FireOf(const Scene &Where);

}  // namespace wizard

#endif
