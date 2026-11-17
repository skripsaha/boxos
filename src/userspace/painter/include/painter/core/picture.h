#ifndef PAINTER_CORE_PICTURE_H
#define PAINTER_CORE_PICTURE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "painter/core/pixel.h"

namespace painter::core {

// A picture to fill with: its pixels as a canvas keeps them (Pixel — light,
// with its opacity, in sixteen bits), and smaller copies of it, each half the
// one before, made once as it is made: a picture drawn much smaller than it
// is is seen from the copy nearest its size, mixed as light, and does not
// shimmer. Made once and shared by every fill that shows it.
class Picture {
public:
    // Bytes R, G, B, A a pixel, sRGB's numbers, the opacity not multiplied
    // in — as a PNG or JPEG reader gives them — Stride bytes from row to row.
    // None when the sizes are none, past BOX_SHAPE_REACH, or the bytes too
    // few.
    static std::shared_ptr<const Picture> FromRgba(std::span<const std::uint8_t> Bytes, std::uint32_t Width,
                                                   std::uint32_t Height, std::size_t Stride);
    // Pixels as a canvas keeps them, Stride pixels from row to row.
    static std::shared_ptr<const Picture> FromPixels(std::span<const Pixel> Pixels, std::uint32_t Width,
                                                     std::uint32_t Height, std::size_t Stride);

    std::uint32_t Width() const noexcept { return Levels.front().Width; }
    std::uint32_t Height() const noexcept { return Levels.front().Height; }

    // One size of it: the picture itself (0) and each copy half the size of
    // the one before, down to a pixel.
    struct Level {
        std::uint32_t      Width = 0, Height = 0;
        std::vector<Pixel> Pixels;
    };
    std::span<const Level> Sizes() const noexcept { return Levels; }

private:
    explicit Picture(Level Whole);
    void MakeSmaller();

    std::vector<Level> Levels;
};

}  // namespace painter::core

#endif  // PAINTER_CORE_PICTURE_H
