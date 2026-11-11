/* canvas.h — the picture as pixels before it is cells: the glass's own half-block
 * pixels, a sprite with a see-through edge, and the shapes both are drawn with. */
#ifndef WIZARD_CANVAS_H
#define WIZARD_CANVAS_H

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "numbers.h"

namespace wizard {

// A Weight of 0..256 of Ink over what is there, by how much of the pixel a shape covers.
inline int Weighted(float Coverage)
{
    return static_cast<int>(Clamped(Coverage) * 256.0f);
}

class Canvas {
public:
    Canvas(int Wide, int High) : Width(Wide), Height(High), Pixels(static_cast<std::size_t>(Wide) * High) {}

    bool Inside(int X, int Y) const { return X >= 0 && X < Width && Y >= 0 && Y < Height; }
    Color &At(int X, int Y) { return Pixels[static_cast<std::size_t>(Y) * Width + X]; }
    Color  Read(int X, int Y) const { return Inside(X, Y) ? Pixels[static_cast<std::size_t>(Y) * Width + X] : Color(); }

    void Put(int X, int Y, Color Ink)
    {
        if (Inside(X, Y)) At(X, Y) = Ink;
    }
    void Blend(int X, int Y, Color Ink, int Weight)
    {
        if (Inside(X, Y) && Weight > 0) At(X, Y) = Mixed(At(X, Y), Ink, Weight);
    }
    void Cover(int X, int Y, Color Ink, float Coverage) { Blend(X, Y, Ink, Weighted(Coverage)); }
    void Add(int X, int Y, int Red, int Green, int Blue)
    {
        if (Inside(X, Y)) At(X, Y) = Added(At(X, Y), Red, Green, Blue);
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

// A picture with a see-through edge: what the wizard is drawn in, once, and
// blitted every frame.
class Sprite {
public:
    Sprite() = default;
    Sprite(int Wide, int High) : Width(Wide), Height(High), Texels(static_cast<std::size_t>(Wide) * High, 0) {}

    bool Inside(int X, int Y) const { return X >= 0 && X < Width && Y >= 0 && Y < Height; }

    // Ink over what is there by Coverage, the sprite's own see-through added.
    void Cover(int X, int Y, Color Ink, float Coverage);
    void Put(int X, int Y, Color Ink) { Cover(X, Y, Ink, 1.0f); }
    Color Read(int X, int Y) const { return Color::FromRaw(Texels[static_cast<std::size_t>(Y) * Width + X]); }
    int   Opacity(int X, int Y) const { return static_cast<int>(Texels[static_cast<std::size_t>(Y) * Width + X] >> 24); }

    int Width = 0;
    int Height = 0;

private:
    std::vector<std::uint32_t> Texels;
};

// How far a point is outside a shape, in the units the point is in: negative inside.
inline float EllipseDistance(float X, float Y, float Rx, float Ry)
{
    const float Nx = X / Rx, Ny = Y / Ry;
    const float Far = std::sqrt(Nx * Nx + Ny * Ny);
    return (Far - 1.0f) * std::min(Rx, Ry);
}

inline float SegmentDistance(float X, float Y, float Ax, float Ay, float Bx, float By)
{
    const float Dx = Bx - Ax, Dy = By - Ay;
    const float Along = Clamped(((X - Ax) * Dx + (Y - Ay) * Dy) / std::max(0.0001f, Dx * Dx + Dy * Dy));
    const float Px = X - (Ax + Dx * Along), Py = Y - (Ay + Dy * Along);
    return std::sqrt(Px * Px + Py * Py);
}

// The share of a pixel a shape covers when its edge is `Distance` pixels away.
inline float Covered(float Distance)
{
    return Clamped((0.5f - Distance) * 2.0f);
}

}  // namespace wizard

#endif
