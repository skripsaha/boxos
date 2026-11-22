/* skyline.h — the pictures behind the pipes: the clouds, the city and its
 * windows, the bushes and the night's stars, each worked out once as rounds
 * and boxes in units of the field and made a path of points. The clouds, the
 * city and the bushes are a picture Period units wide that goes on into
 * itself, so a layer laid many times over slides along with no seam. */
#ifndef FLAPPYBIRD_SKYLINE_H
#define FLAPPYBIRD_SKYLINE_H

#include <vector>

#include "painter/core/path.h"

namespace flappybird::skyline {

inline constexpr float Period = 288;

struct Round {
    float X = 0, Y = 0, R = 0;
};

struct Box {
    float X = 0, Y = 0, Wide = 0, Tall = 0;
};

struct Pattern {
    std::vector<Round> Rounds;
    std::vector<Box>   Boxes;
};

Pattern Clouds();
Pattern City();
/* The lit and unlit windows of City's houses. */
Pattern Windows(const Pattern &City);
Pattern Bushes();
/* Stars over a sky Wide units across, down to Low. */
Pattern Stars(float Wide, float Low);

/* The pattern as a path of points, a unit Unit points, laid Count times a
 * Period apart. */
painter::core::Path Tiled(const Pattern &Once, float Unit, int Count);

}  // namespace flappybird::skyline

#endif
