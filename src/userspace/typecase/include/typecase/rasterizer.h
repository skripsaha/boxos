#ifndef TYPECASE_RASTERIZER_H
#define TYPECASE_RASTERIZER_H

/* An outline to a picture: for each pixel, the share of it the ink covers,
 * found as an area, not by sampling — each edge leaves in the pixels it
 * crosses the area it closes off, and a row summed from the left is the
 * ink's winding at every pixel; a pixel's coverage is that winding, whole
 * where the contours wind round it once or more. Curves are followed by
 * straight lines that keep within a sixteenth of a pixel of them.
 *
 * Exact for contours that do not overlap. Where two do and their edges
 * cross inside one pixel, that pixel takes the sum of their shares, at
 * most whole: a little more ink than their union has, there only.
 *
 * One rasterizer holds the scratch it sums in, grown to the largest picture
 * it has drawn; it is used by one strand at a time. */

#include <cstdint>
#include <vector>

#include "typecase/glyph_image.h"
#include "typecase/outline.h"

namespace typecase {

/* A rectangle of pixels against the pen, as GlyphImage counts them: Right
 * and Bottom are the first column and row past it. */
struct Bounds {
    std::int32_t Left   = 0;
    std::int32_t Top    = 0;
    std::int32_t Right  = 0;
    std::int32_t Bottom = 0;
};

class Rasterizer {
public:
    /* Shape, in font units, at Scale pixels to a unit across and Rise up;
     * the picture no larger than its ink, and no part of it outside Limit.
     * Advance is the pen's, in pixels. */
    GlyphImage Draw(const Outline &Shape, float Scale, float Rise, Bounds Limit, float Advance);

private:
    void Edge(Point From, Point To);
    void Line(Point From, Point To);
    void Curve(Point From, Point Control, Point To);

    std::vector<float> Area;          /* (Width + 2) x Height: an edge reaches a column past the last */
    std::int32_t       Width  = 0;
    std::int32_t       Height = 0;
};

}  // namespace typecase

#endif  // TYPECASE_RASTERIZER_H
