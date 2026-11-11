/* canvas.h — the picture as pixels before it is cells: the glass's own
 * half-block pixels, one across and two down to a cell. */
#ifndef SEABATTLE_CANVAS_H
#define SEABATTLE_CANVAS_H

#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

#include "numbers.h"

namespace seabattle {

// The share of a pixel a shape covers when its edge is `Distance` pixels away.
inline float Covered(float Distance)
{
    return Clamped((0.5f - Distance) * 2.0f);
}

class Canvas {
public:
    Canvas(int Wide, int High) : Width(Wide), Height(High), Pixels(static_cast<std::size_t>(Wide) * High) {}

    bool  Inside(int X, int Y) const { return X >= 0 && X < Width && Y >= 0 && Y < Height; }
    Color Read(int X, int Y) const { return Inside(X, Y) ? Pixels[static_cast<std::size_t>(Y) * Width + X] : Color(); }

    void Put(int X, int Y, Color Ink)
    {
        if (Inside(X, Y)) Pixels[static_cast<std::size_t>(Y) * Width + X] = Ink;
    }
    // Ink over what is there, by a weight of 0..256.
    void Blend(int X, int Y, Color Ink, int Weight)
    {
        if (Inside(X, Y) && Weight > 0) Put(X, Y, Mixed(Read(X, Y), Ink, Weight));
    }
    // Ink over what is there, by how much of the pixel a shape covers.
    void Cover(int X, int Y, Color Ink, float Coverage) { Blend(X, Y, Ink, static_cast<int>(Clamped(Coverage) * 256.0f)); }
    void Add(int X, int Y, int Red, int Green, int Blue)
    {
        if (Inside(X, Y)) Put(X, Y, Added(Read(X, Y), Red, Green, Blue));
    }
    void Fill(int X, int Y, int Wide, int High, Color Ink);
    void Line(int X0, int Y0, int X1, int Y1, Color Ink);
    // A soft round light added over what is there: Strength 0..256 at the middle, none at Radius.
    void Glow(float X, float Y, float Radius, Color Ink, int Strength);

    std::span<const Color> All() const { return Pixels; }

    int Width;
    int Height;

private:
    std::vector<Color> Pixels;
};

}  // namespace seabattle

#endif
