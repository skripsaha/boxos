#ifndef PAINTER_CORE_BLUR_H
#define PAINTER_CORE_BLUR_H

/* blur.h — a Gaussian blur as three passes of a box, each way: the boxes
 * chosen so their spread is the Gaussian's (Kovesi's widths), every pass a
 * running sum, so the cost is the same at any radius. Integers only, a mean
 * rounded to the nearest; the passes grow the plane by how far they reach, so
 * nothing at its edge is lost. */

#include <cstdint>

namespace painter::core::engine {

struct BlurBoxes {
    std::int32_t Radius[3] = {0, 0, 0};   /* each box (2·Radius + 1) wide */
    std::int32_t Reach     = 0;           /* their radii together: how far the blur spreads */
};

/* The boxes for a Gaussian of Sigma pixels; none for a sigma too small to
 * blur. */
BlurBoxes BoxesFor(float Sigma) noexcept;

/* Plane — Width x Height values, the blurred picture in its middle, Reach of
 * nothing round it — blurred where it lies, each value the boxes' rounded
 * mean of those round it, past the plane's edge nothing. */
void BlurPlane(std::uint32_t *Plane, std::int32_t Width, std::int32_t Height, const BlurBoxes &Boxes);

/* The same for each of a pixel's four channels in one plane of
 * Width x Height pixels, four values each, the plane's edge held — what lies
 * past it as its edge pixel — so a blurred backdrop keeps its edge colour. */
void BlurPixels(std::uint32_t *Plane, std::int32_t Width, std::int32_t Height, const BlurBoxes &Boxes);

}  // namespace painter::core::engine

#endif  // PAINTER_CORE_BLUR_H
