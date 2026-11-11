/* icons.h — the things for the cauldron as pictures: eight pixels by eight,
 * each drawn by hand and shaded as the room's light finds it. The same pictures
 * are what falls into the cauldron. */
#ifndef WIZARD_ICONS_H
#define WIZARD_ICONS_H

#include <cstddef>

#include "painter/tui/frame.h"

#include "canvas.h"

namespace wizard {

inline constexpr int IconSize = 8;

// The picture of Ingredients()[Which] with its top left at X, Y. Lit is how
// much light is on it, 0..256 (the shelf's lamp); Glint 0..256 is what the
// chosen one gives back.
void DrawIcon(Canvas &Out, int X, int Y, std::size_t Which, int Lit, int Glint);

// The same picture drawn in the cells of a frame, its top left at column X and
// pixel row Y, lit as it is.
void DrawIconOnFrame(painter::tui::Frame &Out, int X, int Y, std::size_t Which);

// The colour at the middle of a picture: what its splash is.
Color IconColor(std::size_t Which);

}  // namespace wizard

#endif
