#include "painter/tui/surface.h"

#include <new>
#include <utility>

#include "kept_cells.h"

namespace painter::tui {

struct Surface::Impl {
    KeptCells Canvas;

    explicit Impl(KeptCells Value) noexcept : Canvas(std::move(Value)) {}
};

std::expected<Surface, StageError> Surface::Open(std::int32_t LeastCols,
                                                 std::int32_t LeastRows,
                                                 SurfaceRole Role) noexcept
{
    const CellsArea::Where Shown = Role == SurfaceRole::Notice ? CellsArea::Where::Always : CellsArea::Where::Over;
    std::expected<KeptCells, StageError> Opened = KeptCells::Open(LeastCols, LeastRows, Shown);
    if (!Opened) return std::unexpected(Opened.error());
    try {
        return Surface(std::unique_ptr<Impl>(new Impl(std::move(*Opened))));
    } catch (const std::bad_alloc &) {
        return std::unexpected(StageError{StageErrorKind::FrameUnavailable,
                                          box::error{box::errc::no_memory},
                                          {LeastCols, LeastRows},
                                          {LeastCols, LeastRows}});
    }
}

Surface::Surface(std::unique_ptr<Impl> StateValue) noexcept
    : State(std::move(StateValue))
{
}

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
    return State ? State->Canvas.Size() : core::Size{};
}

GlassKind Surface::Glass() const noexcept
{
    return State ? State->Canvas.Glass() : GlassKind::Pixels;
}

std::uint8_t Surface::Mosaics() const noexcept
{
    return State ? State->Canvas.Mosaics() : 0;
}

bool Surface::IsOpen() const noexcept
{
    return State && State->Canvas.IsOpen();
}

box::status Surface::Composite(core::Point Origin, const Frame &FrameBuffer) noexcept
{
    if (!State) return std::unexpected(box::error{box::errc::invalid_state});
    return State->Canvas.Composite(Origin, FrameBuffer);
}

box::status Surface::Composite(core::Point Origin, const Frame &FrameBuffer,
                                core::Rect Region) noexcept
{
    if (!State) return std::unexpected(box::error{box::errc::invalid_state});
    return State->Canvas.Composite(Origin, FrameBuffer, Region);
}

box::status Surface::Clear() noexcept
{
    if (!State) return std::unexpected(box::error{box::errc::invalid_state});
    return State->Canvas.Clear();
}

box::status Surface::Close() noexcept
{
    if (!State) return {};
    return State->Canvas.Close();
}

}  // namespace painter::tui
