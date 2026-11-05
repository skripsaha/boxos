#include "painter/tui/keyboard.h"

#include <cstring>
#include <memory>
#include <new>

#include "box/error.h"
#include "box/keyboard.h"
#include "box/print.h"
#include "box/touch.h"

namespace painter::tui {

struct Keyboard::Impl {
    TouchTag Ear = TOUCH_TAG_INVALID;
    bool     Opened = true;

    explicit Impl(TouchTag EarValue) noexcept : Ear(EarValue) {}
};

namespace {

box::error Cause(int Code)
{
    return Code == 0 ? box::error{} : box::error{box_errno_of(Code)};
}

Event Decode(Touch &Data)
{
    if (Data.payload_len < sizeof(kb_event_t))
        return Event{EventKind::Gone, {}, box::error{box::errc::corrupted}};
    kb_event_t Key{};
    std::memcpy(&Key, Data.payload, sizeof(Key));
    return Event{EventKind::Key, KeyEvent{Key.ascii, Key.scancode, Key.mods}, {}};
}

}  // namespace

std::expected<Keyboard, StageError> Keyboard::Open(std::string_view Name) noexcept
{
    (void)Name;
    TouchTag Ear = console_listen();
    if (Ear == TOUCH_TAG_INVALID)
        return std::unexpected(StageError{StageErrorKind::KeyboardUnavailable,
                                          box::error{box::errc::device_not_ready}, {}, {}});
    try {
        return Keyboard(std::make_unique<Impl>(Ear));
    } catch (const std::bad_alloc &) {
        console_unlisten();
        return std::unexpected(StageError{StageErrorKind::FrameUnavailable,
                                          box::error{box::errc::no_memory}, {}, {}});
    }
}

Keyboard::Keyboard(std::unique_ptr<Impl> StateValue) noexcept : State(std::move(StateValue)) {}
Keyboard::Keyboard(Keyboard &&) noexcept = default;

Keyboard &Keyboard::operator=(Keyboard &&Other) noexcept
{
    if (this == &Other) return *this;
    (void)Close();
    State = std::move(Other.State);
    return *this;
}

Keyboard::~Keyboard()
{
    (void)Close();
}

bool Keyboard::IsOpen() const noexcept
{
    return State && State->Opened;
}

Event Keyboard::Poll() noexcept
{
    if (!IsOpen()) return Event{EventKind::Gone, {}, box::error{box::errc::invalid_state}};
    Touch Data{};
    return touch_try_pop_tag(State->Ear, &Data) ? Decode(Data) : Event{EventKind::Quiet, {}, {}};
}

Event Keyboard::Wait(std::uint32_t TimeoutMs) noexcept
{
    if (!IsOpen()) return Event{EventKind::Gone, {}, box::error{box::errc::invalid_state}};
    Touch Data{};
    int Code = touch_await(State->Ear, &Data, TimeoutMs);
    if (Code == -ERR_TIMEOUT) return Event{EventKind::Quiet, {}, {}};
    if (Code != 0) return Event{EventKind::Gone, {}, Cause(Code)};
    return Decode(Data);
}

box::status Keyboard::Close() noexcept
{
    if (!State || !State->Opened) return {};
    State->Opened = false;
    console_unlisten();
    State->Ear = TOUCH_TAG_INVALID;
    return {};
}

}  // namespace painter::tui
