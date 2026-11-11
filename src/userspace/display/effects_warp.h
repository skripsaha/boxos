#ifndef BOXOS_DISPLAY_EFFECTS_WARP_H
#define BOXOS_DISPLAY_EFFECTS_WARP_H

#include <cstdint>
#include <vector>

#include "effects_math.h"

// The effects that move cells: where the cell shown at a place is taken from.
// Worked out once for a frame (WarpPlan), then asked cell by cell (WarpSource).

namespace effects {

struct Warp {
    std::vector<std::int32_t> ColumnBase;   // the column each column is taken from before anything moves it: mirror, pulse, mosaic
    std::vector<std::int32_t> RowBase;
    std::vector<std::int32_t> RowShift;     // cells each row is moved sideways: sway, glitch
    std::vector<std::int32_t> ColumnShift;  // cells each column is moved up or down: sway
    std::vector<std::int32_t> Drip;         // cells each column has slid down: melt
    std::vector<std::int32_t> Split;        // how far a row's colours come apart, in cells: glitch
    std::int32_t ShakeX = 0;                // the whole picture's, in cells: quake
    std::int32_t ShakeY = 0;
    bool         Rippling = false;
    std::vector<std::int32_t> RingHeight;   // by the distance from the middle in sixteenths of a pixel: how far the rings move a cell
    std::vector<std::int32_t> RingAim;      // and 2^22 over that distance, to find the direction by
};

struct Source {
    std::int32_t X;
    std::int32_t Y;
};

void WarpPlan(Warp &Out, const Moment &Now);

Source WarpSource(const Warp &Plan, const Moment &Now, std::int32_t X, std::int32_t Y) noexcept;

}  // namespace effects

#endif
