/* look.h — what the person set (settings random), read at every start. */
#ifndef RANDOM_LOOK_H
#define RANDOM_LOOK_H

#include <cstdint>

#include "painter/core/color.h"

namespace random_pick {

struct Look {
    bool                Animated;      // a choice rolls before it settles
    std::uint32_t       RollMs;        // how long the roll lasts
    std::int64_t        Least;         // the range when none is said
    std::int64_t        Most;
    painter::core::Color Rolling;      // what turns
    painter::core::Color Chosen;       // what it settled on
    painter::core::Color Frame;        // the brackets round it
};

Look ReadLook();

}  // namespace random_pick

#endif
