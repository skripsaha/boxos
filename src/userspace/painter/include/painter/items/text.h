#ifndef PAINTER_ITEMS_TEXT_H
#define PAINTER_ITEMS_TEXT_H

// A text item laid out as the display lays it: its role's style under what it
// says (painter/items/look.h), its stretches, its box — all in points, the
// screen's scale coming only when it is drawn — and the inks its stretches
// say. One way for the display drawing it and for a program measuring it
// beforehand, so the two never differ.

#include <cstdint>
#include <vector>

#include "box/items.h"
#include "painter/core/color.h"
#include "painter/items/look.h"
#include "painter/items/properties.h"
#include "typecase/fonts.h"
#include "typecase/layout.h"

namespace painter::items {

struct LaidText {
    typecase::Layout         Laid;
    std::vector<core::Color> Inks;         // the colours its stretches say: ink 1 on
    ItemPoint                Measured{};   // its size by its content, of ITEM_POINT — a plate's room round it too
    ItemPoint                Inset{};      // where its words lie from its corner: in from a plate's (look.h Plated)
};

// Said of a text item in Role, laid out in Book. Throws std::bad_alloc only.
LaidText LayText(const Properties &Said, std::uint16_t Role, const Theme &Look, const typecase::Fonts &Book);

}  // namespace painter::items

#endif
