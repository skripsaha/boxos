/* shapes.h — what the pictures are drawn with: an ellipse or a thick line
 * filled with a colour that depends on where in it a pixel is, its edge worked
 * out to a fraction of a pixel so nothing is a staircase. A target is a Canvas
 * or a Sprite. */
#ifndef WIZARD_SHAPES_H
#define WIZARD_SHAPES_H

#include <algorithm>
#include <cmath>

#include "canvas.h"

namespace wizard {

// An ellipse at Cx, Cy of Rx by Ry pixels. Shade(Nx, Ny, Inside) gives the colour at
// a place whose offsets from the middle are Nx, Ny in radii (-1..1), and whose
// depth under the edge is Inside pixels (negative on the edge's outside).
template <class Target, class Shader>
void FillEllipse(Target &Out, float Cx, float Cy, float Rx, float Ry, Shader Shade)
{
    const int Left = static_cast<int>(std::floor(Cx - Rx - 1.0f)), Right = static_cast<int>(std::ceil(Cx + Rx + 1.0f));
    const int Top = static_cast<int>(std::floor(Cy - Ry - 1.0f)), Bottom = static_cast<int>(std::ceil(Cy + Ry + 1.0f));
    for (int Y = Top; Y <= Bottom; Y++) {
        for (int X = Left; X <= Right; X++) {
            const float Dx = static_cast<float>(X) + 0.5f - Cx, Dy = static_cast<float>(Y) + 0.5f - Cy;
            const float Distance = EllipseDistance(Dx, Dy, Rx, Ry);
            const float Share = Covered(Distance);
            if (Share > 0.0f) Out.Cover(X, Y, Shade(Dx / Rx, Dy / Ry, -Distance), Share);
        }
    }
}

// A line from A to B, Ra thick at A and Rb at B (radii). Shade(Along, Across, Inside): how far
// along it the pixel is (0..1), how far across (-1..1) and how deep under the edge.
template <class Target, class Shader>
void FillStroke(Target &Out, float Ax, float Ay, float Bx, float By, float Ra, float Rb, Shader Shade)
{
    const float Reach = std::max(Ra, Rb) + 1.0f;
    const int Left = static_cast<int>(std::floor(std::min(Ax, Bx) - Reach)), Right = static_cast<int>(std::ceil(std::max(Ax, Bx) + Reach));
    const int Top = static_cast<int>(std::floor(std::min(Ay, By) - Reach)), Bottom = static_cast<int>(std::ceil(std::max(Ay, By) + Reach));
    const float Dx = Bx - Ax, Dy = By - Ay;
    const float Length2 = std::max(0.0001f, Dx * Dx + Dy * Dy);
    for (int Y = Top; Y <= Bottom; Y++) {
        for (int X = Left; X <= Right; X++) {
            const float Px = static_cast<float>(X) + 0.5f - Ax, Py = static_cast<float>(Y) + 0.5f - Ay;
            const float Along = Clamped((Px * Dx + Py * Dy) / Length2);
            const float Nearx = Px - Dx * Along, Neary = Py - Dy * Along;
            const float Radius = Ra + (Rb - Ra) * Along;
            const float Distance = std::sqrt(Nearx * Nearx + Neary * Neary) - Radius;
            const float Share = Covered(Distance);
            if (Share <= 0.0f) continue;
            const float Side = (Px * Dy - Py * Dx) / std::sqrt(Length2);
            Out.Cover(X, Y, Shade(Along, Clamped(Side / std::max(0.01f, Radius), -1.0f, 1.0f), -Distance), Share);
        }
    }
}

}  // namespace wizard

#endif
