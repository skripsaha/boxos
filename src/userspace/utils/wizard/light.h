/* light.h — the light of the room: a few lamps, each with a map of how far its
 * light reaches, made once; what is lit is the wall's own colour times the sum
 * of the lamps at their levels of the moment (the fire flickers, the candles
 * gutter). Surfaces drawn over the room ask the same sum. */
#ifndef WIZARD_LIGHT_H
#define WIZARD_LIGHT_H

#include <cstdint>
#include <vector>

#include "canvas.h"

namespace wizard {

// What the light does to a colour: a 256th for each of its channels.
struct Tint {
    int Red = 256;
    int Green = 256;
    int Blue = 256;
};

enum class LampKind : std::uint8_t { Round, Shaft };

struct Lamp {
    LampKind Kind;
    float    X, Y;           // pixels: where it burns, or where the shaft begins
    float    Reach;          // pixels: how far it throws, or how long the shaft is
    Color    Shade;          // the colour of its light
    float    Aim;            // a shaft's slant: pixels sideways for each pixel down
    float    Width;          // a shaft's width at its start
};

class Lighting {
public:
    // Maps for the lamps over a W by H picture, and the light the room has when none burns.
    void Build(int Wide, int High, const std::vector<Lamp> &Lamps, Tint Ambient);

    // A lamp's level of the moment: 256 as it was made.
    void Set(std::size_t Which, int Level);
    // The light at a pixel, as a tint.
    Tint At(int X, int Y) const;
    // Every pixel of Under times the light, into Out.
    void Apply(const Canvas &Under, Canvas &Out) const;

private:
    int                                Width = 0;
    int                                Height = 0;
    Tint                               Ambient;
    std::vector<Tint>                  Shades;       // each lamp's colour
    std::vector<int>                   Levels;
    std::vector<std::vector<std::uint8_t>> Maps;
    std::vector<Tint>                  Weights;      // colour times level, 256ths of 256ths
};

}  // namespace wizard

#endif
