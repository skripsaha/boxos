/*
 * text.cpp — a text item laid out as the display lays it. See
 * painter/items/text.h.
 */

#include "painter/items/text.h"

#include <cmath>
#include <string>
#include <utility>

namespace painter::items {

namespace {

constexpr float PerPoint = 1.0f / ITEM_POINT;

/* A style of the language as Typecase takes it: all of it for the text's
 * own, only what it says for a stretch's. */
typecase::Style StyleFor(const TextStyle &Of, bool Whole)
{
    typecase::Style Made;
    if (Whole || (Of.Said & ITEM_SAYS_FAMILY)) Made.Family = Of.Family;
    if (Whole || (Of.Said & ITEM_SAYS_SIZE)) Made.Size = static_cast<float>(Of.Size) * PerPoint;
    if (Whole || (Of.Said & ITEM_SAYS_WEIGHT)) Made.Weight = Of.Weight;
    if (Whole || (Of.Said & ITEM_SAYS_ITALIC)) Made.Italic = Of.Italic;
    if (Whole || (Of.Said & ITEM_SAYS_SPACING)) Made.LetterSpacing = static_cast<float>(Of.Spacing) * PerPoint;
    if ((Whole && Of.LineHeight > 0) || (Of.Said & ITEM_SAYS_LINE_HEIGHT))
        Made.LineHeight = static_cast<float>(Of.LineHeight) / ITEM_ONE;
    if (Whole || (Of.Said & ITEM_SAYS_UNDERLINE)) Made.Underline = Of.Underline;
    if (Whole || (Of.Said & ITEM_SAYS_STRIKE)) Made.Strike = Of.Strike;
    return Made;
}

}  // namespace

LaidText LayText(const Properties &Said, std::uint16_t Role, const Theme &Look, const typecase::Fonts &Book)
{
    const TextStyle          Base = StyleOf(Said, Role, Look);
    typecase::Text           Text = typecase::Text::Of(Said.Text ? *Said.Text : std::string(), StyleFor(Base, true));
    std::vector<core::Color> Inks;
    Text.Secret = Said.Secret && *Said.Secret;
    if (Said.Spans)
        for (const Span &Each : *Said.Spans) {
            typecase::Span Made{Each.From, Each.To, StyleFor(Each.Style, false)};
            if (Each.Style.Said & ITEM_SAYS_COLOR) {
                Inks.push_back(ColorOf(Each.Style.Color));
                Made.Style.Ink = static_cast<std::uint32_t>(Inks.size());
            }
            Text.Spans.push_back(std::move(Made));
        }
    typecase::TextBox Box;
    const ItemPoint   Size = Said.Size ? *Said.Size : ItemPoint{ITEM_BY_CONTENT, ITEM_BY_CONTENT};
    if (Size.X != ITEM_BY_CONTENT) Box.Width = static_cast<float>(Size.X) * PerPoint;
    if (Size.Y != ITEM_BY_CONTENT) Box.Height = static_cast<float>(Size.Y) * PerPoint;
    if (Said.Box) {
        Box.Align    = static_cast<typecase::TextAlign>(Said.Box->Align);
        Box.Overflow = static_cast<typecase::TextOverflow>(Said.Box->Overflow);
        Box.Wrap     = Said.Box->Wrap != 0;
        Box.MaxLines = Said.Box->MaxLines;
    }
    typecase::Layout Laid     = typecase::Layout::Of(Text, Box, Book);
    const ItemPoint  Measured = ItemPoint{static_cast<std::int32_t>(std::lround(Laid.Width() * ITEM_POINT)),
                                          static_cast<std::int32_t>(std::lround(Laid.Height() * ITEM_POINT))};
    return LaidText{std::move(Laid), std::move(Inks), Measured};
}

}  // namespace painter::items
