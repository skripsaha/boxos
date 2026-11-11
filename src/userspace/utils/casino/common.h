/* common.h — small things the tables say or draw alike: a number of chips in
 * words, a key as the person spelled it, the color of a pocket, the bulbs. */
#ifndef CASINO_COMMON_H
#define CASINO_COMMON_H

#include <cstdint>
#include <string>
#include <string_view>

#include "canvas.h"
#include "key_chord.h"
#include "look.h"
#include "wheel.h"

namespace casino {

// 12500 as "12,500".
std::string Counted(std::int64_t Chips);
std::string Spelled(const KeyChord &Chord);
// A line of the help: the keys, then what they do, in columns.
std::string Entry(std::string_view Keys, std::string_view What);

// The color of a pocket or a cell of a hue.
Color HueColor(const Look &Shown, Hue Which);

// A bulb of the lights round a table or a cabinet, lit or dark.
void Bulb(Canvas &Draw, float Across, float Down, Color Lights, bool Lit);

}  // namespace casino

#endif
