#ifndef TYPECASE_RASTERIZER_H
#define TYPECASE_RASTERIZER_H

/* An outline to a picture: for each pixel, the share of it the ink covers,
 * worked out from the outline's edges as an area — by the one coverage every
 * hand of BoxOS draws with (boxos_shape.h), so a glyph is covered as the
 * brush and Painter cover a shape. Curves are followed by straight lines that
 * keep within a sixteenth of a pixel of them; a pixel is whole where the
 * contours wind round it once or more.
 *
 * Exact for contours that do not overlap. Where two do and their edges
 * cross inside one pixel, that pixel takes the sum of their shares, at
 * most whole: a little more ink than their union has, there only.
 *
 * One rasterizer holds the room it builds and sweeps a shape in, grown to the
 * largest glyph it has drawn; it is used by one strand at a time. */

#include <cstddef>
#include <cstdint>
#include <vector>

#include "boxos_shape.h"
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
    std::vector<BoxShapeEdge> Edges = std::vector<BoxShapeEdge>(512);
    std::vector<std::int32_t> Cells;   /* a row's cells: the picture's width and two */
};

}  // namespace typecase

#endif  // TYPECASE_RASTERIZER_H
