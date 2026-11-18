#include "kept_cells.h"

#include <new>
#include <optional>
#include <utility>


namespace painter::tui {

struct KeptCells::Impl {
    CellsArea   Area;
    Frame       Cells;    /* its caret is the area's */
    core::Point Cursor;   /* where the console's caret stands */
    bool        Opened = true;

    Impl(CellsArea AreaValue, Frame CellsValue, core::Point CursorValue) noexcept
        : Area(std::move(AreaValue)), Cells(std::move(CellsValue)), Cursor(CursorValue)
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

bool Holds(core::Rect Area, core::Point At)
{
    return At.X >= Area.X && At.Y >= Area.Y && At.X - Area.X < Area.Width && At.Y - Area.Y < Area.Height;
}

/* The caret is part of the picture: what replaces a rectangle replaces the
 * caret in it, and the caret Source carries inside Region lands where its
 * cell lands. */
void CarryCaret(Frame &Out, core::Point Origin, const Frame &Source, core::Rect Region)
{
    const std::optional<core::Point> Held = Out.Caret();
    if (Held && Holds({Origin.X, Origin.Y, Region.Width, Region.Height}, *Held)) (void)Out.SetCaret(std::nullopt);

    const std::optional<core::Point> Brought = Source.Caret();
    if (Brought && Holds(Region, *Brought))
        (void)Out.SetCaret(core::Point{Origin.X + Brought->X - Region.X, Origin.Y + Brought->Y - Region.Y});
}

}  // namespace

std::expected<KeptCells, StageError> KeptCells::Open(std::int32_t LeastCols, std::int32_t LeastRows,
                                                     CellsArea::Where Shown) noexcept
{
    std::expected<CellsArea, StageError> Opened = CellsArea::Open(LeastCols, LeastRows, Shown);
    if (!Opened) return std::unexpected(Opened.error());

    // An area that goes unmade below is closed by its own destructor.
    const core::Size Actual = Opened->Size();
    const core::Size Required{LeastCols, LeastRows};
    box::result<Frame> Cells = Frame::Create(Actual);
    if (!Cells)
        return std::unexpected(Error(StageErrorKind::FrameUnavailable, Cells.error(), Actual, Required));
    Cells->ClearTransparent();

    const core::Point Cursor = Opened->Cursor();
    try {
        std::unique_ptr<Impl> StateValue(new Impl(std::move(*Opened), std::move(*Cells), Cursor));
        return KeptCells(std::move(StateValue));
    } catch (const std::bad_alloc &) {
        return std::unexpected(Error(StageErrorKind::FrameUnavailable,
                                      box::error{box::errc::no_memory}, Actual, Required));
    }
}

KeptCells::KeptCells(std::unique_ptr<Impl> StateValue) noexcept
    : State(std::move(StateValue))
{
}

KeptCells::KeptCells(KeptCells &&Other) noexcept = default;

KeptCells &KeptCells::operator=(KeptCells &&Other) noexcept
{
    if (this == &Other) return *this;
    (void)Close();
    State = std::move(Other.State);
    return *this;
}

KeptCells::~KeptCells()
{
    (void)Close();
}

core::Size KeptCells::Size() const noexcept
{
    return State ? State->Area.Size() : core::Size{};
}

GlassKind KeptCells::Glass() const noexcept
{
    return State ? State->Area.Glass() : GlassKind::Pixels;
}

std::uint8_t KeptCells::Mosaics() const noexcept
{
    return State ? State->Area.Mosaics() : 0;
}

bool KeptCells::IsOpen() const noexcept
{
    return State && State->Opened;
}

std::expected<core::Point, box::error> KeptCells::Cursor() const noexcept
{
    if (!IsOpen()) return std::unexpected(box::error{box::errc::invalid_state});
    return State->Cursor;
}

box::status KeptCells::Paint(core::Point Origin, const Frame &FrameBuffer) noexcept
{
    return Paint(Origin, FrameBuffer,
                 {0, 0, FrameBuffer.Size().Width, FrameBuffer.Size().Height});
}

box::status KeptCells::Paint(core::Point Origin, const Frame &FrameBuffer,
                               core::Rect Region) noexcept
{
    if (!IsOpen()) return std::unexpected(box::error{box::errc::invalid_state});
    const core::Size FrameSize = FrameBuffer.Size();
    if (Region.Empty() || Region.X < 0 || Region.Y < 0 ||
        Region.X > FrameSize.Width - Region.Width ||
        Region.Y > FrameSize.Height - Region.Height || Origin.X < 0 || Origin.Y < 0 ||
        Origin.X > State->Cells.Size().Width - Region.Width ||
        Origin.Y > State->Cells.Size().Height - Region.Height)
        return std::unexpected(box::error{box::errc::out_of_range});

    for (std::int32_t Row = 0; Row < Region.Height; Row++) {
        for (std::int32_t Col = 0; Col < Region.Width; Col++) {
            StoreCell(State->Cells, {Origin.X + Col, Origin.Y + Row},
                      FrameBuffer.Read({Region.X + Col, Region.Y + Row}), true);
        }
    }
    CarryCaret(State->Cells, Origin, FrameBuffer, Region);
    return State->Area.Present(State->Cells);
}

box::status KeptCells::Composite(core::Point Origin, const Frame &FrameBuffer) noexcept
{
    return Composite(Origin, FrameBuffer,
                     {0, 0, FrameBuffer.Size().Width, FrameBuffer.Size().Height});
}

box::status KeptCells::Composite(core::Point Origin, const Frame &FrameBuffer,
                                   core::Rect Region) noexcept
{
    if (!IsOpen()) return std::unexpected(box::error{box::errc::invalid_state});
    const core::Size FrameSize = FrameBuffer.Size();
    if (Region.Empty() || Region.X < 0 || Region.Y < 0 ||
        Region.X > FrameSize.Width - Region.Width ||
        Region.Y > FrameSize.Height - Region.Height || Origin.X < 0 || Origin.Y < 0 ||
        Origin.X > State->Cells.Size().Width - Region.Width ||
        Origin.Y > State->Cells.Size().Height - Region.Height)
        return std::unexpected(box::error{box::errc::out_of_range});

    for (std::int32_t Row = 0; Row < Region.Height; Row++) {
        for (std::int32_t Col = 0; Col < Region.Width; Col++) {
            StoreCell(State->Cells, {Origin.X + Col, Origin.Y + Row},
                      FrameBuffer.Read({Region.X + Col, Region.Y + Row}), false);
        }
    }
    CarryCaret(State->Cells, Origin, FrameBuffer, Region);
    return State->Area.Present(State->Cells);
}

box::status KeptCells::Clear() noexcept
{
    if (!IsOpen()) return std::unexpected(box::error{box::errc::invalid_state});
    State->Cells.ClearTransparent();
    return {};
}

box::status KeptCells::SetCursor(core::Point At) noexcept
{
    if (!IsOpen()) return std::unexpected(box::error{box::errc::invalid_state});
    if (!State->Cells.SetCaret(At)) return std::unexpected(box::error{box::errc::out_of_range});
    State->Cursor = At;
    return State->Area.Present(State->Cells);
}

box::status KeptCells::Commit(std::uint32_t ScrollRows) noexcept
{
    if (!IsOpen()) return std::unexpected(box::error{box::errc::invalid_state});
    const box::status Result = State->Area.Commit(ScrollRows);
    if (Result) State->Opened = false;
    return Result;
}

box::status KeptCells::Close() noexcept
{
    if (!State) return {};
    State->Opened = false;
    return State->Area.Close();
}

}  // namespace painter::tui
