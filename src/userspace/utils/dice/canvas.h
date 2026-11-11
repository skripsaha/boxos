/* canvas.h — a frame drawn on in pixels, within a rectangle. */
#ifndef DICE_CANVAS_H
#define DICE_CANVAS_H

#include "painter/core/color.h"
#include "painter/core/geometry.h"
#include "painter/tui/frame.h"

namespace dice {

using Color = painter::core::Color;

// Pixels are two to a cell down and one across; the top and the height of Clip
// are whole cells.
struct Canvas {
    painter::tui::Frame &Out;
    painter::core::Rect  Clip;

    bool Allows(int X, int Y) const
    {
        return X >= Clip.X && Y >= Clip.Y && X < Clip.X + Clip.Width && Y < Clip.Y + Clip.Height;
    }

    void Plot(int X, int Y, Color Ink) const
    {
        if (Allows(X, Y)) Out.PutPixel({X, Y}, Ink);
    }
};

}  // namespace dice

#endif
