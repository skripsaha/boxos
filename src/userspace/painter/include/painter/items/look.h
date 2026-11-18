#ifndef PAINTER_ITEMS_LOOK_H
#define PAINTER_ITEMS_LOOK_H

// How an item looks: each property as the program said it, or — unsaid — as
// its role is drawn by the theme, or else as the system draws it; and where
// the person insists, the theme's over the program's for every item with a
// role (system < theme < program < the person). The theme: the screen's text
// and page colours (what the keeper of the program in front says), a panel
// and quiet text made from them, and what the person sets of it — an accent,
// how round things are, whether they cast shadows, whether the theme insists;
// Inter for text and JetBrains Mono for code. A text in the role of a button
// or a field wears its own plate round its words. Every property is looked up
// here and nowhere else, so where a look comes from is decided once.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "box/items.h"
#include "painter/items/properties.h"
#include "painter/core/color.h"

namespace painter::items {

struct Theme {
    painter::core::Color Text   = painter::core::Color::Rgb(0xC0, 0xC0, 0xC0);
    painter::core::Color Page   = painter::core::Color::Rgb(0, 0, 0);
    painter::core::Color Panel  = painter::core::Color::Rgb(0x1C, 0x1C, 0x1C);
    painter::core::Color Quiet  = painter::core::Color::Rgb(0x80, 0x80, 0x80);
    painter::core::Color Accent = painter::core::Color::Rgb(0x4E, 0x8C, 0xFF);
    painter::core::Color OnAccent = painter::core::Color::Rgb(0xFF, 0xFF, 0xFF);
    std::string          TextFamily = "Inter";
    std::string          CodeFamily = "JetBrains Mono NL";
    std::int32_t         Round   = 8 * ITEM_POINT;   // a button's corners; a field's less, a panel's more
    bool                 Shadows = true;             // panels, buttons and fields raised by a soft shadow
    bool                 Insist  = false;            // the theme's look over the program's, for items with a role
    float                Faint   = 0.4f;             // how much of a thing not to be used is seen
    float                FocusWidth = 2, FocusGap = 3;   // the ring round what the keyboard reaches, in points
};

// What the person sets of the theme.
struct ThemeSaid {
    painter::core::Color Accent  = painter::core::Color::Rgb(0x4E, 0x8C, 0xFF);
    std::int32_t         Round   = 8 * ITEM_POINT;
    bool                 Shadows = true;
    bool                 Insist  = false;
};

// The screen's theme from its text and page colours, #RRGGBB, and what the
// person sets of it; what is drawn on the accent, light or dark, the one seen
// best on it.
Theme ThemeOf(std::uint32_t TextColor, std::uint32_t PageColor, const ThemeSaid &Person = {});

// A colour of the language as the engine takes it.
painter::core::Color ColorOf(const ItemColor &Of) noexcept;
painter::core::Color ThemeColor(const Theme &Of, std::uint16_t Which) noexcept;

// What an item of Kind in Role is drawn with, said or not.
std::vector<Fill>           FillsOf(const Properties &Said, std::uint8_t Kind, std::uint16_t Role, const Theme &With);
std::array<std::int32_t, 4> CornersOf(const Properties &Said, std::uint16_t Role, const Theme &With) noexcept;
std::vector<Shadow>         ShadowsOf(const Properties &Said, std::uint16_t Role, const Theme &With);
bool                        StrokeOf(const Properties &Said, std::uint16_t Role, const Theme &With, Stroke &Into);

// A text that wears its own plate round its words — a button's, a field's —
// the plate's fills, and how far in from the plate's corner the words lie.
bool              Plated(std::uint8_t Kind, std::uint16_t Role) noexcept;
std::vector<Fill> PlateOf(std::uint16_t Role);
ItemPoint         InsetOf(std::uint16_t Role) noexcept;

// Whether the keyboard reaches an item of Kind in Role, and whether it is not
// to be used.
bool FocusableOf(const Properties &Said, std::uint8_t Kind, std::uint16_t Role) noexcept;
bool DisabledOf(const Properties &Said) noexcept;

// A text's style, every part of it said: the item's own over its role's.
TextStyle StyleOf(const Properties &Said, std::uint16_t Role, const Theme &With);

std::uint16_t OpacityOf(const Properties &Said) noexcept;
bool          HiddenOf(const Properties &Said) noexcept;

}  // namespace painter::items

#endif
