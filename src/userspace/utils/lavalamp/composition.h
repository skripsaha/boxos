/* composition.h — what a lamp is made of, as a person tunes it: its colours,
 * a palette's or its own, how many blobs of wax and how large, how fast they
 * flow and how far they glow. The settings give the first one (look.h), the
 * line and a lamp kept before change it, the panel on the screen changes it
 * as it runs, and a lamp is kept in a file as the text of one. */
#ifndef LAVALAMP_COMPOSITION_H
#define LAVALAMP_COMPOSITION_H

#include <array>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "painter/core/color.h"

namespace lavalamp {

using painter::core::Color;

struct Palette {
    const char *Name;
    Color       Wax, Rim, Glass;
};

inline constexpr std::array<Palette, 6> Palettes{{
    {"lava", Color::Rgb(0xFF, 0x3A, 0x6B), Color::Rgb(0x7A, 0x10, 0x40), Color::Rgb(0x10, 0x08, 0x20)},
    {"magma", Color::Rgb(0xFF, 0x9A, 0x2E), Color::Rgb(0xB8, 0x24, 0x0E), Color::Rgb(0x14, 0x08, 0x05)},
    {"ocean", Color::Rgb(0x4F, 0xE3, 0xFF), Color::Rgb(0x15, 0x50, 0xB0), Color::Rgb(0x03, 0x0C, 0x1C)},
    {"lime", Color::Rgb(0xC8, 0xFF, 0x4A), Color::Rgb(0x2F, 0x8C, 0x1A), Color::Rgb(0x07, 0x12, 0x08)},
    {"violet", Color::Rgb(0xD5, 0x8C, 0xFF), Color::Rgb(0x5B, 0x1A, 0x9E), Color::Rgb(0x0C, 0x05, 0x18)},
    {"sunset", Color::Rgb(0xFF, 0xD2, 0x5A), Color::Rgb(0xE2, 0x40, 0x5E), Color::Rgb(0x1C, 0x0A, 0x24)},
}};

/* The palette a composition's colours are not: its own. */
inline constexpr int Own = -1;

namespace limits {
inline constexpr int LeastBlobs = 2, MostBlobs = 24;
inline constexpr int LeastSize = 50, MostSize = 200;   // percent
inline constexpr int LeastFlow = 1, MostFlow = 9;
inline constexpr int LeastGlow = 0, MostGlow = 100;    // percent
}  // namespace limits

struct Composition {
    int   Palette = 0;      // of Palettes, or Own
    Color Wax  = Palettes[0].Wax;
    Color Rim  = Palettes[0].Rim;
    Color Glass = Palettes[0].Glass;
    int   Blobs = 7;
    int   Size  = 100;
    int   Flow  = 5;
    int   Glow  = 60;

    /* The palette's colours taken, by its place among Palettes. */
    void        Take(int Which);
    /* The palette's name, or "own". */
    std::string PaletteName() const;
    bool operator==(const Composition &) const = default;
};

/* The palette called Name, by its place; none for a name that is no palette. */
std::optional<int> PaletteNamed(std::string_view Name);

/* "#RRGGBB". */
std::string Spelled(Color Of);
/* A colour as a person types it: "#rrggbb" or "rrggbb"; none otherwise. */
std::optional<Color> ColorTyped(std::string_view Text);

/* A lamp as its file's text — its first line "lavalamp", then a line for each
 * thing it is made of — and back: a text that is no lamp, or a line that is
 * wrong, is said why. */
std::string                                Written(const Composition &Lamp);
std::expected<Composition, std::string>    Read(std::string_view Text);
/* Whether a file's first line says it is a lamp. */
bool                                       IsLamp(std::string_view Text);

}  // namespace lavalamp

#endif
