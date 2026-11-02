/* flame_paint.cpp — the fire on the glass. The console font has the upper
 * half block, so a cell shows two pixels one above the other: the block's ink
 * is the pixel above and the paper behind it the pixel below. */

#include "box/vga.h"

#include "flame.h"

namespace {

constexpr char UpperHalfBlock = static_cast<char>(0xDF);

}

void FlamePaintFire(const FlameFire &Fire, const FlamePalette &Palette, TextCell *Frame, int Rows, int Cols)
{
    for (int Row = 0; Row < Rows; Row++) {
        for (int Col = 0; Col < Cols; Col++) {
            TextCell &Cell = Frame[static_cast<std::size_t>(Row) * Cols + Col];
            Cell.ch = UpperHalfBlock;
            Cell.fg = Palette[Fire.At(Col, Row * 2)];
            Cell.bg = Palette[Fire.At(Col, Row * 2 + 1)];
        }
    }
}
