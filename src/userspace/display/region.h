#ifndef BOXOS_DISPLAY_REGION_H
#define BOXOS_DISPLAY_REGION_H

// A set of the screen's pixels: where it is to be composed again. Kept as the
// rectangles it was made of, however many — one place changed or thousands —
// and given back as rectangles none of which overlaps another, covering
// those pixels exactly: nothing joined into more than was asked. Given back
// in bands: the rows where the same rectangles lie are one band, its spans
// across each a rectangle, joined to the same span of the band above it.

#include <cstdint>
#include <vector>

#include "painter/core/geometry.h"

namespace items {

class Region {
public:
    void Add(painter::core::Rect Area) noexcept;
    bool Empty() const noexcept { return Parts.empty() && !Overflowed; }
    // How many pixels it holds.
    std::uint64_t Pixels() const;
    // Its pixels as rectangles none of which overlaps another; and it is
    // emptied.
    std::vector<painter::core::Rect> Take();
    void Clear() noexcept;

private:
    // Each band of it, top to bottom: its rows [Top, Bottom) and its spans
    // across, each [From, To), in order and apart.
    template <typename Telling> void Bands(Telling &&Tell) const;

    std::vector<painter::core::Rect> Parts;
    painter::core::Rect              Bounds{};            // all of them: what is given back when no room was had
    bool                             Overflowed = false;  // a part had no room: Bounds stands for it all
};

}  // namespace items

#endif
