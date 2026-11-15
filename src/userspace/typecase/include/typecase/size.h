#ifndef TYPECASE_SIZE_H
#define TYPECASE_SIZE_H

/* How large type is set, and the cell each character stands in. A font
 * draws to its em; a source that draws to the cell itself — a bitmap font,
 * the code of a missing character — fills the cell; and a picture is
 * limited to the cell and an em round it, so no glyph, however damaged its
 * font, asks for more pixels than its size gives. Running text that knows
 * no cells takes its line for the cell's height and its advance for the
 * width. */

#include <cstdint>

#include "typecase/font.h"

namespace typecase {

struct Size {
    float        Em       = 0;   /* pixels to the em */
    std::int32_t Width    = 0;   /* the cell, in pixels */
    std::int32_t Height   = 0;
    std::int32_t Baseline = 0;   /* rows from the cell's top down to the baseline */

    bool operator==(const Size &) const = default;
};

/* Face's type as it fills a cell of Width by Height: its em such that M's
 * advance is the width, its baseline where the font's line, ascender to
 * descender, stands centred in the height. */
Size FitCell(const Font &Face, std::int32_t Width, std::int32_t Height);

}  // namespace typecase

#endif  // TYPECASE_SIZE_H
