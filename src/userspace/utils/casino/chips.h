/* chips.h — the play chips: what each size is worth and its color, a value
 * in a few characters, and chips drawn as stacks standing on the cloth. */
#ifndef CASINO_CHIPS_H
#define CASINO_CHIPS_H

#include <cstdint>
#include <string>

#include "canvas.h"
#include "look.h"
#include "painter/tui/frame.h"

namespace casino {

// The 7 sizes are Step times 1, 2, 5, 10, 20, 50 and 100, in order.
inline constexpr int ChipSizes = 7;
std::int64_t ChipValue(const Look &Shown, int Size);
Color        ChipColor(int Size);

// A number of chips in at most four characters: 950, 1.2k, 40k, 2.5M.
std::string Brief(std::int64_t Chips);

// Chips as stacks seen from the side, one stack for each size in Chips, from
// the largest worth at least four of it; each stands on the top of cell row
// FloorRow with its value written in that row, none is higher than Tallest
// pixels, and no stack stands further than Room cells from Across. How many
// cells across it took.
std::int32_t Stacks(painter::tui::Frame &Out, const Look &Shown, std::int64_t Chips, std::int32_t Across, std::int32_t FloorRow,
                    std::int32_t Tallest, std::int32_t Room);

}  // namespace casino

#endif
