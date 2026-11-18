#ifndef PAINTER_ITEMS_LOOK_H
#define PAINTER_ITEMS_LOOK_H

// How an item looks: each property as the program said it, or — unsaid — as
// its role is drawn by the theme, or else as the system draws it. The theme is
// the screen's own until themes are files the person keeps: the screen's text
// and page colours (what the keeper of the program in front says), a panel
// and quiet text made from them, an accent, Inter for text and JetBrains Mono
// for code. Every property is looked up here and nowhere else, so where a
// look comes from is decided once.

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
};

// The screen's theme from its text and page colours, #RRGGBB.
Theme ThemeOf(std::uint32_t TextColor, std::uint32_t PageColor);

// A colour of the language as the engine takes it.
painter::core::Color ColorOf(const ItemColor &Of) noexcept;
painter::core::Color ThemeColor(const Theme &Of, std::uint16_t Which) noexcept;

// What an item of Kind in Role is drawn with, said or not.
std::vector<Fill>           FillsOf(const Properties &Said, std::uint8_t Kind, std::uint16_t Role);
std::array<std::int32_t, 4> CornersOf(const Properties &Said, std::uint16_t Role) noexcept;
std::vector<Shadow>         ShadowsOf(const Properties &Said, std::uint16_t Role);
bool                        StrokeOf(const Properties &Said, std::uint16_t Role, Stroke &Into);

// A text's style, every part of it said: the item's own over its role's.
TextStyle StyleOf(const Properties &Said, std::uint16_t Role, const Theme &With);

std::uint16_t OpacityOf(const Properties &Said) noexcept;
bool          HiddenOf(const Properties &Said) noexcept;

}  // namespace painter::items

#endif
