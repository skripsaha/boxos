/* figure_art.h — what the wizard's parts share: his units on the sprite's
 * pixels, the tones he is shaded in, and the filling of a shape whose width is
 * a function of its height. */
#ifndef WIZARD_FIGURE_ART_H
#define WIZARD_FIGURE_ART_H

#include <algorithm>
#include <cmath>

#include "canvas.h"

namespace wizard {

// His units on a sprite: it covers 112 across (his middle at 56) and 116 down
// (his hat's tip at 4), and a unit is Unit pixels.
struct Grid {
    float Unit;

    float X(float Across) const { return (Across + 56.0f) * Unit; }
    float Y(float Down) const { return (Down + 4.0f) * Unit; }
    float Across(float Pixel) const { return Pixel / Unit - 56.0f; }
    float Down(float Pixel) const { return Pixel / Unit - 4.0f; }
    int   Width() const { return static_cast<int>(std::ceil(112.0f * Unit)) + 2; }
    int   Height() const { return static_cast<int>(std::ceil(116.0f * Unit)) + 2; }
};

inline constexpr Color HatTones[] = {Rgb(0x12, 0x15, 0x3C), Rgb(0x22, 0x2A, 0x74), Rgb(0x34, 0x44, 0xA8), Rgb(0x50, 0x64, 0xD2), Rgb(0x92, 0xA2, 0xF2)};
inline constexpr Color GoldTones[] = {Rgb(0x50, 0x34, 0x0C), Rgb(0x9C, 0x70, 0x1C), Rgb(0xE0, 0xAE, 0x38), Rgb(0xFF, 0xE6, 0x80), Rgb(0xFF, 0xF8, 0xC8)};
inline constexpr Color RobeTones[] = {Rgb(0x16, 0x0C, 0x34), Rgb(0x2A, 0x1A, 0x62), Rgb(0x44, 0x28, 0x98), Rgb(0x66, 0x44, 0xC4), Rgb(0x98, 0x7C, 0xEA)};
inline constexpr Color SkinTones[] = {Rgb(0x74, 0x44, 0x36), Rgb(0xB0, 0x72, 0x58), Rgb(0xDA, 0x9C, 0x7C), Rgb(0xF2, 0xC2, 0xA0), Rgb(0xFF, 0xE2, 0xC8)};
inline constexpr Color BeardTones[] = {Rgb(0x70, 0x76, 0x98), Rgb(0xA0, 0xA6, 0xC4), Rgb(0xCA, 0xCE, 0xE4), Rgb(0xEA, 0xEC, 0xF6), Rgb(0xFF, 0xFF, 0xFF)};
inline constexpr Color LeatherTones[] = {Rgb(0x1C, 0x10, 0x0A), Rgb(0x3E, 0x24, 0x16), Rgb(0x68, 0x40, 0x24), Rgb(0x90, 0x62, 0x38)};
inline constexpr Color WoodTones[] = {Rgb(0x2A, 0x1A, 0x0C), Rgb(0x54, 0x38, 0x1C), Rgb(0x84, 0x5C, 0x2E), Rgb(0xB4, 0x84, 0x48)};

// Where a shape is at a height: its left and right, in units.
struct Span {
    float Left;
    float Right;
};

// A shape between two heights whose edges at each height are Edge(Down), shaded
// by Shade(Across, Down, Middle, Inside): the place in his units, how far from the
// shape's middle it is (-1..1) and how deep under the nearest edge in pixels.
// Kept(Across, Down) is false where the shape has been cut away.
template <class Edges, class Shader, class Keep>
void FillProfile(Sprite &Out, const Grid &On, float Y0, float Y1, Edges Edge, Shader Shade, Keep Kept)
{
    const int Top = static_cast<int>(std::floor(On.Y(Y0))) - 1, Bottom = static_cast<int>(std::ceil(On.Y(Y1))) + 1;
    for (int Row = Top; Row <= Bottom; Row++) {
        const float Down = On.Down(static_cast<float>(Row) + 0.5f);
        const Span  Here = Edge(std::clamp(Down, Y0, Y1));
        const float Half = std::max(0.01f, (Here.Right - Here.Left) * 0.5f);
        const float Mid = (Here.Left + Here.Right) * 0.5f;
        const int   From = static_cast<int>(std::floor(On.X(Here.Left))) - 1, To = static_cast<int>(std::ceil(On.X(Here.Right))) + 1;
        for (int Column = From; Column <= To; Column++) {
            const float Across = On.Across(static_cast<float>(Column) + 0.5f);
            const float InsideX = std::min(Across - Here.Left, Here.Right - Across) * On.Unit;
            const float InsideY = std::min(Down - Y0, Y1 - Down) * On.Unit;
            const float Inside = std::min(InsideX, InsideY);
            const float Share = Clamped(0.5f + Inside);
            if (Share <= 0.0f || !Kept(Across, Down)) continue;
            Out.Cover(Column, Row, Shade(Across, Down, (Across - Mid) / Half, Inside), Share);
        }
    }
}

template <class Edges, class Shader>
void FillProfile(Sprite &Out, const Grid &On, float Y0, float Y1, Edges Edge, Shader Shade)
{
    FillProfile(Out, On, Y0, Y1, Edge, Shade, [](float, float) { return true; });
}

// The robe, shoes and trim into the back sprite (figure_robe.cpp).
void DrawRobe(Sprite &Out, const Grid &On);

}  // namespace wizard

#endif
