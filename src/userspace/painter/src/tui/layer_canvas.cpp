#include "layer_canvas.h"

#include <new>
#include <utility>

#include "display_client.h"

namespace painter::tui {

struct LayerCanvas::Impl {
    DisplayLayer Layer;
    Frame        LayerCells;
    core::Point  Cursor;
    bool         CursorVisible = false;
    bool         Opened = true;

    Impl(DisplayLayer LayerValue, Frame LayerCellsValue, core::Point CursorValue) noexcept
        : Layer(std::move(LayerValue)), LayerCells(std::move(LayerCellsValue)), Cursor(CursorValue)
    {
    }
};

namespace {

StageError Error(StageErrorKind Kind, box::error CauseValue, core::Size Actual,
                 core::Size Required)
{
    return StageError{Kind, CauseValue, Actual, Required};
}

void StoreCell(Frame &Out, core::Point At, Cell Value, bool ForceOpaque)
{
    if (ForceOpaque) Value.VisibilityMode = Visibility::Opaque;
    (void)Out.Put(At, Value);
}

}  // namespace

std::expected<LayerCanvas, StageError> LayerCanvas::Open(std::int32_t LeastCols,
                                                          std::int32_t LeastRows,
                                                          DisplayLayerRole Role) noexcept
{
    std::expected<DisplayLayer, StageError> Opened = DisplayLayer::Open(LeastCols, LeastRows, Role);
    if (!Opened) return std::unexpected(Opened.error());

    // A layer that goes unmade below is closed by its own destructor.
    const core::Size Actual = Opened->Size();
    const core::Size Required{LeastCols, LeastRows};
    box::result<Frame> Cells = Frame::Create(Actual);
    if (!Cells)
        return std::unexpected(Error(StageErrorKind::FrameUnavailable, Cells.error(), Actual, Required));
    Cells->ClearTransparent();

    const core::Point Cursor = Opened->Cursor();
    try {
        std::unique_ptr<Impl> StateValue(new Impl(std::move(*Opened), std::move(*Cells), Cursor));
        return LayerCanvas(std::move(StateValue));
    } catch (const std::bad_alloc &) {
        return std::unexpected(Error(StageErrorKind::FrameUnavailable,
                                      box::error{box::errc::no_memory}, Actual, Required));
    }
}

LayerCanvas::LayerCanvas(std::unique_ptr<Impl> StateValue) noexcept
    : State(std::move(StateValue))
{
}

LayerCanvas::LayerCanvas(LayerCanvas &&Other) noexcept = default;

LayerCanvas &LayerCanvas::operator=(LayerCanvas &&Other) noexcept
{
    if (this == &Other) return *this;
    (void)Close();
    State = std::move(Other.State);
    return *this;
}

LayerCanvas::~LayerCanvas()
{
    (void)Close();
}

core::Size LayerCanvas::Size() const noexcept
{
    return State ? State->Layer.Size() : core::Size{};
}

bool LayerCanvas::IsOpen() const noexcept
{
    return State && State->Opened;
}

std::expected<core::Point, box::error> LayerCanvas::Cursor() const noexcept
{
    if (!IsOpen()) return std::unexpected(box::error{box::errc::invalid_state});
    return State->Cursor;
}

box::status LayerCanvas::Paint(core::Point Origin, const Frame &FrameBuffer) noexcept
{
    return Paint(Origin, FrameBuffer,
                 {0, 0, FrameBuffer.Size().Width, FrameBuffer.Size().Height});
}

box::status LayerCanvas::Paint(core::Point Origin, const Frame &FrameBuffer,
                               core::Rect Region) noexcept
{
    if (!IsOpen()) return std::unexpected(box::error{box::errc::invalid_state});
    const core::Size FrameSize = FrameBuffer.Size();
    if (Region.Empty() || Region.X < 0 || Region.Y < 0 ||
        Region.X > FrameSize.Width - Region.Width ||
        Region.Y > FrameSize.Height - Region.Height || Origin.X < 0 || Origin.Y < 0 ||
        Origin.X > State->LayerCells.Size().Width - Region.Width ||
        Origin.Y > State->LayerCells.Size().Height - Region.Height)
        return std::unexpected(box::error{box::errc::out_of_range});

    for (std::int32_t Row = 0; Row < Region.Height; Row++) {
        for (std::int32_t Col = 0; Col < Region.Width; Col++) {
            StoreCell(State->LayerCells, {Origin.X + Col, Origin.Y + Row},
                      FrameBuffer.Read({Region.X + Col, Region.Y + Row}), true);
        }
    }
    return State->Layer.Present(State->LayerCells, State->Cursor, State->CursorVisible);
}

box::status LayerCanvas::Composite(core::Point Origin, const Frame &FrameBuffer) noexcept
{
    return Composite(Origin, FrameBuffer,
                     {0, 0, FrameBuffer.Size().Width, FrameBuffer.Size().Height});
}

box::status LayerCanvas::Composite(core::Point Origin, const Frame &FrameBuffer,
                                   core::Rect Region) noexcept
{
    if (!IsOpen()) return std::unexpected(box::error{box::errc::invalid_state});
    const core::Size FrameSize = FrameBuffer.Size();
    if (Region.Empty() || Region.X < 0 || Region.Y < 0 ||
        Region.X > FrameSize.Width - Region.Width ||
        Region.Y > FrameSize.Height - Region.Height || Origin.X < 0 || Origin.Y < 0 ||
        Origin.X > State->LayerCells.Size().Width - Region.Width ||
        Origin.Y > State->LayerCells.Size().Height - Region.Height)
        return std::unexpected(box::error{box::errc::out_of_range});

    for (std::int32_t Row = 0; Row < Region.Height; Row++) {
        for (std::int32_t Col = 0; Col < Region.Width; Col++) {
            StoreCell(State->LayerCells, {Origin.X + Col, Origin.Y + Row},
                      FrameBuffer.Read({Region.X + Col, Region.Y + Row}), false);
        }
    }
    return State->Layer.Present(State->LayerCells, State->Cursor, State->CursorVisible);
}

box::status LayerCanvas::Clear() noexcept
{
    if (!IsOpen()) return std::unexpected(box::error{box::errc::invalid_state});
    State->LayerCells.ClearTransparent();
    return {};
}

box::status LayerCanvas::SetCursor(core::Point At) noexcept
{
    if (!IsOpen()) return std::unexpected(box::error{box::errc::invalid_state});
    if (At.X < 0 || At.Y < 0 || At.X >= State->LayerCells.Size().Width ||
        At.Y >= State->LayerCells.Size().Height)
        return std::unexpected(box::error{box::errc::out_of_range});
    State->Cursor = At;
    State->CursorVisible = true;
    return State->Layer.Present(State->LayerCells, State->Cursor, State->CursorVisible);
}

box::status LayerCanvas::Commit(std::uint32_t ScrollRows) noexcept
{
    if (!IsOpen()) return std::unexpected(box::error{box::errc::invalid_state});
    const box::status Result = State->Layer.Commit(ScrollRows);
    if (Result) State->Opened = false;
    return Result;
}

box::status LayerCanvas::Close() noexcept
{
    if (!State) return {};
    State->Opened = false;
    return State->Layer.Close();
}

}  // namespace painter::tui
