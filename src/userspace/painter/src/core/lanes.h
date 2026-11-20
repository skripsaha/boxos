#ifndef PAINTER_CORE_LANES_H
#define PAINTER_CORE_LANES_H

/* lanes.h — the engine's rows of pixels: laid, written to the glass, and
 * coloured by a gradient or a picture. Each way
 * of working them (plain, SSE2, AVX2 — painter/core/cpu_path.h) gives the
 * very numbers light.h says, bit for bit; the draw stand and drawcheck hold
 * the ways to one another. */

#include <cstddef>
#include <cstdint>

#include "boxos_paint.h"
#include "painter/core/pixel.h"

namespace painter::core::lanes {

/* A gradient's row (paint.cpp): where each pixel falls in it worked as
 * boxos_paint.h works it — the walk's sums carried along the row, rounded —
 * its way brought within its ends, and the colour there between the two kept
 * every 16 of the way. */
struct GradientWalk {
    const BoxPaint *Paint;
    const Pixel    *Table;          // the colour every 16 of the way, and one past
    std::int64_t    U, V;           // the walk's sums at the row's first pixel
    std::int64_t    StepU, StepV;   // what a pixel to the right adds to them
};

/* A smooth picture's row (paint.cpp): the four pixels round each place mixed
 * by nearness, at Opacity of 65535. */
struct PictureWalk {
    const BoxPaint *Paint;          // its size, its stride, what lies past its edge
    const Pixel    *Texels;
    std::int64_t    U, V, StepU, StepV;
    std::uint32_t   Opacity;
};

struct Kit {
    /* Row[i] = Color at Share[i] of 65535 laid over Row[i]. */
    void (*LayColor)(Pixel *Row, const std::uint16_t *Share, std::uint32_t Count, Pixel Color);
    /* Row[i] = Paint[i] at Share[i] laid over Row[i]. */
    void (*LayPaint)(Pixel *Row, const Pixel *Paint, const std::uint16_t *Share, std::uint32_t Count);
    /* Row[i] = From[i] at Share laid over Row[i]. */
    void (*LayRow)(Pixel *Row, const Pixel *From, std::uint32_t Count, std::uint32_t Share);
    /* Out[i] = Row[i]'s light over black as the glass's numbers in the layout
     * Format: dithered by Thresholds[i], or the nearest when there are none. */
    void (*Show)(std::uint32_t *Out, const Pixel *Row, std::uint32_t Count, std::uint32_t Format,
                 const std::uint8_t *Thresholds);
    /* Out[i] = the gradient's colour at the row's pixel i (paint_rows.h). */
    void (*Gradient)(Pixel *Out, const GradientWalk &Walk, std::uint32_t Count);
    /* Out[i] = the smooth picture's colour at the row's pixel i (paint_rows.h). */
    void (*Picture)(Pixel *Out, const PictureWalk &Walk, std::uint32_t Count);
};

const Kit &Plain() noexcept;
#if defined(__x86_64__)
const Kit &Sse2() noexcept;
const Kit &Avx2() noexcept;
#endif

/* The kit of the way chosen (cpu_path.cpp). */
const Kit &Chosen() noexcept;

}  // namespace painter::core::lanes

#endif  // PAINTER_CORE_LANES_H
