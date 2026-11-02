/* flame_palette.cpp — heat 0..255 as colour. Every palette is black at 0, so
 * what is not burning is not drawn, and ends near white, because the hottest
 * part of a flame is the part too bright to have a colour. */

#include "flame.h"

namespace {

struct Stop {
    std::uint8_t  At;
    std::uint32_t Color;
};

template <std::size_t Count>
FlamePalette Gradient(const Stop (&Stops)[Count])
{
    FlamePalette Palette{};
    for (std::size_t Segment = 0; Segment + 1 < Count; Segment++) {
        const Stop &From = Stops[Segment];
        const Stop &To   = Stops[Segment + 1];
        int         Span = To.At - From.At;
        for (int Heat = From.At; Heat <= To.At; Heat++) {
            auto Weight = static_cast<std::uint8_t>((Heat - From.At) * 255 / Span);
            Palette[static_cast<std::size_t>(Heat)] = color_mix(From.Color, To.Color, Weight);
        }
    }
    return Palette;
}

FlamePalette Custom(std::uint32_t Color)
{
    const Stop Stops[] = {
        {0,   0x000000},
        {70,  color_mix(0x000000, Color, 90)},
        {140, Color},
        {210, color_mix(Color, 0xFFFFFF, 150)},
        {255, 0xFFFFFF},
    };
    return Gradient(Stops);
}

}

FlamePalette FlameMakePalette(FlameInk Ink, std::uint32_t CustomColor)
{
    switch (Ink) {
    case FlameInk::Gas: {
        const Stop Stops[] = {
            {0, 0x000000}, {45, 0x00061E}, {95, 0x0A2AA0}, {150, 0x1E78FF},
            {205, 0x70E4FF}, {255, 0xFFFFFF},
        };
        return Gradient(Stops);
    }
    case FlameInk::Toxic: {
        const Stop Stops[] = {
            {0, 0x000000}, {45, 0x001400}, {95, 0x0A6400}, {150, 0x3CC000},
            {205, 0xB4FF30}, {255, 0xF4FFD8},
        };
        return Gradient(Stops);
    }
    case FlameInk::Spirit: {
        const Stop Stops[] = {
            {0, 0x000000}, {45, 0x1A0030}, {95, 0x5A10A0}, {150, 0xA040F0},
            {205, 0xE494FF}, {255, 0xFFF2FF},
        };
        return Gradient(Stops);
    }
    case FlameInk::Custom:
        return Custom(CustomColor);
    case FlameInk::Fire:
        break;
    }
    const Stop Stops[] = {
        {0, 0x000000}, {30, 0x2A0200}, {75, 0x8A1200}, {115, 0xD03C00},
        {150, 0xFF8210}, {185, 0xFFC030}, {220, 0xFFEE90}, {255, 0xFFFFFF},
    };
    return Gradient(Stops);
}

const char *FlamePlaceName(FlamePlace Place)
{
    static const char *const Names[FlamePlaceCount] = {"bottom", "top", "sides", "frame", "pit", "word"};
    return Names[static_cast<int>(Place)];
}

const char *FlameInkName(FlameInk Ink)
{
    static const char *const Names[FlameInkCount] = {"fire", "gas", "toxic", "spirit", "custom"};
    return Names[static_cast<int>(Ink)];
}
