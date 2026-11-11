/* digits.h — numbers drawn in pixels, each digit three wide and five high
 * times its scale: the faces of the big dice and the total. */
#ifndef DICE_DIGITS_H
#define DICE_DIGITS_H

#include <cstddef>
#include <string_view>

#include "canvas.h"

namespace dice {

// The pixels Length characters take across, and a digit takes down, at Scale.
int DigitsWide(std::size_t Length, int Scale);
int DigitsHigh(int Scale);

// Text — digits and a minus — with its corner at At.
void DrawDigits(const Canvas &On, painter::core::Point At, std::string_view Text, int Scale, Color Ink);

}  // namespace dice

#endif
