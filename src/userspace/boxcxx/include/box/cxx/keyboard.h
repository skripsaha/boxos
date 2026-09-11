#ifndef BOXCXX_BOX_KEYBOARD_H
#define BOXCXX_BOX_KEYBOARD_H

#include <coroutine>
#include <cstdint>
#include <optional>

#include "box/keyboard.h"
#include "box/cxx/touch.h"

namespace box {

class key {
    char         ch_{0};
    std::uint8_t scancode_{0};
    std::uint8_t mods_{0};

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

    bool has_char() const noexcept { return ch_ != 0; }
    bool printable() const noexcept { return ch_ >= 0x20 && ch_ < 0x7F; }

    static key from_event(const kb_event_t &e) noexcept { return key(e.ascii, e.scancode, e.mods); }
    static std::optional<key> from_touch(const touch &ev) noexcept
    {
        if (std::optional<kb_event_t> e = ev.payload_as<kb_event_t>()) return from_event(*e);
        return std::nullopt;
    }
};

class key_stream {
    subscription sub_;

public:
    key_stream() noexcept : sub_(tag("keyboard")) {}
    key_stream(key_stream &&) noexcept            = default;
    key_stream &operator=(key_stream &&) noexcept = default;
    key_stream(const key_stream &)                = delete;
    key_stream &operator=(const key_stream &)     = delete;

    std::optional<key> poll() noexcept
    {
        if (std::optional<touch> ev = sub_.poll()) return key::from_touch(*ev);
        return std::nullopt;
    }
    std::optional<key> wait(std::uint32_t timeout_ms = 0) noexcept
    {
        if (std::optional<touch> ev = sub_.wait(timeout_ms)) return key::from_touch(*ev);
        return std::nullopt;
    }

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

    subscription &events() noexcept { return sub_; }
};

}

#endif