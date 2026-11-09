#include "painter/tui/keyboard.h"

#include <cstring>
#include <memory>
#include <new>
#include <optional>

#include "box/display.h"
#include "box/error.h"
#include "box/keyboard.h"
#include "box/print.h"
#include "box/touch.h"

namespace painter::tui {

struct Keyboard::Impl {
    TouchTag Ear = TOUCH_TAG_INVALID;
    bool     Opened = true;
    bool     HearsEnd = false;   // a request to finish is the program's to answer

    explicit Impl(TouchTag EarValue) noexcept : Ear(EarValue) {}
};

namespace {

box::error Cause(int Code)
{
    return Code == 0 ? box::error{} : box::error{box_errno_of(Code)};
}

Event Decode(const Touch &Data)
{
    kb_event_t Key{};
    std::memcpy(&Key, Data.payload, sizeof(Key));
    return Event{EventKind::Key, KeyEvent{Key.ascii, Key.scancode, Key.mods}, {}};
}

// A key is three bytes (kb_event_t); the display's word that the program is
// asked to finish is a ConsoleNotice. Anything else said on the tag is no
// event.
std::optional<Event> Heard(const Touch &Data, bool HearsEnd)
{
    if (console_notice_of(Data.payload, Data.payload_len) == CONSOLE_NOTICE_END)
        return Event{HearsEnd ? EventKind::EndAsked : EventKind::Gone, {}, {}};
    if (Data.payload_len == sizeof(kb_event_t)) return Decode(Data);
    return std::nullopt;
}

}  // namespace

std::expected<Keyboard, StageError> Keyboard::Open() noexcept
{
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
    while (touch_try_pop_tag(State->Ear, &Data))
        if (std::optional<Event> Said = Heard(Data, State->HearsEnd)) return *Said;
    return Event{EventKind::Quiet, {}, {}};
}

Event Keyboard::Wait(std::uint32_t TimeoutMs) noexcept
{
    if (!IsOpen()) return Event{EventKind::Gone, {}, box::error{box::errc::invalid_state}};
    for (;;) {
        Touch Data{};
        int Code = touch_await(State->Ear, &Data, TimeoutMs);
        if (Code == -ERR_TIMEOUT) return Event{EventKind::Quiet, {}, {}};
        if (Code != 0) return Event{EventKind::Gone, {}, Cause(Code)};
        if (std::optional<Event> Said = Heard(Data, State->HearsEnd)) return *Said;
        // Not a key, so nothing for the caller: a bounded wait says so, and
        // one that waits until a key waits on.
        if (TimeoutMs != 0) return Event{EventKind::Quiet, {}, {}};
    }
}

void Keyboard::HearEnd() noexcept
{
    if (State) State->HearsEnd = true;
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
