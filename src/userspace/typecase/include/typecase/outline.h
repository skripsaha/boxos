#ifndef TYPECASE_OUTLINE_H
#define TYPECASE_OUTLINE_H

/* A glyph's shape: closed contours of straight lines and quadratic curves,
 * in the font's units, y up from the baseline, the pen at x 0. Each contour
 * begins with a Move and closes back to it; its ink is where the contours
 * wind round a point at all (the nonzero rule), so contours that overlap
 * cover once. */

#include <cstdint>
#include <vector>

namespace typecase {

struct Point {
    float X = 0;
    float Y = 0;
};

enum class Step : std::uint8_t {
    Move,   /* a new contour, at the next point */
    Line,   /* a straight line to the next point */
    Quad,   /* a curve pulled toward the next point, to the one after it */
};

struct Outline {
    std::vector<Step>  Steps;
    std::vector<Point> Points;   /* one for each Move and Line, two for each Quad */
};

}  // namespace typecase

#endif  // TYPECASE_OUTLINE_H
