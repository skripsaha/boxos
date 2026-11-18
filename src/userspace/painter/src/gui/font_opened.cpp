/*
 * font_opened.cpp — who hears of a font an area opens. See font_opened.h.
 */

#include "font_opened.h"

namespace painter::gui::detail {

void (*FontOpened)(std::span<const std::uint8_t> Bytes) = nullptr;

}  // namespace painter::gui::detail
