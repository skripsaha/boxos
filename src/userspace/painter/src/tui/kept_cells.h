#ifndef PAINTER_TUI_KEPT_CELLS_H
#define PAINTER_TUI_KEPT_CELLS_H

#include <cstdint>
#include <expected>
#include <memory>

#include "cells_area.h"
#include "painter/tui/stage.h"

namespace painter::tui {

/* Cells a program keeps on the screen over the console, see-through until
 * something is put in them: what Surface and ConsoleSurface share. A part of
 * a frame put in (Paint, Composite) is shown at once; ConsoleSurface's are
 * folded into the console's text at last (Commit). */
class KeptCells {
public:
    static std::expected<KeptCells, StageError> Open(std::int32_t LeastCols, std::int32_t LeastRows,
                                                     CellsArea::Where Shown) noexcept;

    KeptCells(KeptCells &&) noexcept;
    KeptCells &operator=(KeptCells &&) noexcept;
    KeptCells(const KeptCells &) = delete;
    KeptCells &operator=(const KeptCells &) = delete;
    ~KeptCells();

    core::Size Size() const noexcept;
    bool IsOpen() const noexcept;
    GlassKind Glass() const noexcept;
    std::uint8_t Mosaics() const noexcept;
    std::expected<core::Point, box::error> Cursor() const noexcept;

    box::status Paint(core::Point Origin, const Frame &FrameBuffer) noexcept;
    box::status Paint(core::Point Origin, const Frame &FrameBuffer,
                      core::Rect Region) noexcept;
    box::status Composite(core::Point Origin, const Frame &FrameBuffer) noexcept;
    box::status Composite(core::Point Origin, const Frame &FrameBuffer,
                          core::Rect Region) noexcept;
    box::status Clear() noexcept;
    /* The console's caret, for ConsoleSurface: where the console goes on
     * once the cells are part of its text. */
    box::status SetCursor(core::Point At) noexcept;
    box::status Commit(std::uint32_t ScrollRows = 0) noexcept;
    box::status Close() noexcept;

private:
    struct Impl;
    explicit KeptCells(std::unique_ptr<Impl> State) noexcept;

    std::unique_ptr<Impl> State;
};

}  // namespace painter::tui

#endif  // PAINTER_TUI_KEPT_CELLS_H
