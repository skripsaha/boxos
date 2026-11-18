/*
 * measure.cpp — a text measured beforehand, as the display lays it. See
 * painter/gui/measure.h.
 */

#include "painter/gui/measure.h"

#include <algorithm>
#include <cstddef>

#include "fonts.h"
#include "painter/items/properties.h"
#include "painter/items/text.h"
#include "typecase/text.h"

namespace painter::gui {

namespace detail {

core::PointF MeasureOn(gui::Glass On, std::string_view Utf8, gui::Role Of, float Width, const TextStyle &Style)
{
    if (On == gui::Glass::Text) {
        /* A character a cell, a line a row (box/items.h ITEM_GLASS_TEXT). */
        const std::u32string Characters = typecase::CharactersOf(std::string(Utf8));
        std::size_t          Widest = 0, Line = 0, Lines = 1;
        for (char32_t Each : Characters) {
            if (Each == U'\n') {
                Lines++;
                Line = 0;
                continue;
            }
            Widest = std::max(Widest, ++Line);
        }
        return core::PointF{static_cast<float>(Widest * ITEM_PAGE_CELL_WIDE), static_cast<float>(Lines * ITEM_PAGE_CELL_TALL)};
    }
    /* Said as an item says it, and read back as the display reads it, so
     * every number rounds as it rounds there. */
    constexpr std::uint32_t Item = 1;
    Changes                 Said;
    Said.Text(Item, Utf8);
    Said.Size(Item, core::PointF{Width, ByContent});
    Said.Style(Item, Style);
    items::Properties Read;
    std::uint16_t     Role = static_cast<std::uint16_t>(Of);
    for (std::uint16_t Property : Said.SaidOf(Item)) (void)items::Decode(Property, *Said.Said(Item, Property), Read, Role);
    const ItemPoint Measured = WithProgramFonts([&](const typecase::Fonts &Book) {
        return items::LayText(Read, static_cast<std::uint16_t>(Of), items::Theme{}, Book).Measured;
    });
    return core::PointF{static_cast<float>(Measured.X) / ITEM_POINT, static_cast<float>(Measured.Y) / ITEM_POINT};
}

}  // namespace detail

core::PointF Measure(const Area &On, std::string_view Utf8, gui::Role Of, float Width, const TextStyle &Style)
{
    return detail::MeasureOn(On.On(), Utf8, Of, Width, Style);
}

}  // namespace painter::gui
