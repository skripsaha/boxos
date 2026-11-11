#include "painter/tui/console_surface.h"

#include <new>
#include <utility>

#include "layer_canvas.h"

namespace painter::tui {

struct ConsoleSurface::Impl {
    LayerCanvas Canvas;
    std::uint32_t ScrollRows = 0;

    explicit Impl(LayerCanvas Value) noexcept : Canvas(std::move(Value)) {}
};

std::expected<ConsoleSurface, StageError> ConsoleSurface::Open(std::int32_t LeastCols,
                                                                std::int32_t LeastRows) noexcept
{
    std::expected<LayerCanvas, StageError> Opened =
        LayerCanvas::Open(LeastCols, LeastRows, DISPLAY_LAYER_OVERLAY);
    if (!Opened) return std::unexpected(Opened.error());
    try {
        return ConsoleSurface(std::unique_ptr<Impl>(new Impl(std::move(*Opened))));
    } catch (const std::bad_alloc &) {
        return std::unexpected(StageError{StageErrorKind::FrameUnavailable,
                                          box::error{box::errc::no_memory},
                                          {LeastCols, LeastRows},
                                          {LeastCols, LeastRows}});
    }
}

ConsoleSurface::ConsoleSurface(std::unique_ptr<Impl> StateValue) noexcept
    : State(std::move(StateValue))
{
}

ConsoleSurface::ConsoleSurface(ConsoleSurface &&Other) noexcept = default;

ConsoleSurface &ConsoleSurface::operator=(ConsoleSurface &&Other) noexcept
{
    if (this == &Other) return *this;
    (void)Close();
    State = std::move(Other.State);
    return *this;
}

ConsoleSurface::~ConsoleSurface()
{
    (void)Close();
}

core::Size ConsoleSurface::Size() const noexcept
{
    return State ? State->Canvas.Size() : core::Size{};
}

GlassKind ConsoleSurface::Glass() const noexcept
{
    return State ? State->Canvas.Glass() : GlassKind::Pixels;
}

bool ConsoleSurface::IsOpen() const noexcept
{
    return State && State->Canvas.IsOpen();
}

std::expected<core::Point, box::error> ConsoleSurface::Cursor() const noexcept
{
    if (!State) return std::unexpected(box::error{box::errc::invalid_state});
    std::expected<core::Point, box::error> Position = State->Canvas.Cursor();
    if (!Position) return std::unexpected(Position.error());
    Position->Y = Position->Y > static_cast<std::int32_t>(State->ScrollRows)
                      ? Position->Y - static_cast<std::int32_t>(State->ScrollRows)
                      : 0;
    return Position;
}

box::status ConsoleSurface::Paint(core::Point Origin, const Frame &FrameBuffer) noexcept
{
    if (!State) return std::unexpected(box::error{box::errc::invalid_state});
    return State->Canvas.Paint(Origin, FrameBuffer);
}

box::status ConsoleSurface::Paint(core::Point Origin, const Frame &FrameBuffer,
                                  core::Rect Region) noexcept
{
    if (!State) return std::unexpected(box::error{box::errc::invalid_state});
    return State->Canvas.Paint(Origin, FrameBuffer, Region);
}

box::status ConsoleSurface::SetCursor(core::Point At) noexcept
{
    if (!State) return std::unexpected(box::error{box::errc::invalid_state});
    return State->Canvas.SetCursor(At);
}

box::status ConsoleSurface::ScrollUp() noexcept
{
    if (!State) return std::unexpected(box::error{box::errc::invalid_state});
    if (!State->Canvas.IsOpen()) return std::unexpected(box::error{box::errc::invalid_state});
    if (State->ScrollRows == static_cast<std::uint32_t>(State->Canvas.Size().Height))
        return std::unexpected(box::error{box::errc::out_of_range});
    State->ScrollRows++;
    return {};
}

box::status ConsoleSurface::Commit() noexcept
{
    if (!State) return std::unexpected(box::error{box::errc::invalid_state});
    return State->Canvas.Commit(State->ScrollRows);
}

box::status ConsoleSurface::Close() noexcept
{
    if (!State) return {};
    return State->Canvas.Close();
}

}  // namespace painter::tui
