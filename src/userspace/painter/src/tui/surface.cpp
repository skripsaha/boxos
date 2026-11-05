#include "painter/tui/surface.h"

#include <new>
#include <optional>
#include <utility>
#include <vector>

#include "box/error.h"
#include "box/vga.h"
#include "text_cell.h"

namespace painter::tui {

struct Surface::Impl {
    core::Size            Bounds;
    std::vector<TextCell> NativeCells;
    bool                  Opened = true;

    Impl(core::Size BoundsValue, std::vector<TextCell> NativeCellsValue) noexcept
        : Bounds(BoundsValue), NativeCells(std::move(NativeCellsValue))
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

    try {
        return Surface(std::make_unique<Impl>(Actual, std::vector<TextCell>{}));
    } catch (const std::bad_alloc &) {
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
    vga_pos_t Cursor{};
    const int Code = vga_getcursor(&Cursor);
    if (Code != 0) return std::unexpected(Cause(Code));
    return core::Point{Cursor.col, Cursor.row};
}

box::status Surface::Paint(core::Point Origin, const Frame &FrameBuffer) noexcept
{
    if (!IsOpen()) return std::unexpected(box::error{box::errc::invalid_state});
    const core::Size Region = FrameBuffer.Size();
    if (Origin.X < 0 || Origin.Y < 0 || Region.Empty() || Origin.X > State->Bounds.Width - Region.Width ||
        Origin.Y > State->Bounds.Height - Region.Height)
        return std::unexpected(box::error{box::errc::out_of_range});

    const std::optional<std::size_t> CellCount = Region.CellCount();
    if (!CellCount)
        return std::unexpected(box::error{box::errc::invalid_argument});
    try {
        State->NativeCells.resize(*CellCount);
    } catch (const std::bad_alloc &) {
        return std::unexpected(box::error{box::errc::no_memory});
    }
    FrameBuffer.WriteNative(State->NativeCells.data(), State->NativeCells.size());
    const int Code = vga_paint(static_cast<std::uint8_t>(Origin.Y), static_cast<std::uint8_t>(Origin.X),
                               static_cast<std::uint8_t>(Region.Height), static_cast<std::uint8_t>(Region.Width),
                               State->NativeCells.data());
    return Code == 0 ? box::status{} : std::unexpected(Cause(Code));
}

box::status Surface::SetCursor(core::Point At) noexcept
{
    if (!IsOpen()) return std::unexpected(box::error{box::errc::invalid_state});
    if (At.X < 0 || At.Y < 0 || At.X >= State->Bounds.Width || At.Y >= State->Bounds.Height)
        return std::unexpected(box::error{box::errc::out_of_range});
    const int Code = vga_setcursor(static_cast<std::uint8_t>(At.Y), static_cast<std::uint8_t>(At.X));
    return Code == 0 ? box::status{} : std::unexpected(Cause(Code));
}

box::status Surface::ScrollUp() noexcept
{
    if (!IsOpen()) return std::unexpected(box::error{box::errc::invalid_state});
    const int Code = vga_scroll_up();
    return Code == 0 ? box::status{} : std::unexpected(Cause(Code));
}

box::status Surface::Close() noexcept
{
    if (!State) return {};
    State->Opened = false;
    return {};
}

}  // namespace painter::tui
