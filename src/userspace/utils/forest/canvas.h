/* canvas.h — the picture as pixels, before it is cells. */
#ifndef FOREST_CANVAS_H
#define FOREST_CANVAS_H

#include <cstddef>
#include <span>
#include <vector>

#include "numbers.h"

namespace forest {

class Canvas {
public:
    Canvas(int Wide, int High) : Width(Wide), Height(High), Pixels(static_cast<std::size_t>(Wide) * High) {}

    bool Inside(int X, int Y) const { return X >= 0 && X < Width && Y >= 0 && Y < Height; }

    Color &At(int X, int Y) { return Pixels[static_cast<std::size_t>(Y) * Width + X]; }

    void Put(int X, int Y, Color Ink)
    {
        if (Inside(X, Y)) At(X, Y) = Ink;
    }

    // Weight 0..256 of Ink over what is there.
    void Blend(int X, int Y, Color Ink, int Weight)
    {
        if (Inside(X, Y)) At(X, Y) = Mixed(At(X, Y), Ink, Weight);
    }

    void Line(int X0, int Y0, int X1, int Y1, Color Ink);

    std::span<const Color> All() const { return Pixels; }

    int Width;
    int Height;

private:
    std::vector<Color> Pixels;
};

}  // namespace forest

#endif
