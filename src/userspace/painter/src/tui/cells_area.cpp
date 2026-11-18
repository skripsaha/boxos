#include "cells_area.h"

#include <limits>
#include <new>
#include <utility>

#include "box/display.h"
#include "box/error.h"

namespace painter::tui {

namespace {

/* The one item of the area: its cells. */
constexpr std::uint32_t CellsItem = 1;

box::error Cause(int Code) { return Code == 0 ? box::error{} : box::error{box_errno_of(Code)}; }

StageError Error(StageErrorKind Kind, box::error CauseValue, core::Size Actual, core::Size Required)
{
    return StageError{Kind, CauseValue, Actual, Required};
}

/* A cell of a frame as the language carries it: one that shows nothing as
 * the cell that shows nothing; any other as its character, its colours as
 * the console's, its mark. */
ItemCell CellOf(const Cell &Of) noexcept
{
    if (Of.IsTransparent()) return item_cell_empty();
    return ItemCell{static_cast<std::uint32_t>(Of.Glyph), Of.Foreground.Raw(), Of.Background.Raw(), Of.MarkColor.Raw(),
                    static_cast<std::uint8_t>(Of.Mark), {0, 0, 0}};
}

}  // namespace

CellsArea::CellsArea(gui::Area Opened, core::Size BoundsValue) noexcept : Door(std::move(Opened)), Bounds(BoundsValue)
{
    Kind          = Door->On() == gui::Glass::Text ? GlassKind::Text : GlassKind::Pixels;
    MosaicFlags   = Door->Mosaics();
    ConsoleCursor = Door->ConsoleCaret();
}

std::expected<CellsArea, StageError> CellsArea::Open(std::int32_t LeastCols, std::int32_t LeastRows, Where Shown) noexcept
{
    const core::Size Required{LeastCols, LeastRows};
    if (LeastCols < 0 || LeastRows < 0)
        return std::unexpected(Error(StageErrorKind::TooSmall, box::error{box::errc::invalid_argument}, {}, Required));

    std::uint32_t Cols = 0, Rows = 0;
    const int     Asked = display_size(&Cols, &Rows);
    if (Asked != 0) return std::unexpected(Error(StageErrorKind::DimensionsUnavailable, Cause(Asked), {}, Required));
    const core::Size Size{static_cast<std::int32_t>(Cols), static_cast<std::int32_t>(Rows)};
    if (Size.Empty())
        return std::unexpected(Error(StageErrorKind::NoConsole, box::error{box::errc::device_not_ready}, Size, Required));
    if (Size.Width < LeastCols || Size.Height < LeastRows)
        return std::unexpected(Error(StageErrorKind::TooSmall, box::error{box::errc::out_of_range}, Size, Required));
    if (Cols > ITEM_GRID_SIDE_MOST || Rows > ITEM_GRID_SIDE_MOST)
        return std::unexpected(Error(StageErrorKind::FrameUnavailable, box::error{box::errc::out_of_range}, Size, Required));

    /* Over everything: as large as the screen, the display holding it to
     * the screen. */
    const float Whole = static_cast<float>(ITEM_REACH - 1) / ITEM_POINT;
    try {
        std::expected<gui::Area, box::error> Opened =
            Shown == Where::Screen ? gui::Area::Open(gui::Placement::Whole)
                                   : gui::Area::Open(gui::Placement::Over, core::PointF{Whole, Whole}, Shown == Where::Always);
        if (!Opened) return std::unexpected(Error(StageErrorKind::InvalidState, Opened.error(), Size, Required));
        CellsArea Made(std::move(*Opened), Size);
        /* A program of cells waits on its keys, never on the area's word. */
        Made.Door->Listen(false);
        Made.Row.resize(Cols);
        Made.Door->Create(CellsItem, gui::Kind::Cells, ITEM_AREA, ITEM_TOP, gui::Role::Code);
        Made.Door->Grid(CellsItem, Cols, Rows);
        return Made;
    } catch (const std::bad_alloc &) {
        return std::unexpected(Error(StageErrorKind::FrameUnavailable, box::error{box::errc::no_memory}, Size, Required));
    }
}

box::status CellsArea::Present(const Frame &Picture) noexcept
{
    if (!Door) return std::unexpected(box::error{box::errc::invalid_state});
    if (Picture.Size() != Bounds) return std::unexpected(box::error{box::errc::invalid_argument});
    try {
        for (std::int32_t Y = 0; Y < Bounds.Height; Y++) {
            for (std::int32_t X = 0; X < Bounds.Width; X++) Row[static_cast<std::size_t>(X)] = CellOf(Picture.Read({X, Y}));
            Door->Cells(CellsItem, static_cast<std::uint32_t>(Y), 0, Row);
        }
        Door->Caret(CellsItem, Picture.Caret());
        /* What the display told is taken, so its next word has room; then the
         * frame goes, all of it — a frame half shown is not a frame. */
        while (Door->Next()) {
        }
        Door->PresentAll();
    } catch (const std::bad_alloc &) {
        return std::unexpected(box::error{box::errc::no_memory});
    }
    return {};
}

box::status CellsArea::Commit(std::uint32_t ScrollRows) noexcept
{
    if (!Door) return std::unexpected(box::error{box::errc::invalid_state});
    if (ScrollRows > static_cast<std::uint32_t>(Bounds.Height)) return std::unexpected(box::error{box::errc::out_of_range});
    try {
        const box::status Done = Door->Commit(ScrollRows);
        if (!Done) return Done;
    } catch (const std::bad_alloc &) {
        return std::unexpected(box::error{box::errc::no_memory});
    }
    Door.reset();
    return {};
}

box::status CellsArea::Close() noexcept
{
    Door.reset();
    return {};
}

}  // namespace painter::tui
