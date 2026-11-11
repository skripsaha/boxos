/* canvas.h — the casino's pixels: a Frame seen as square pixels, two to a
 * text cell, with the shapes the tables are drawn from. The shapes are
 * Painter's FineCanvas: an edge is settled on the glyph of the font that
 * shows it best, half a cell across, and the pixels are for what is drawn
 * point by point. */
#ifndef CASINO_CANVAS_H
#define CASINO_CANVAS_H

#include <cstdint>
#include <string_view>

#include "painter/core/color.h"
#include "painter/tui/fine_canvas.h"
#include "painter/tui/frame.h"

namespace casino {

using Color = painter::core::Color;

// The color toward black (Factor under 1) or toward white (over 1).
Color Shade(Color Ink, float Factor);

// Whether the display says the glass is the text console's: its 16 colors turn every softened
// edge into noise, so what is new is drawn there with hard edges and no shadows.
inline bool Hard(const painter::tui::Frame &Out)
{
    return Out.Glass() == painter::tui::GlassKind::Text;
}

class Canvas {
public:
    explicit Canvas(painter::tui::Frame &Target, bool Crisp = false) : Out(Target), Fine(Target), Edged(Crisp) {}

    // Whether shadows are soft and the pixels plotted blend: false on the text console.
    bool Smooth() const { return !Edged; }

    std::int32_t Wide() const { return Out.Size().Width; }
    std::int32_t High() const { return Out.PixelHeight(); }

    // One pixel, Cover of it (0 to 1) the ink.
    void Plot(std::int32_t X, std::int32_t Y, Color Ink, float Cover = 1.0f);
    void Fill(std::int32_t X, std::int32_t Y, std::int32_t Across, std::int32_t Down, Color Ink);
    void Disc(float Across, float Down, float Radius, Color Ink, float Opacity = 1.0f);
    void Ellipse(float Across, float Down, float ReachAcross, float ReachDown, Color Ink, float Opacity = 1.0f);
    void Ring(float Across, float Down, float Outer, float Inner, Color Ink);
    // A box with rounded corners, Left and Top fractions of a pixel allowed.
    void Rounded(float Left, float Top, float Across, float Down, float Radius, Color Ink, float Opacity = 1.0f);
    void Line(float FromAcross, float FromDown, float ToAcross, float ToDown, float Thick, Color Ink);
    // A chip seen from above: a rim, the body, and the stripes between.
    void Chip(float Across, float Down, float Radius, Color Body);

    // Letters five pixels by seven times Scale, from the top left corner.
    void Letters(std::int32_t X, std::int32_t Y, std::string_view Word, std::int32_t Scale, Color Ink);
    static std::int32_t LettersWide(std::string_view Word, std::int32_t Scale);
    // Whether the letter's 5 by 7 pixels are inked at Col (0 to 4), Row (0 to 6).
    static bool         Inked(char Letter, std::int32_t Col, std::int32_t Row);

private:
    painter::tui::Frame     &Out;
    painter::tui::FineCanvas Fine;
    bool                     Edged;
};

}  // namespace casino

#endif
