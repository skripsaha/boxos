#ifndef BOXOS_DISPLAY_ITEM_TREE_H
#define BOXOS_DISPLAY_ITEM_TREE_H

// The items of one area as its program said them (box/items.h): each found by
// its number, in its parent group, among its siblings bottom to top, with the
// properties it said. What changed is marked for the drawing — its picture to
// be drawn again, or only where it lies — up through its groups, so a frame
// looks only where something changed; what was removed leaves where it was
// drawn behind, to be drawn over.
//
// Every change is held to the program's bounds (box/items.h ITEM_*_MOST),
// counted over all its areas in its Budget; a change past one is refused and
// changes nothing.
//
// A cells item holds its grid (cell_grid.h) once its grid is said: its cells
// written a run at a time, only where they changed marked for the drawing.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "box/error.h"
#include "box/items.h"
#include "cell_grid.h"
#include "painter/core/geometry.h"
#include "painter/items/properties.h"

namespace items {

// The language's values as read, how an item looks and a text laid out are
// Painter's (painter/items): the very code a program measures its texts with.
using namespace painter::items;

// What the drawing keeps of an item — its picture, its laid text — kept with
// it and let go with it (compositor.cpp).
struct Drawing;
struct DropDrawing {
    void operator()(Drawing *Of) const noexcept;
};

// What one program holds over all its areas, against its bounds.
struct Budget {
    std::uint32_t Items     = 0;
    std::size_t   TextBytes = 0;
    std::size_t   Cells     = 0;
};

struct Item {
    std::uint32_t              Id     = 0;
    std::uint8_t               Kind   = 0;
    std::uint16_t              Role   = ITEM_ROLE_NONE;
    std::uint32_t              Parent = ITEM_AREA;
    std::uint32_t              Depth  = 1;        // groups above it, and itself
    std::vector<std::uint32_t> Children;          // a group's, bottom to top
    Properties                 Said;
    std::shared_ptr<CellGrid>  Grid;              // a cells item's, once its grid is said

    // Marked for the drawing, and taken by it.
    bool Redraw  = true;     // its look, size or content changed: its picture is drawn again
    bool Moved   = true;     // where or how it lies changed: it, and what is in it
    bool Within  = true;     // something inside it is marked
    painter::core::Rect Shown{};   // where on the glass it was last drawn, its own pixels
    std::unique_ptr<Drawing, DropDrawing> Drawn;
};

class Tree {
public:
    explicit Tree(Budget &Held) noexcept : Held(Held) {}
    Tree(const Tree &)            = delete;
    Tree &operator=(const Tree &) = delete;
    ~Tree();

    error_t Create(std::uint32_t Id, const ItemCreate &How) noexcept;
    error_t Remove(std::uint32_t Id) noexcept;
    error_t Restack(std::uint32_t Id, const ItemPlaceAmong &How) noexcept;
    error_t Change(std::uint32_t Id, std::uint16_t Property, std::span<const std::uint8_t> Value) noexcept;
    error_t Reset(std::uint32_t Id, std::uint16_t Property) noexcept;
    // A cells item's cells written along a row: Bytes holds Run.Count of
    // them (ITEM_OP_CELLS).
    error_t Cells(std::uint32_t Id, const ItemCellsRun &Run, std::span<const std::uint8_t> Bytes) noexcept;
    // A cells item shows Grid, kept and written by another hand — the
    // console's by its model; its grid said as Grid's.
    error_t Show(std::uint32_t Id, std::shared_ptr<CellGrid> Grid) noexcept;
    // A cells item's grid changed under it: looked at by the next frame.
    void CellsChanged(std::uint32_t Id) noexcept;
    // Every item showing Block marked to be drawn again: its pixels changed.
    void BlockChanged(std::uint32_t Block) noexcept;
    // Every item marked to be drawn again: what everything is drawn from
    // changed — the scale, the theme.
    void RedrawAll() noexcept;

    Item       *Find(std::uint32_t Id) noexcept;
    const Item *Find(std::uint32_t Id) const noexcept;
    // The items in the area itself, bottom to top.
    const std::vector<std::uint32_t> &Top() const noexcept { return Roots; }
    std::size_t                       Count() const noexcept { return All.size(); }
    // Whether anything is marked since the drawing last took the marks.
    bool Marked() const noexcept { return AnyMarked; }
    void Taken() noexcept { AnyMarked = false; }
    // Where removed items were drawn: to be drawn over, and taken.
    std::vector<painter::core::Rect> &Vacated() noexcept { return Gone; }

private:
    std::vector<std::uint32_t> *ChildrenOf(std::uint32_t Parent) noexcept;
    void Mark(Item &Of, bool Picture, bool Place) noexcept;
    void MarkUp(std::uint32_t Parent) noexcept;
    void Forget(Item &Of) noexcept;
    error_t Regrid(Item &Of) noexcept;
    void Uses(const Item &Of, bool Add) noexcept;
    error_t Place(std::uint32_t Id, std::uint32_t Parent, std::uint32_t Above, std::uint32_t &Depth) noexcept;

    Budget                                                  &Held;
    std::unordered_map<std::uint32_t, Item>                 All;
    std::vector<std::uint32_t>                              Roots;
    std::unordered_map<std::uint32_t, std::unordered_set<std::uint32_t>> BlockUsers;
    std::vector<painter::core::Rect>                        Gone;
    bool                                                    AnyMarked = false;
};

// Every block an item's properties show: its picture, its canvas, its
// fills' and its stroke's pictures.
std::vector<std::uint32_t> BlocksOf(const Properties &Of);

}  // namespace items

#endif
