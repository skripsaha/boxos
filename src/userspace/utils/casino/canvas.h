/* canvas.h — the casino's pixels: a Frame seen as square pixels, two to a
 * text cell, with the shapes the tables are drawn from. Edges are softened by
 * how much of a pixel a shape covers. */
#ifndef CASINO_CANVAS_H
#define CASINO_CANVAS_H

#include <cstdint>
#include <string_view>

#include "painter/core/color.h"
#include "painter/tui/frame.h"

namespace casino {

using Color = painter::core::Color;

// The color toward black (Factor under 1) or toward white (over 1).
Color Shade(Color Ink, float Factor);

class Canvas {
public:
    explicit Canvas(painter::tui::Frame &Target) : Out(Target) {}

    std::int32_t Wide() const { return Out.Size().Width; }
    std::int32_t High() const { return Out.PixelHeight(); }

    // One pixel, Cover of it (0 to 1) the ink.
    void Plot(std::int32_t X, std::int32_t Y, Color Ink, float Cover = 1.0f);
    void Fill(std::int32_t X, std::int32_t Y, std::int32_t Across, std::int32_t Down, Color Ink);
    void Disc(float Across, float Down, float Radius, Color Ink);
    void Ellipse(float Across, float Down, float ReachAcross, float ReachDown, Color Ink);
    void Ring(float Across, float Down, float Outer, float Inner, Color Ink);
    void Line(float FromAcross, float FromDown, float ToAcross, float ToDown, float Thick, Color Ink);
    // A chip seen from above: a rim, the body, and the stripes between.
    void Chip(float Across, float Down, float Radius, Color Body);

    // Letters five pixels by seven times Scale, from the top left corner.
    void Letters(std::int32_t X, std::int32_t Y, std::string_view Word, std::int32_t Scale, Color Ink);
    static std::int32_t LettersWide(std::string_view Word, std::int32_t Scale);
    // Whether the letter's 5 by 7 pixels are inked at Col (0 to 4), Row (0 to 6).
    static bool         Inked(char Letter, std::int32_t Col, std::int32_t Row);

private:
    painter::tui::Frame &Out;
};

}  // namespace casino

#endif
