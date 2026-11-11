/* common.h — small things both tables draw or say alike: a number of chips in
 * words, a key as the person spelled it, a framed box, the felt, the bulbs,
 * and the chips themselves. */
#ifndef CASINO_COMMON_H
#define CASINO_COMMON_H

#include <cstdint>
#include <string>
#include <string_view>

#include "canvas.h"
#include "key_chord.h"
#include "look.h"
#include "painter/core/geometry.h"
#include "painter/tui/frame.h"
#include "wheel.h"

namespace casino {

// 12500 as "12,500".
std::string Counted(std::int64_t Chips);
std::string Spelled(const KeyChord &Chord);
// A line of the help: the keys, then what they do, in columns.
std::string Entry(std::string_view Keys, std::string_view What);

// The color of a pocket or a cell of a hue.
Color HueColor(const Look &Shown, Hue Which);

// The cloth: the felt color with a grain of cells a little lighter, the same
// every time, over Area of the frame.
void Felt(painter::tui::Frame &Out, painter::core::Rect Area, Color Cloth);

// A framed box over what is under it, its title set into the top edge.
void Box(painter::tui::Frame &Out, painter::core::Rect Area, Color Edge, Color Fill, std::string_view Title,
         Color TitleInk);

// A bulb of the lights round a table or a cabinet, lit or dark.
void Bulb(Canvas &Draw, float Across, float Down, Color Lights, bool Lit);

// The chips by value: the 7 sizes are Step times these, in order.
inline constexpr int ChipSizes = 7;
std::int64_t ChipValue(const Look &Shown, int Size);
Color        ChipColor(int Size);

// Chips as stacks seen from the side, one stack for each size in Chips, from
// the largest worth at least four of it, standing on BaseDown; no stack is higher than Tallest chips.
// How many pixels across it took.
std::int32_t Stacks(Canvas &Draw, const Look &Shown, std::int64_t Chips, std::int32_t Across, std::int32_t BaseDown,
                    std::int32_t Tallest);

}  // namespace casino

#endif
