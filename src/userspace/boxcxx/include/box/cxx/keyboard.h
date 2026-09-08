// boxcxx — box::keyboard  (the event-driven keyboard: box::key over the Touch stream)
//
// BoxOS delivers the keyboard as EVENTS, not a poll target: the keyboard driver
// publishes every key as a Touch event under the "keyboard" tag (the shell's
// line reader consumes it the same way). This layer is purely event-driven —
// it never polls the hardware ring.
//
//   box::key       — one key event: ch() / scancode() / shift()/ctrl()/alt() /
//                    has_char() / printable(); decoded from the Touch payload.
//   box::key_stream — a typed view over the "keyboard" Touch stream:
//                      poll()      — the next buffered key, or nullopt (a
//                                    non-blocking peek at a *delivered* event,
//                                    NOT a hardware poll);
//                      wait(ms)    — block in the kernel for the next key
//                                    (ms == 0 waits forever); nullopt on timeout;
//                      co_await next() — suspend on the current box::executor
//                                    until the next key.
//
// This is a box:: extension, not part of std. There is no poll ring anywhere
// any more: the keyboard is events, and this is how C++ hears them. A program
// under the display daemon hears the keys said on its own console lane
// (readline / getchar); box::key_stream hears the raw "keyboard" tag itself.
#ifndef BOXCXX_BOX_KEYBOARD_H
#define BOXCXX_BOX_KEYBOARD_H

#include <coroutine>
#include <cstdint>
#include <optional>

#include "box/keyboard.h"   // kb_event_t + KB_MOD_* (the keyboard Touch payload)
#include "box/cxx/touch.h"  // box::subscription / box::touch / box::tag

namespace box {

// ── box::key — one structured key event (char + scancode + modifiers) ───────
class key {
    char         ch_{0};
    std::uint8_t scancode_{0};
    std::uint8_t mods_{0};  // KB_MOD_* bitset

public:
    key() noexcept = default;
    key(char ch, std::uint8_t scancode, std::uint8_t mods) noexcept
        : ch_(ch), scancode_(scancode), mods_(mods) {}

    char         ch() const noexcept { return ch_; }
    std::uint8_t scancode() const noexcept { return scancode_; }
    std::uint8_t modifiers() const noexcept { return mods_; }

    bool shift() const noexcept { return (mods_ & KB_MOD_SHIFT) != 0; }
    bool ctrl() const noexcept { return (mods_ & KB_MOD_CTRL) != 0; }
    bool alt() const noexcept { return (mods_ & KB_MOD_ALT) != 0; }

    // Carries an ASCII char (false for a pure scancode key — e.g. an arrow).
    bool has_char() const noexcept { return ch_ != 0; }
    // Printable ASCII (space .. '~').
    bool printable() const noexcept { return ch_ >= 0x20 && ch_ < 0x7F; }

    // Decode the keyboard Touch payload {scancode, ascii, mods}.
    static key from_event(const kb_event_t &e) noexcept { return key(e.ascii, e.scancode, e.mods); }
    // Decode a "keyboard" box::touch (its payload is a kb_event_t).
    static std::optional<key> from_touch(const touch &ev) noexcept
    {
        if (std::optional<kb_event_t> e = ev.payload_as<kb_event_t>()) return from_event(*e);
        return std::nullopt;
    }
};

// ── box::key_stream — the event-driven keyboard (Touch, no polling) ─────────
// (Named key_stream, not keyboard: box::keyboard() is the cooked line channel
// in box/cxx/current.h — the "stdin" of text; this is the raw key-event stream.)
class key_stream {
    subscription sub_;

public:
    key_stream() noexcept : sub_(tag("keyboard")) {}
    key_stream(key_stream &&) noexcept            = default;
    key_stream &operator=(key_stream &&) noexcept = default;
    key_stream(const key_stream &)                = delete;
    key_stream &operator=(const key_stream &)     = delete;

    // The next buffered key, or nullopt — a non-blocking peek at a delivered
    // event (not a hardware poll).
    std::optional<key> poll() noexcept
    {
        if (std::optional<touch> ev = sub_.poll()) return key::from_touch(*ev);
        return std::nullopt;
    }
    // Block in the kernel for the next key, up to `timeout_ms` (0 == forever);
    // nullopt on timeout. Efficient — the kernel wakes us on the key event.
    std::optional<key> wait(std::uint32_t timeout_ms = 0) noexcept
    {
        if (std::optional<touch> ev = sub_.wait(timeout_ms)) return key::from_touch(*ev);
        return std::nullopt;
    }

    // co_await kbd.next() -> optional<key> — suspends on the current executor
    // until the next key (wraps the Touch subscription's awaiter, decoding).
    class key_awaiter {
        subscription::next_awaiter inner_;

    public:
        explicit key_awaiter(subscription::next_awaiter a) noexcept : inner_(a) {}
        bool await_ready() noexcept { return inner_.await_ready(); }
        bool await_suspend(std::coroutine_handle<> h) { return inner_.await_suspend(h); }
        std::optional<key> await_resume() noexcept
        {
            if (std::optional<touch> ev = inner_.await_resume()) return key::from_touch(*ev);
            return std::nullopt;
        }
    };
    key_awaiter next() noexcept { return key_awaiter{sub_.next()}; }

    // Escape hatch: the underlying Touch subscription (stream-view, ack, …).
    subscription &events() noexcept { return sub_; }
};

}  // namespace box

#endif  // BOXCXX_BOX_KEYBOARD_H
