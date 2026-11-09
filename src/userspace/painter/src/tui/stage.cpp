#include "painter/tui/stage.h"

#include <format>
#include <new>
#include <utility>

#include "painter/tui/keyboard.h"

#include "display_client.h"

namespace painter::tui {

struct Stage::Impl {
    Frame        FrameBuffer;
    DisplayLayer Layer;
    Keyboard     Ear;
    bool         Opened = true;

    Impl(Frame FrameValue, DisplayLayer LayerValue, Keyboard EarValue) noexcept
        : FrameBuffer(std::move(FrameValue)), Layer(std::move(LayerValue)), Ear(std::move(EarValue))
    {
    }
};

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

std::expected<Stage, StageError> Stage::Open(std::int32_t LeastCols, std::int32_t LeastRows) noexcept
{
    std::expected<DisplayLayer, StageError> Layer = DisplayLayer::Open(LeastCols, LeastRows, DISPLAY_LAYER_SCENE);
    if (!Layer) return std::unexpected(Layer.error());

    // What goes unmade below is given back by its own destructor.
    const core::Size Actual = Layer->Size();
    const core::Size Required{LeastCols, LeastRows};
    box::result<Frame> Created = Frame::Create(Actual);
    if (!Created)
        return std::unexpected(StageError{StageErrorKind::FrameUnavailable, Created.error(), Actual, Required});

    std::expected<Keyboard, StageError> Ear = Keyboard::Open();
    if (!Ear) return std::unexpected(Ear.error());

    try {
        return Stage(std::make_unique<Impl>(std::move(*Created), std::move(*Layer), std::move(*Ear)));
    } catch (const std::bad_alloc &) {
        return std::unexpected(StageError{StageErrorKind::FrameUnavailable, box::error{box::errc::no_memory},
                                          Actual, Required});
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
    return State ? State->Layer.Size() : core::Size{};
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

box::status Stage::Present() noexcept
{
    if (!IsOpen()) return std::unexpected(box::error{box::errc::invalid_state});
    return State->Layer.Present(State->FrameBuffer);
}

Event Stage::Poll() noexcept
{
    if (!IsOpen()) return Event{EventKind::Gone, {}, box::error{box::errc::invalid_state}};
    return State->Ear.Poll();
}

Event Stage::Wait(std::uint32_t TimeoutMs) noexcept
{
    if (!IsOpen()) return Event{EventKind::Gone, {}, box::error{box::errc::invalid_state}};
    return State->Ear.Wait(TimeoutMs);
}

void Stage::HearEnd() noexcept
{
    if (State) State->Ear.HearEnd();
}

box::status Stage::Close() noexcept
{
    if (!State || !State->Opened) return {};

    State->Opened = false;
    (void)State->Ear.Close();
    return State->Layer.Close();
}

}  // namespace painter::tui
