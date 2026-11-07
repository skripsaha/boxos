/* flame_paint.cpp — the fire on the glass. The console font has the upper
 * half block, so a cell shows two pixels one above the other: the block's ink
 * is the pixel above and the paper behind it the pixel below. */

#include "flame.h"
#include "painter/tui/glyph.h"

void FlamePaintFire(const FlameFire &Fire, const FlamePalette &Palette, painter::tui::Frame &Frame, int Rows,
                    int Cols)
{
    for (int Row = 0; Row < Rows; Row++) {
        for (int Col = 0; Col < Cols; Col++) {
            const std::uint8_t Upper = Fire.At(Col, Row * 2);
            const std::uint8_t Lower = Fire.At(Col, Row * 2 + 1);
            painter::core::Color Foreground = painter::core::Color::FromRaw(Palette[Upper]);
            painter::core::Color Background = painter::core::Color::FromRaw(Palette[Lower]);
            Frame.Put({Col, Row}, painter::tui::Cell{painter::tui::Glyphs::UpperHalf, Foreground, Background});
        }
    }
}
