#include "painter/tui/surface.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <optional>
#include <utility>
#include <vector>

#include "box/error.h"
#include "box/vga.h"
#include "text_cell.h"

#include "display_client.h"

namespace painter::tui {

struct Surface::Impl {
    core::Size            Bounds;
    DisplayLayer          Layer;
    std::vector<DisplayLayerCell> LayerCells;
    core::Point            Cursor;
    bool                  CursorVisible = false;
    bool                  Opened = true;

    Impl(core::Size BoundsValue, DisplayLayer LayerValue,
         std::vector<DisplayLayerCell> LayerCellsValue, core::Point CursorValue) noexcept
        : Bounds(BoundsValue), Layer(std::move(LayerValue)),
          LayerCells(std::move(LayerCellsValue)), Cursor(CursorValue)
    {
    }
};

namespace {

box::error Cause(int Code)
{
    return Code == 0 ? box::error{} : box::error{box_errno_of(Code)};
}

StageError Error(StageErrorKind Kind, box::error CauseValue, core::Size Actual, core::Size Required)
{
    return StageError{Kind, CauseValue, Actual, Required};
}

}  // namespace

std::expected<Surface, StageError> Surface::Open(std::string_view Name, std::int32_t LeastCols,
                                                  std::int32_t LeastRows) noexcept
{
    (void)Name;
    if (LeastCols < 0 || LeastRows < 0)
        return std::unexpected(Error(StageErrorKind::TooSmall, box::error{box::errc::invalid_argument}, {},
                                      {LeastCols, LeastRows}));

    vga_dimensions_t Dimensions{};
    int Code = vga_getdimensions(&Dimensions);
    if (Code != 0)
        return std::unexpected(Error(StageErrorKind::DimensionsUnavailable,
                                     Cause(Code), {}, {LeastCols, LeastRows}));

    const core::Size Actual{Dimensions.cols, Dimensions.rows};
    const core::Size Required{LeastCols, LeastRows};
    if (Actual.Empty())
        return std::unexpected(Error(StageErrorKind::NoConsole, box::error{box::errc::device_not_ready}, Actual,
                                      Required));
    if (Actual.Width < LeastCols || Actual.Height < LeastRows)
        return std::unexpected(Error(StageErrorKind::TooSmall, box::error{box::errc::out_of_range}, Actual,
                                      Required));

    const std::optional<std::size_t> TotalCells = Actual.CellCount();
    if (!TotalCells)
        return std::unexpected(Error(StageErrorKind::FrameUnavailable,
                                     box::error{box::errc::out_of_range}, Actual, Required));

    std::vector<DisplayLayerCell> LayerCells;
    try {
        LayerCells.resize(*TotalCells);
        for (DisplayLayerCell &Cell : LayerCells) {
            Cell = DisplayLayerCell{};
            Cell.Opaque = 0;
        }
    } catch (const std::bad_alloc &) {
        return std::unexpected(Error(StageErrorKind::FrameUnavailable, box::error{box::errc::no_memory}, Actual,
                                      Required));
    }

    std::expected<DisplayLayer, box::error> LayerOpened =
        DisplayLayer::Open(Actual, DISPLAY_LAYER_OVERLAY);
    if (!LayerOpened)
        return std::unexpected(Error(StageErrorKind::InvalidState, LayerOpened.error(), Actual, Required));

    vga_pos_t Position{};
    const int CursorCode = vga_getcursor(&Position);
    if (CursorCode != 0) {
        (void)LayerOpened->Close();
        return std::unexpected(Error(StageErrorKind::DimensionsUnavailable, Cause(CursorCode), Actual, Required));
    }

    DisplayLayer LayerValue = std::move(*LayerOpened);
    try {
        std::unique_ptr<Impl> StateValue(new Impl(Actual, std::move(LayerValue),
                                                  std::move(LayerCells),
                                                  core::Point{Position.col, Position.row}));
        return Surface(std::move(StateValue));
    } catch (const std::bad_alloc &) {
        (void)LayerValue.Close();
        return std::unexpected(Error(StageErrorKind::FrameUnavailable, box::error{box::errc::no_memory}, Actual,
                                      Required));
    }
}

Surface::Surface(std::unique_ptr<Impl> StateValue) noexcept : State(std::move(StateValue)) {}

Surface::Surface(Surface &&Other) noexcept = default;

Surface &Surface::operator=(Surface &&Other) noexcept
{
    if (this == &Other) return *this;
    (void)Close();
    State = std::move(Other.State);
    return *this;
}

Surface::~Surface()
{
    (void)Close();
}

core::Size Surface::Size() const noexcept
{
    return State ? State->Bounds : core::Size{};
}

bool Surface::IsOpen() const noexcept
{
    return State && State->Opened;
}

std::expected<core::Point, box::error> Surface::Cursor() const noexcept
{
    if (!IsOpen()) return std::unexpected(box::error{box::errc::invalid_state});
    return State->Cursor;
}

namespace {

void StoreCell(DisplayLayerCell &Out, const Cell &Value, bool ForceOpaque)
{
    Out.Cell = TextCell{Value.Foreground.Raw(), Value.Background.Raw(), Value.Glyph, {0, 0, 0}};
    Out.Opaque = ForceOpaque || !Value.IsTransparent() ? 1u : 0u;
    Out.Reserved[0] = Out.Reserved[1] = Out.Reserved[2] = 0;
}

}  // namespace

box::status Surface::Paint(core::Point Origin, const Frame &FrameBuffer) noexcept
{
    return Paint(Origin, FrameBuffer, {0, 0, FrameBuffer.Size().Width, FrameBuffer.Size().Height});
}

box::status Surface::Paint(core::Point Origin, const Frame &FrameBuffer, core::Rect Region) noexcept
{
    if (!IsOpen()) return std::unexpected(box::error{box::errc::invalid_state});
    const core::Size FrameSize = FrameBuffer.Size();
    if (Region.Empty() || Region.X < 0 || Region.Y < 0 || Region.X > FrameSize.Width - Region.Width ||
        Region.Y > FrameSize.Height - Region.Height || Origin.X < 0 || Origin.Y < 0 ||
        Origin.X > State->Bounds.Width - Region.Width || Origin.Y > State->Bounds.Height - Region.Height)
        return std::unexpected(box::error{box::errc::out_of_range});

    const std::optional<std::size_t> RegionCells = core::Size{Region.Width, Region.Height}.CellCount();
    if (!RegionCells)
        return std::unexpected(box::error{box::errc::invalid_argument});
    for (std::int32_t Row = 0; Row < Region.Height; Row++) {
        for (std::int32_t Col = 0; Col < Region.Width; Col++) {
            const Cell Value = FrameBuffer.Read({Region.X + Col, Region.Y + Row});
            DisplayLayerCell &Out = State->LayerCells[
                static_cast<std::size_t>(Origin.Y + Row) * State->Bounds.Width + Origin.X + Col];
            StoreCell(Out, Value, true);
        }
    }
    return State->Layer.Present(State->LayerCells, State->Cursor, State->CursorVisible);
}

box::status Surface::Composite(core::Point Origin, const Frame &FrameBuffer) noexcept
{
    return Composite(Origin, FrameBuffer, {0, 0, FrameBuffer.Size().Width, FrameBuffer.Size().Height});
}

box::status Surface::Composite(core::Point Origin, const Frame &FrameBuffer, core::Rect Region) noexcept
{
    if (!IsOpen()) return std::unexpected(box::error{box::errc::invalid_state});
    const core::Size FrameSize = FrameBuffer.Size();
    if (Region.Empty() || Region.X < 0 || Region.Y < 0 || Region.X > FrameSize.Width - Region.Width ||
        Region.Y > FrameSize.Height - Region.Height || Origin.X < 0 || Origin.Y < 0 ||
        Origin.X > State->Bounds.Width - Region.Width || Origin.Y > State->Bounds.Height - Region.Height)
        return std::unexpected(box::error{box::errc::out_of_range});

    for (std::int32_t Row = 0; Row < Region.Height; Row++) {
        for (std::int32_t Col = 0; Col < Region.Width; Col++) {
            const Cell Value = FrameBuffer.Read({Region.X + Col, Region.Y + Row});
            DisplayLayerCell &Out = State->LayerCells[
                static_cast<std::size_t>(Origin.Y + Row) * State->Bounds.Width + Origin.X + Col];
            StoreCell(Out, Value, false);
        }
    }
    return State->Layer.Present(State->LayerCells, State->Cursor, State->CursorVisible);
}

box::status Surface::Clear() noexcept
{
    if (!IsOpen()) return std::unexpected(box::error{box::errc::invalid_state});
    for (DisplayLayerCell &Cell : State->LayerCells) {
        Cell = DisplayLayerCell{};
    }
    return {};
}

box::status Surface::SetCursor(core::Point At) noexcept
{
    if (!IsOpen()) return std::unexpected(box::error{box::errc::invalid_state});
    if (At.X < 0 || At.Y < 0 || At.X >= State->Bounds.Width || At.Y >= State->Bounds.Height)
        return std::unexpected(box::error{box::errc::out_of_range});
    State->Cursor = At;
    State->CursorVisible = true;
    return State->Layer.Present(State->LayerCells, State->Cursor, State->CursorVisible);
}

box::status Surface::ScrollUp() noexcept
{
    if (!IsOpen()) return std::unexpected(box::error{box::errc::invalid_state});
    for (std::int32_t Row = 1; Row < State->Bounds.Height; Row++) {
        std::memmove(&State->LayerCells[static_cast<std::size_t>(Row - 1) * State->Bounds.Width],
                     &State->LayerCells[static_cast<std::size_t>(Row) * State->Bounds.Width],
                     static_cast<std::size_t>(State->Bounds.Width) * sizeof(DisplayLayerCell));
    }
    std::fill(State->LayerCells.end() - State->Bounds.Width, State->LayerCells.end(), DisplayLayerCell{});
    return State->Layer.Present(State->LayerCells, State->Cursor, State->CursorVisible);
}

box::status Surface::Close() noexcept
{
    if (!State) return {};
    State->Opened = false;
    return State->Layer.Close();
}

}  // namespace painter::tui
