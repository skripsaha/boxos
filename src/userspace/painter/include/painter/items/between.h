#ifndef PAINTER_ITEMS_BETWEEN_H
#define PAINTER_ITEMS_BETWEEN_H

// The values between two of a property: what an animation shows on its way
// (box/items.h ITEM_OP_ANIMATE). A property of numbers and colours is read as
// the numbers that walk — a place, a size, a turn, roundings, a path's
// points, fills' colours and gradients, a stroke, shadows, a text's size and
// spacing; colours as the eye sees them, through OKLab, each weighed by its
// opacity — and what does not walk as its form: two values walk between each
// other only when they are of one form. A property that has no numbers
// between — characters, a picture, a role — has none: the display fades the
// one look into the other instead.

#include <cstdint>
#include <span>
#include <vector>

#include "painter/items/look.h"
#include "painter/items/properties.h"

namespace painter::items {

struct Numbers {
    std::vector<double>        Values;   // what walks
    std::vector<std::uint32_t> Form;     // what does not
};

// Property said in Said as the item shows it — its role's value where it is
// unsaid — so an animation goes from what is seen to what is seen. False for
// a property that has no numbers between, and for a size its content decides,
// which only the display, laying it, knows.
bool SayAsShown(std::uint16_t Property, std::uint8_t Kind, std::uint16_t Role, const Theme &Look, Properties &Said);

// From and To made of one form where they can be: shadows of one count, those
// one has more of coming in or going out from nothing — seen through and
// spread nowhere; a stroke coming in or going out at no width.
void MakeAlike(std::uint16_t Property, Properties &From, Properties &To);

// Property of Of as numbers and form; false for one of no numbers, or unsaid.
bool NumbersOf(std::uint16_t Property, const Properties &Of, const Theme &Look, Numbers &Into);

// Values — as many as Like's numbers — said into Into's Property in the form
// Like holds it; a colour of the theme said as the colour it walked as.
void SayNumbers(std::uint16_t Property, std::span<const double> Values, const Properties &Like, const Theme &Look,
                Properties &Into);

}  // namespace painter::items

#endif
