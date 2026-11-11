/* word.h — a word in big letters made of pixels, for the face the coin lay on. */
#ifndef COINFLIP_WORD_H
#define COINFLIP_WORD_H

#include <cstdint>
#include <string_view>

#include "painter/core/color.h"
#include "painter/tui/frame.h"

namespace coinflip {

// Pixels a letter is high, and how many pixels a word of Letters takes across,
// each dot of a letter being Dot pixels square.
inline constexpr std::int32_t LetterRows = 5;
std::int32_t                  WordWidth(std::string_view Word, std::int32_t Dot);

// The word with its top left at the pixel (X, Y): the letters H E A D S T I L,
// any other character a gap.
void PaintWord(painter::tui::Frame &Out, std::int32_t X, std::int32_t Y, std::string_view Word, std::int32_t Dot,
               painter::core::Color Ink);

}  // namespace coinflip

#endif
