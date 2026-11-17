#ifndef PAINTER_CORE_FILL_H
#define PAINTER_CORE_FILL_H

#include <cstdint>
#include <memory>
#include <variant>

#include "painter/core/color.h"
#include "painter/core/gradient.h"
#include "painter/core/picture.h"
#include "painter/core/transform.h"

namespace painter::core {

// Which pixels a shape holds: where its contours wind round a pixel at all
// (NonZero — a contour wound the other way cuts a hole), or where they wind
// round it an odd number of times (EvenOdd).
enum class FillRule : std::uint8_t { NonZero, EvenOdd };

// What a shape is filled with: one colour, a gradient, or a picture — laid
// at Opacity of its own. A shape may take several fills, one over another,
// its coverage counted once.
class Fill {
public:
    // A picture, its pixel I, J covering I to I+1 across and J to J+1 down of
    // the path's space as Place lays it; Sharp keeps each pixel square, as
    // pixel art wants, where it is drawn larger than it is.
    struct PictureFill {
        std::shared_ptr<const core::Picture> Source;
        Transform                            Place;
        core::Extend                         Past  = core::Extend::None;
        bool                                 Sharp = false;
    };

    Fill(core::Color Solid) noexcept : Kind(Solid) {}
    Fill(core::Gradient Colors) : Kind(std::move(Colors)) {}
    static Fill Picture(std::shared_ptr<const core::Picture> Source, const Transform &Place = {},
                        core::Extend Past = core::Extend::None, bool Sharp = false);

    Fill &WithOpacity(float Of) noexcept
    {
        Opacity = Of;
        return *this;
    }

    const std::variant<core::Color, core::Gradient, PictureFill> &What() const noexcept { return Kind; }
    float                                                       Seen() const noexcept { return Opacity; }

private:
    explicit Fill(PictureFill Shown) : Kind(std::move(Shown)) {}

    std::variant<core::Color, core::Gradient, PictureFill> Kind;
    float                                                  Opacity = 1.0f;
};

}  // namespace painter::core

#endif  // PAINTER_CORE_FILL_H
