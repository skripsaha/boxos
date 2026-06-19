// boxcxx — box::keyboard  (structured key events: box::key + box::key_input + async)
//
// The idiomatic C++ surface over the boxlib structured keyboard API
// (box/keyboard.h). This is the RAW / structured layer — individual key events
// carrying char + scancode + modifiers — distinct from box::current<byte>'s
// "keyboard" channel, which delivers cooked text lines (the "stdin" use case).
// Same device, two layers for two needs.
//
//   box::key         — one key event: ch() / scancode() / shift()/ctrl()/alt().
//   box::key_input   — the synchronous reader: get_key() (blocking) / get_for()
//                      (timed) / try_get() (non-blocking) / available().
//   box::keyboard_events() — a Touch-backed async stream: co_await its
//                      subscription's next(), decode with box::key::from_touch.
//
// This is a box:: extension, not part of std. A process picks ONE consumer —
// the synchronous ring (box::key_input) OR the async Touch stream
// (keyboard_events) — they are independent consumers of the same key stream.
#ifndef BOXCXX_BOX_KEYBOARD_H
#define BOXCXX_BOX_KEYBOARD_H

#include <chrono>
#include <cstdint>
#include <optional>

#include "box/error.h"      // ERR_TIMEOUT
#include "box/keyboard.h"   // kb_char_t / kb_event_t / kb_status_t / kb_getchar_ex[_timeout] / kb_status + KB_MOD_*
#include "box/cxx/touch.h"  // box::subscription / box::event / box::tag (async key stream)

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

    // ── decoders ──
    static key from_kb_char(const kb_char_t &k) noexcept { return key(k.ch, k.scancode, k.flags); }
    static key from_event(const kb_event_t &e) noexcept { return key(e.ascii, e.scancode, e.mods); }
    // Decode a "keyboard" Touch event (its payload is a kb_event_t).
    static std::optional<key> from_touch(const event &ev) noexcept
    {
        if (std::optional<kb_event_t> e = ev.payload_as<kb_event_t>()) return from_event(*e);
        return std::nullopt;
    }
};

// ── box::key_input — the synchronous structured keyboard reader ─────────────
namespace key_input {

// Keys buffered and ready (0 if none); nullopt on a failed query.
inline std::optional<std::uint32_t> available() noexcept
{
    kb_status_t s{};
    if (::kb_status(&s) != 0) return std::nullopt;
    return s.available;
}

// Next key, waiting up to `timeout`; nullopt on timeout / error. The full event
// (scancode + modifiers) is preserved (the timed op keeps them).
inline std::optional<key> get_for(std::chrono::milliseconds timeout) noexcept
{
    const std::int64_t ms = timeout.count();
    const std::uint32_t to = ms <= 0 ? 0u
                           : ms > 0xFFFFFFFFLL ? 0xFFFFFFFFu
                           : static_cast<std::uint32_t>(ms);
    kb_char_t k{};
    if (::kb_getchar_ex_timeout(&k, to) != 0) return std::nullopt;
    return key::from_kb_char(k);
}

// Next key only if one is already buffered (never blocks); nullopt otherwise.
inline std::optional<key> try_get() noexcept
{
    std::optional<std::uint32_t> n = available();
    if (!n || *n == 0) return std::nullopt;
    return get_for(std::chrono::milliseconds(1));  // a key is ready → returns at once
}

// Next key, blocking until one arrives (or a hard error). Loops over bounded
// kernel waits — it never busy-spins, and a timeout alone is not "done".
inline std::optional<key> get_key() noexcept
{
    for (;;) {
        kb_char_t k{};
        int rc = ::kb_getchar_ex_timeout(&k, 60000u);
        if (rc == 0) return key::from_kb_char(k);
        if (rc == -ERR_TIMEOUT) continue;  // no key yet — keep waiting
        return std::nullopt;               // hard error
    }
}

}  // namespace key_input

// ── async — a Touch-backed key stream on the current box::executor ──────────
// Subscribe to the kernel's "keyboard" Touch tag; co_await sub.next() yields a
// box::event whose payload is a kb_event_t (decode with box::key::from_touch).
inline subscription keyboard_events() noexcept { return subscription(tag("keyboard")); }

}  // namespace box

#endif  // BOXCXX_BOX_KEYBOARD_H
