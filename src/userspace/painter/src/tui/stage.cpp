#include "painter/tui/stage.h"

#include <format>
#include <new>
#include <utility>

#include "painter/tui/keyboard.h"

#include "cells_area.h"

namespace painter::tui {

struct Stage::Impl {
    Frame     FrameBuffer;
    CellsArea Area;
    Keyboard  Ear;
    bool      Opened = true;

    Impl(Frame FrameValue, CellsArea AreaValue, Keyboard EarValue) noexcept
        : FrameBuffer(std::move(FrameValue)), Area(std::move(AreaValue)), Ear(std::move(EarValue))
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
    std::expected<CellsArea, StageError> Area = CellsArea::Open(LeastCols, LeastRows, CellsArea::Where::Screen);
    if (!Area) return std::unexpected(Area.error());

    // What goes unmade below is given back by its own destructor.
    const core::Size Actual = Area->Size();
    const core::Size Required{LeastCols, LeastRows};
    box::result<Frame> Created = Frame::Create(Actual);
    if (!Created)
        return std::unexpected(StageError{StageErrorKind::FrameUnavailable, Created.error(), Actual, Required});
    Created->SetGlass(Area->Glass());
    Created->SetMosaics(Area->Mosaics());

    std::expected<Keyboard, StageError> Ear = Keyboard::Open();
    if (!Ear) return std::unexpected(Ear.error());

    try {
        return Stage(std::make_unique<Impl>(std::move(*Created), std::move(*Area), std::move(*Ear)));
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
    return State ? State->Area.Size() : core::Size{};
}

GlassKind Stage::Glass() const noexcept
{
    return State ? State->Area.Glass() : GlassKind::Pixels;
}

std::uint8_t Stage::Mosaics() const noexcept
{
    return State ? State->Area.Mosaics() : 0;
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
    return State->Area.Present(State->FrameBuffer);
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
    return State->Area.Close();
}

}  // namespace painter::tui
