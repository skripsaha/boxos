/* skyline.h — the pictures behind the pipes: the clouds, the city and its
 * windows, the trees and the bushes and the night's stars, each worked out
 * once as rounds, boxes and spires in units of the field and made a path of
 * points. All but the stars are a picture Period units wide that goes on into
 * itself, so a layer laid many times over slides along with no seam.
 *
 * What is round is lit from the top left: Lights makes, of every round, a
 * smaller one up and to the left inside it, and of every spire its left half,
 * so a picture laid in its shade's colour with its lights over it in a
 * lighter one stands out as clumps — and its lights' lights in a lighter one
 * still, as leaves catching the sun. */
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

/* A fir's tier: Wide across at its foot, on Base, rising Tall to a point over
 * X, its sides curving in; Lit, its left half only. */
struct Spire {
    float X = 0, Base = 0, Wide = 0, Tall = 0;
    bool  Lit = false;
};

struct Pattern {
    std::vector<Round> Rounds;
    std::vector<Box>   Boxes;
    std::vector<Spire> Spires;
};

/* The clouds: a far bank, high and pale; the near bank of heaped heads; and a
 * few small clouds alone in the sky above them. */
Pattern FarClouds();
Pattern Clouds();
Pattern Floating();

Pattern City();
/* The lit and unlit windows of City's houses. */
Pattern Windows(const Pattern &City);

/* The trees standing over the bushes, round ones and firs: their trunks, and
 * their crowns. */
struct Woods {
    Pattern Trunks, Crowns;
};
Woods   Trees();
Pattern Bushes();

/* The lit part of each round, Share of it across, and each spire's left
 * half. */
Pattern Lights(const Pattern &Of, float Share);

/* Stars over a sky Wide units across, down to Low. */
Pattern Stars(float Wide, float Low);

/* The pattern as a path of points, a unit Unit points: Placed once where it
 * is; Tiled Count times a Period apart, and once more before the first, so
 * what reaches over into the next period is there at the start too. */
painter::core::Path Placed(const Pattern &Once, float Unit);
painter::core::Path Tiled(const Pattern &Once, float Unit, int Count);

}  // namespace flappybird::skyline

#endif
