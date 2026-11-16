#ifndef TYPECASE_GLYPH_IMAGE_H
#define TYPECASE_GLYPH_IMAGE_H

/* A glyph drawn: how much of each pixel its ink covers, and where the
 * picture stands against the pen. The pen stands on the baseline; x runs
 * right and y down from it, so a picture above the baseline has a negative
 * Top. Who shows it chooses the colours: coverage is ink, not light. */

#include <cstdint>
#include <vector>

namespace typecase {

struct GlyphImage {
    std::int32_t              Left    = 0;   /* the picture's first column, from the pen */
    std::int32_t              Top     = 0;   /* its first row, from the baseline */
    std::int32_t              Width   = 0;
    std::int32_t              Height  = 0;
    float                     Advance = 0;   /* how far the pen moves past it, in pixels */
    std::vector<std::uint8_t> Coverage;      /* Width x Height, row by row: 0 none of a pixel, 255 all of it */
};

}  // namespace typecase

#endif  // TYPECASE_GLYPH_IMAGE_H
