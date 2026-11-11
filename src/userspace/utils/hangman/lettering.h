/* lettering.h — big capital letters for the word's slots, in the glass's own
 * pixels: five across and seven down, a pixel being a cell across and half a
 * cell down, so a letter is 40 by 56 screen pixels on the common font and has
 * the same chunky edges as the scene behind it. */
#ifndef HANGMAN_LETTERING_H
#define HANGMAN_LETTERING_H

#include <cstdint>

#include "painter/core/color.h"
#include "painter/core/geometry.h"
#include "painter/tui/frame.h"

namespace hangman {

inline constexpr std::int32_t LetterWide = 5;
inline constexpr std::int32_t LetterHigh = 7;

// A capital letter (A to Z) with its top-left pixel at Corner, in pixels: the lowest Visible of its seven
// rows only, so that a letter can rise into view.
void PaintLetter(painter::tui::Frame &Out, char Letter, painter::core::Point Corner, std::int32_t Visible, painter::core::Color Ink);

}  // namespace hangman

#endif
