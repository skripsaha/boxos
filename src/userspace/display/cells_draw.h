#ifndef BOXOS_DISPLAY_CELLS_DRAW_H
#define BOXOS_DISPLAY_CELLS_DRAW_H

// How a cell looks — the console's look (box/items.h ITEM_CELLS), one rule
// for every cell on the glass, the console's and every program's: the
// character as the typesetter pictures it (typesetter.h) in a cell of its
// size, each pixel the ink and the paper mixed as light by how much of it the
// picture covers; a mark — a line under the glyph, a bar down its left side —
// in the mark's colour; the caret an eighth of the cell at its foot. A colour
// no program chose (COLOR_DEFAULT) is the screen's own; a see-through paper
// or ink lets what lies beneath show, pixel for pixel.
//
// A text page has no pixels: there a cell is laid over the cell beneath it
// (CellOver), its mark the mark's colour behind its glyph when the easel
// writes it.

#include <cstddef>
#include <cstdint>
#include <span>

#include "box/color.h"
#include "box/items.h"
#include "cell_grid.h"
#include "painter/core/canvas.h"
#include "painter/core/geometry.h"

class Typesetter;

struct CellsLook {
    Typesetter         *Type = nullptr;           // none on a text page
    std::int32_t        Width = 8, Height = 16;   // a cell's pixels; a text page's are eighths and sixteenths of a cell
    std::uint32_t       Text = COLOR_LIGHT_GRAY;  // the screen's own colours, triples
    std::uint32_t       Page = COLOR_BLACK;
    painter::core::Rect Grid{};                   // the console's cells on the glass
};

// Grid's cells drawn onto Target within its clip, the corner of the first
// at Corner on Target, the whole at Opacity.
void DrawCells(const CellGrid &Grid, const CellsLook &Look, painter::core::Canvas &Target, painter::core::Point Corner,
               float Opacity);

// Area of the glass — Pixels its corner, Stride from row to row — written as
// triples from Grid's cells, the corner of the first at Corner on the glass:
// every one of them opaque (CellGrid::Opaque), each pixel the very triple a
// canvas drawn by DrawCells and put on the glass would hold.
void LayCells(const CellGrid &Grid, const CellsLook &Look, std::uint32_t *Pixels, std::size_t Stride, painter::core::Rect Area,
              painter::core::Point Corner) noexcept;

// Grid's cells laid over a text page of Cols x Rows cells, the first at Col,
// Row of it: each that covers something over the cell beneath it, and each
// marked cell with its mark. Page is the screen's page colour.
void PageCells(const CellGrid &Grid, std::int32_t Col, std::int32_t Row, std::span<ItemCell> Cells, std::uint32_t Cols,
               std::uint32_t Rows, std::uint32_t Page) noexcept;

// The corner a cells item whose place is unsaid lies at on the glass: the
// first cell corner of the console's grid at or after Corner, the corner of
// its area (or its group) on the glass.
painter::core::Point OnTheGrid(const CellsLook &Look, painter::core::Point Corner) noexcept;

#endif
