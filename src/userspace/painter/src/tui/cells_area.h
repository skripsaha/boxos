#ifndef PAINTER_TUI_CELLS_AREA_H
#define PAINTER_TUI_CELLS_AREA_H

#include <cstdint>
#include <expected>
#include <optional>
#include <vector>

#include "box/cxx/error.h"
#include "box/items.h"
#include "painter/core/geometry.h"
#include "painter/gui/area.h"
#include "painter/tui/frame.h"
#include "painter/tui/stage.h"

namespace painter::tui {

/* A program's cells on the screen, in the items' language (box/items.h): an
 * area the display gives it, holding one cells item as large as the console
 * and on its grid, so a frame of cells shows as the console does. Open asks
 * the display how big the console is and refuses one smaller than the least
 * asked for, so the checks every Painter surface needs are made here once.
 * A frame presented goes as the cells that changed since the last. */
class CellsArea {
public:
    // Where the cells are shown: the whole screen while the program is in
    // front (a scene), over everything while it is in front (an overlay), or
    // over everything always, behind or not (a notice).
    enum class Where : std::uint8_t { Screen, Over, Always };

    static std::expected<CellsArea, StageError> Open(std::int32_t LeastCols, std::int32_t LeastRows, Where Shown) noexcept;

    CellsArea(CellsArea &&) noexcept            = default;
    CellsArea &operator=(CellsArea &&) noexcept = delete;
    CellsArea(const CellsArea &)                = delete;
    CellsArea &operator=(const CellsArea &)     = delete;
    ~CellsArea()                                = default;

    core::Size   Size() const noexcept { return Bounds; }
    bool         IsOpen() const noexcept { return Door.has_value(); }
    GlassKind    Glass() const noexcept { return Kind; }
    std::uint8_t Mosaics() const noexcept { return MosaicFlags; }
    // Where the console's caret stood when the area opened — after
    // everything this strand printed before opening it.
    core::Point  Cursor() const noexcept { return ConsoleCursor; }

    // The picture, as large as the area's cells: what changed of it said and
    // shown as one step — a transparent cell shows what lies beneath it, and
    // the picture's caret is the cells'.
    box::status Present(const Frame &Picture) noexcept;
    // The cells folded into the console's text, the console scrolled up
    // ScrollRows first: the area is gone then.
    box::status Commit(std::uint32_t ScrollRows) noexcept;
    box::status Close() noexcept;

private:
    explicit CellsArea(gui::Area Opened, core::Size BoundsValue) noexcept;

    std::optional<gui::Area> Door;
    core::Size               Bounds;
    GlassKind                Kind = GlassKind::Pixels;
    std::uint8_t             MosaicFlags = 0;
    core::Point              ConsoleCursor;
    std::vector<ItemCell>    Row;   // a row of the picture as cells, made once
};

}  // namespace painter::tui

#endif  // PAINTER_TUI_CELLS_AREA_H
