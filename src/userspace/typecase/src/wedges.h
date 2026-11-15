#ifndef TYPECASE_WEDGES_H
#define TYPECASE_WEDGES_H

/* The wedges of Symbols for Legacy Computing, U+1FB3C..U+1FB6F, worked out
 * for the cell: a part of it cut off by a diagonal from one point of its
 * edge to another, or a triangle to its middle. The points are the corners,
 * the middles of the top and the bottom, and the thirds of the sides — the
 * very edges the sextants stand on — so a wedge meets the mosaics beside it,
 * and each with its complement fills the cell once. */

#include "cell_ink.h"
#include "typecase/rasterizer.h"

namespace typecase {

bool IsWedge(char32_t Character) noexcept;
void DrawWedge(char32_t Character, CellInk &Ink, Rasterizer &Pen);

}  // namespace typecase

#endif  // TYPECASE_WEDGES_H
