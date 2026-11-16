#ifndef TYPECASE_BOX_LINES_H
#define TYPECASE_BOX_LINES_H

/* The lines of frames, U+2500..U+257F, worked out for the cell: each arm of
 * a character from its edge to the arms across it, light, heavy or double;
 * dashed lines, rounded corners and diagonals. An arm leaves the cell at
 * the same pixels in every character, so a frame closes cell to cell. */

#include "cell_ink.h"
#include "typecase/rasterizer.h"

namespace typecase {

bool IsBoxLine(char32_t Character) noexcept;
void DrawBoxLine(char32_t Character, CellInk &Ink, Rasterizer &Pen);

}  // namespace typecase

#endif  // TYPECASE_BOX_LINES_H
