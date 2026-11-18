#ifndef PAINTER_ITEMS_PROPERTIES_H
#define PAINTER_ITEMS_PROPERTIES_H

// The items' language read (box/items.h): a property's value as the program
// wrote it, held to its bounds and kept as the display keeps it — and as a
// program reads back what it said, to measure it as the display lays it. Nothing in
// the bytes is trusted — every count, length and number is checked before it
// is used, and a value that breaks the language is refused whole, its reason
// said, the property left as it was.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "box/error.h"
#include "box/items.h"

namespace painter::items {

struct Fill {
    std::uint8_t          Kind    = ITEM_FILL_COLOR;
    std::uint8_t          Extend  = ITEM_EXTEND_PAD;
    std::uint16_t         Opacity = ITEM_OPAQUE;
    ItemColor             Color{};
    std::uint16_t         Theme   = 0;
    ItemPoint             From{}, To{};          // a linear gradient's ends; a radial's middle is From
    std::int32_t          Radius  = 0;
    ItemPoint             Focus{};
    std::int32_t          Start   = 0;           // a conic's, of ITEM_TURN
    std::vector<ItemStop> Stops;
    std::uint32_t         Block   = 0;           // a picture's
    ItemPoint             At{}, Size{};
    bool                  Sharp   = false;
};

struct Stroke {
    std::int32_t              Width      = ITEM_POINT;
    std::uint8_t              Cap        = ITEM_CAP_BUTT;
    std::uint8_t              Join       = ITEM_JOIN_MITER;
    std::uint8_t              Align      = ITEM_ALIGN_CENTER;
    std::int32_t              MiterLimit = 4 * ITEM_POINT;
    std::int32_t              DashOffset = 0;
    std::vector<std::int32_t> Dashes;
    Fill                      With;
};

struct Shadow {
    ItemPoint    Offset{};
    std::int32_t Blur   = 0;
    std::int32_t Spread = 0;
    ItemColor    Color{};
    bool         Inner  = false;
};

struct TextStyle {
    std::uint16_t Said       = 0;    // ITEM_SAYS_*: what of the rest is said
    std::string   Family;
    std::int32_t  Size       = 0;
    std::uint16_t Weight     = 400;
    bool          Italic     = false;
    std::int32_t  Spacing    = 0;
    std::int32_t  LineHeight = 0;
    bool          Underline  = false;
    bool          Strike     = false;
    ItemColor     Color{};
};

struct Span {
    std::uint32_t From = 0, To = 0;
    TextStyle     Style;
};

struct Shape {
    std::uint8_t              Form = ITEM_FORM_RECT;
    std::uint8_t              Rule = 0;
    std::vector<std::uint8_t> Steps;
    std::vector<ItemPoint>    Points;
};

struct PictureOf {
    std::uint32_t Block = 0;
    std::uint8_t  Fit   = ITEM_FIT_STRETCH;
    bool          Sharp = false;
};

// Every property an item may say, each unsaid until it is: what is unsaid is
// its role's, as the theme draws it (painter/items/look.h).
struct Properties {
    std::optional<ItemPoint>                   Place, Size, Scale, Anchor;
    std::optional<std::uint16_t>               Opacity;
    std::optional<bool>                        Hidden, Clip, Secret, Focusable, Disabled;
    std::optional<std::int32_t>                Turn;
    std::optional<std::array<std::int32_t, 4>> Corners;
    std::optional<items::Shape>                Shape;
    std::optional<std::vector<Fill>>           Fills;
    std::optional<items::Stroke>               Stroke;
    std::optional<std::vector<Shadow>>         Shadows;
    std::optional<std::string>                 Text, Caption;
    std::optional<TextStyle>                   Style;
    std::optional<std::vector<Span>>           Spans;
    std::optional<ItemTextBox>                 Box;
    std::optional<PictureOf>                   Picture;
    std::optional<std::uint32_t>               Canvas;
    std::optional<ItemGrid>                    Grid;
    std::optional<ItemCaret>                   Caret;
};

// Bytes, the value of Property, said into Into: OK, or why not — the
// property as it was. Role is set apart (ITEM_PROP_ROLE): it is the item's,
// not a look's, and comes back in Role.
error_t Decode(std::uint16_t Property, std::span<const std::uint8_t> Bytes, Properties &Into,
               std::uint16_t &Role) noexcept;

// Property unsaid in Into: OK, or why not.
error_t Unsay(std::uint16_t Property, Properties &Into) noexcept;

// Property in Into as From holds it, said or unsaid; the rest as it was.
void Assign(std::uint16_t Property, const Properties &From, Properties &Into);

// Whether Of says Property.
bool IsSaid(std::uint16_t Property, const Properties &Of) noexcept;

// The bytes of text Into holds, said and in spans' families, counted against
// the program's bound (ITEM_TEXTS_MOST).
std::size_t TextBytes(const Properties &Of) noexcept;

}  // namespace painter::items

#endif
