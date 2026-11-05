#include "painter/tui/stage.h"

#include <cstring>
#include <format>
#include <new>
#include <utility>
#include <vector>

#include "box/error.h"
#include "box/keyboard.h"
#include "box/print.h"
#include "box/touch.h"
#include "box/vga.h"
#include "text_cell.h"

namespace painter::tui {

struct Stage::Impl {
    core::Size            Bounds;
    Frame                 FrameBuffer;
    TouchTag              Ear = TOUCH_TAG_INVALID;
    std::vector<TextCell> NativeCells;
    core::Point            Cursor;
    bool                  Opened = true;

    Impl(core::Size BoundsValue, Frame FrameValue, TouchTag EarValue,
         std::vector<TextCell> NativeCellsValue) noexcept
        : Bounds(BoundsValue), FrameBuffer(std::move(FrameValue)), Ear(EarValue),
          NativeCells(std::move(NativeCellsValue)), Cursor{BoundsValue.Width - 1, BoundsValue.Height - 1}
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

std::string StageError::Describe(std::string_view Subject) const
{
    switch (Kind) {
    case StageErrorKind::DimensionsUnavailable:
        return std::format("This machine would not say how big its screen is: {}", Cause);
    case StageErrorKind::NoConsole:
        return std::format("This machine has no console for {}.", Subject);
    case StageErrorKind::TooSmall:
        return std::format("This screen is {} by {} rows; {} needs at least {} by {}.", Actual.Width,
                           Actual.Height, Subject, Required.Width, Required.Height);
    case StageErrorKind::FrameUnavailable:
        return std::format("There is not enough memory for {}: {}", Subject, Cause);
    case StageErrorKind::KeyboardUnavailable:
        return "This machine has no way to hear a key.";
    case StageErrorKind::InvalidState:
        return std::format("The screen for {} could not be opened.", Subject);
    }
    return std::format("The screen for {} could not be opened.", Subject);
}

std::expected<Stage, StageError> Stage::Open(std::string_view Name, std::int32_t LeastCols,
                                             std::int32_t LeastRows) noexcept
{
    (void)Name;

    if (LeastCols < 0 || LeastRows < 0)
        return std::unexpected(Error(StageErrorKind::TooSmall, box::error{box::errc::invalid_argument}, {},
                                      {LeastCols, LeastRows}));

    vga_dimensions_t Dimensions{};
    int Code = vga_getdimensions(&Dimensions);
    if (Code != 0)
        return std::unexpected(Error(StageErrorKind::DimensionsUnavailable, Cause(Code), {}, {}));

    const core::Size Actual{Dimensions.cols, Dimensions.rows};
    const core::Size Required{LeastCols, LeastRows};
    if (Actual.Empty())
        return std::unexpected(Error(StageErrorKind::NoConsole, box::error{box::errc::device_not_ready}, Actual,
                                      Required));
    if (Actual.Width < LeastCols || Actual.Height < LeastRows)
        return std::unexpected(Error(StageErrorKind::TooSmall, box::error{box::errc::out_of_range}, Actual,
                                      Required));

    box::result<Frame> Created = Frame::Create(Actual);
    if (!Created)
        return std::unexpected(Error(StageErrorKind::FrameUnavailable, Created.error(), Actual, Required));

    std::optional<std::size_t> CellCount = Actual.CellCount();
    if (!CellCount)
        return std::unexpected(Error(StageErrorKind::FrameUnavailable, box::error{box::errc::out_of_range}, Actual,
                                      Required));

    std::vector<TextCell> NativeCells;
    try {
        NativeCells.resize(*CellCount);
    } catch (const std::bad_alloc &) {
        return std::unexpected(Error(StageErrorKind::FrameUnavailable, box::error{box::errc::no_memory}, Actual,
                                      Required));
    }

    TouchTag Ear = console_listen();
    if (Ear == TOUCH_TAG_INVALID)
        return std::unexpected(Error(StageErrorKind::KeyboardUnavailable,
                                     box::error{box::errc::device_not_ready}, Actual, Required));

    try {
        auto State = std::make_unique<Impl>(Actual, std::move(*Created), Ear, std::move(NativeCells));
        return Stage(std::move(State));
    } catch (const std::bad_alloc &) {
        console_unlisten();
        return std::unexpected(Error(StageErrorKind::FrameUnavailable, box::error{box::errc::no_memory}, Actual,
                                      Required));
    }
}

Stage::Stage(std::unique_ptr<Impl> StateValue) noexcept : State(std::move(StateValue)) {}

Stage::Stage(Stage &&Other) noexcept = default;

Stage &Stage::operator=(Stage &&Other) noexcept
{
    if (this == &Other) return *this;
    (void)Close();
    State = std::move(Other.State);
    return *this;
}

Stage::~Stage()
{
    (void)Close();
}

core::Size Stage::Size() const noexcept
{
    return State ? State->Bounds : core::Size{};
}

bool Stage::IsOpen() const noexcept
{
    return State && State->Opened;
}

Frame &Stage::FrameBuffer() noexcept
{
    return State->FrameBuffer;
}

const Frame &Stage::FrameBuffer() const noexcept
{
    return State->FrameBuffer;
}

box::status Stage::SetCursor(core::Point At) noexcept
{
    if (!IsOpen()) return std::unexpected(box::error{box::errc::invalid_state});
    if (At.X < 0 || At.Y < 0 || At.X >= State->Bounds.Width || At.Y >= State->Bounds.Height)
        return std::unexpected(box::error{box::errc::out_of_range});
    State->Cursor = At;
    return {};
}

box::status Stage::Present() noexcept
{
    if (!IsOpen()) return std::unexpected(box::error{box::errc::invalid_state});

    State->FrameBuffer.WriteNative(State->NativeCells.data(), State->NativeCells.size());
    int Code = vga_setcursor(static_cast<std::uint8_t>(State->Cursor.Y),
                             static_cast<std::uint8_t>(State->Cursor.X));
    if (Code != 0) return std::unexpected(Cause(Code));

    Code = vga_paint(0, 0, static_cast<std::uint8_t>(State->Bounds.Height),
                     static_cast<std::uint8_t>(State->Bounds.Width), State->NativeCells.data());
    if (Code != 0) return std::unexpected(Cause(Code));
    return {};
}

Event Stage::Poll() noexcept
{
    if (!IsOpen()) return Event{EventKind::Gone, {}, box::error{box::errc::invalid_state}};

    Touch EventData{};
    if (!touch_try_pop_tag(State->Ear, &EventData)) return Event{EventKind::Quiet, {}, {}};
    if (EventData.payload_len < sizeof(kb_event_t))
        return Event{EventKind::Gone, {}, box::error{box::errc::corrupted}};

    kb_event_t Key{};
    std::memcpy(&Key, EventData.payload, sizeof(Key));
    return Event{EventKind::Key, KeyEvent{Key.ascii, Key.scancode, Key.mods}, {}};
}

Event Stage::Wait(std::uint32_t TimeoutMs) noexcept
{
    if (!IsOpen()) return Event{EventKind::Gone, {}, box::error{box::errc::invalid_state}};

    Touch EventData{};
    int Code = touch_await(State->Ear, &EventData, TimeoutMs);
    if (Code == -ERR_TIMEOUT) return Event{EventKind::Quiet, {}, {}};
    if (Code != 0) return Event{EventKind::Gone, {}, Cause(Code)};
    if (EventData.payload_len < sizeof(kb_event_t))
        return Event{EventKind::Gone, {}, box::error{box::errc::corrupted}};

    kb_event_t Key{};
    std::memcpy(&Key, EventData.payload, sizeof(Key));
    return Event{EventKind::Key, KeyEvent{Key.ascii, Key.scancode, Key.mods}, {}};
}

box::status Stage::Close() noexcept
{
    if (!State || !State->Opened) return {};

    State->Opened = false;
    console_unlisten();
    State->Ear = TOUCH_TAG_INVALID;

    int First = vga_clear_rgb(COLOR_LIGHT_GRAY, COLOR_BLACK);
    int Code  = vga_setcursor(0, 0);
    if (First == 0) First = Code;
    return First == 0 ? box::status{} : std::unexpected(Cause(First));
}

}  // namespace painter::tui
