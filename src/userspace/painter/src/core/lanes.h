#ifndef PAINTER_CORE_LANES_H
#define PAINTER_CORE_LANES_H

/* lanes.h — the engine's rows of pixels: laid, written to the glass. Each way
 * of working them (plain, SSE2, AVX2 — painter/core/cpu_path.h) gives the
 * very numbers light.h says, bit for bit; the draw stand and drawcheck hold
 * the ways to one another. */

#include <cstdint>

#include "painter/core/pixel.h"

namespace painter::core::lanes {

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
