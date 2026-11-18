#ifndef PAINTER_GUI_FONT_OPENED_H
#define PAINTER_GUI_FONT_OPENED_H

// Who hears of a font an area of the program opens (Area::OpenFont): the
// program's fonts kept for measuring (fonts.cpp), set as the program starts
// when the program measures — none in a program that does not, which so
// carries no reader of fonts.

#include <cstdint>
#include <span>

namespace painter::gui::detail {

extern void (*FontOpened)(std::span<const std::uint8_t> Bytes);

}  // namespace painter::gui::detail

#endif
