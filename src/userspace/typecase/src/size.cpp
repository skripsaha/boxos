/* size.cpp — a font's type fitted to a cell. */

#include "typecase/size.h"

#include <cmath>

namespace typecase {

Size FitCell(const Font &Face, std::int32_t Width, std::int32_t Height)
{
    const FontMetrics &Measures = Face.Metrics();
    const std::uint16_t Advance = Face.AdvanceOf(Face.Find(U'M'));
    const float         Em      = Advance ? static_cast<float>(Width) * Measures.UnitsPerEm / Advance
                                          : static_cast<float>(Width);
    const float Scale = Em / Measures.UnitsPerEm;
    const float Line  = static_cast<float>(Measures.Ascender - Measures.Descender) * Scale;
    const float Above = static_cast<float>(Measures.Ascender) * Scale;
    return Size{Em, Width, Height, static_cast<std::int32_t>(std::lround((static_cast<float>(Height) - Line) / 2 + Above))};
}

}  // namespace typecase
