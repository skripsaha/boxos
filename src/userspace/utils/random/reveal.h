/* reveal.h — the roll: each choice turns through what it might have been, as
 * the reels of a slot machine do, slowing until it settles on what was drawn,
 * the first reel to stop first. It is painted into the console's own text, so
 * what it settled on stays where it is, and the prompt comes back under it. */
#ifndef RANDOM_REVEAL_H
#define RANDOM_REVEAL_H

#include <vector>

#include "draw.h"
#include "look.h"
#include "request.h"
#include "source.h"

namespace random_pick {

// Whether the roll was shown and the choices stay on the glass. False when it
// could not be — too many to roll at once, words the glass has no letters for,
// a glass too small, no console to paint on — and nothing was shown: the
// caller says them.
bool Reveal(const Request &Asked, const std::vector<Drawn> &Chosen, const Look &Shown, Source &Random);

}  // namespace random_pick

#endif
