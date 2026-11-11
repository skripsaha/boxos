/* shape.h — one die drawn in half-block pixels: a cube with pips for the d6,
 * and for the others the shape its solid casts — a triangle, a diamond, a
 * kite, a pentagon, a hexagon with a triangle in it — with the face as a
 * number; a disc for any die that has no solid. Smaller than a die can be
 * drawn, a tile of the number alone. */
#ifndef DICE_SHAPE_H
#define DICE_SHAPE_H

#include <cstdint>

#include "painter/core/geometry.h"

#include "canvas.h"

namespace dice {

enum class Shape : std::uint8_t { Triangle, Cube, Diamond, Kite, Pentagon, Hexagon, Disc };

struct DieStyle {
    Color Body;
    Color Ink;     // pips and number
    Color Edge;    // the outline
    Color Line;    // the lines inside a solid's picture
};

Shape ShapeOf(int Sides);

// A die of Size pixels square with its corner at At.
void DrawShadow(const Canvas &On, painter::core::Point At, int Size, Shape Of, Color Shade);
void DrawDie(const Canvas &On, painter::core::Point At, int Size, int Sides, int Face, const DieStyle &Style);
// A number in a tile one cell high and Digits + 2 wide, its corner at At.
void DrawTile(const Canvas &On, painter::core::Point At, int Digits, int Face, const DieStyle &Style);

// The cells a number takes.
int DigitsOf(int Number);

}  // namespace dice

#endif
