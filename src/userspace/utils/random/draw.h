/* draw.h — the drawing itself: what a Request asks for, taken from its pool. */
#ifndef RANDOM_DRAW_H
#define RANDOM_DRAW_H

#include <string>
#include <vector>

#include "box/types.h"

#include "request.h"
#include "source.h"

namespace random_pick {

struct Drawn {
    std::string Text;
    file_id_t   File = 0;    // the file, when it was files that were chosen among
};

// As many as the Request says, in the order they came.
std::vector<Drawn> Draw(const Request &Asked, Source &Random);

// Any one of the pool, to show while the choice is not yet made.
std::string Candidate(const Request &Asked, Source &Random);

}  // namespace random_pick

#endif
