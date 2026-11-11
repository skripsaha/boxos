/* scene.h — where the garden is drawn: the pixels of the sky and the grass
 * over the soil, and a pen that keeps to them. A pixel is half a cell, so the
 * scene is as wide as it has cells and twice as high. */
#ifndef GARDEN_SCENE_H
#define GARDEN_SCENE_H

#include <cstdint>

#include "painter/core/color.h"
#include "painter/tui/frame.h"

namespace garden {

using Ink = painter::core::Color;

struct Scene {
    std::int32_t Width = 0;
    std::int32_t Top = 0;        // the first pixel row of the sky
    std::int32_t Ground = 0;     // where the grass begins: plants are rooted here
    std::int32_t SoilTop = 0;    // the first pixel row of the soil, a cell's edge
    std::int32_t Horizon() const { return Ground - 6; }
};

// A pen over a frame that draws inside the scene only.
class Pen {
public:
    Pen(painter::tui::Frame &Out, Scene Area) : Out(Out), Area(Area) {}

    void Dot(std::int32_t X, std::int32_t Y, Ink Color) const
    {
        if (Inside(X, Y)) Out.PutPixel({X, Y}, Color);
    }
    // A pixel taken toward Color by Weight of 255.
    void Veil(std::int32_t X, std::int32_t Y, Ink Color, std::uint8_t Weight) const
    {
        if (Inside(X, Y)) Out.BlendPixel({X, Y}, Color, Weight);
    }
    const Scene &Of() const { return Area; }

private:
    bool Inside(std::int32_t X, std::int32_t Y) const
    {
        return X >= 0 && X < Area.Width && Y >= Area.Top && Y < Area.SoilTop;
    }

    painter::tui::Frame &Out;
    Scene                Area;
};

}  // namespace garden

#endif
