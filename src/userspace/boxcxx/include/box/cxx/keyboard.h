// boxcxx — box::keyboard  (structured key events: box::key + box::key_input + async)
//
// The idiomatic C++ surface over the boxlib structured keyboard API
// (box/keyboard.h). This is the RAW / structured layer — individual key events
// carrying char + scancode + modifiers — distinct from box::current<byte>'s
// "keyboard" channel, which delivers cooked text lines (the "stdin" use case).
// Same device, two layers for two needs.
//
//   box::key         — one key event: ch() / scancode() / shift()/ctrl()/alt().
//   box::key_input   — non-blocking reads of the HW key ring: available() /
//                      try_get(). The ring does NOT block, so this only polls.
//   box::keyboard_events() — the blocking / timed / async key stream (Touch):
//                      the subscription waits in the kernel — sub.wait(ms),
//                      co_await sub.next(), or sub.poll() — decode each event
//                      with box::key::from_touch.
//
// This is a box:: extension, not part of std. A process picks ONE consumer —
// the ring poller (box::key_input) OR the Touch stream (keyboard_events) — they
// are independent consumers of the same key stream. "Wait for a key" lives only
// on keyboard_events (the ring cannot block); key_input is poll-only.
#ifndef BOXCXX_BOX_KEYBOARD_H
#define BOXCXX_BOX_KEYBOARD_H

#include <cstdint>
#include <optional>

#include "box/keyboard.h"   // kb_char_t / kb_event_t / kb_status_t / kb_getchar_ex / kb_status + KB_MOD_*
#include "box/cxx/touch.h"  // box::subscription / box::event / box::tag (the key stream)

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

// ── box::key_input — non-blocking reads of the HW key ring ──────────────────
// The HW_KB_GETCHAR ring does not block (it returns "no data" at once on an
// empty buffer), so this namespace only POLLS. For "wait for a key", use
// box::keyboard_events() — the Touch channel waits in the kernel.
namespace key_input {

// Keys buffered and ready (0 if none); nullopt on a failed query.
inline std::optional<std::uint32_t> available() noexcept
{
    kb_status_t s{};
    if (::kb_status(&s) != 0) return std::nullopt;
    return s.available;
}

// The next key if one is buffered, else nullopt — never blocks (one ring poll).
inline std::optional<key> try_get() noexcept
{
    kb_char_t k{};
    if (::kb_getchar_ex(&k) == 0) return key::from_kb_char(k);
    return std::nullopt;
}

}  // namespace key_input

// ── box::keyboard_events — the blocking / timed / async key stream (Touch) ──
// Every key is also published under the kernel "keyboard" Touch tag; the
// returned subscription WAITS in the kernel (no busy-poll). Use sub.wait(ms)
// for a timed blocking read, `co_await sub.next()` on the current executor, or
// sub.poll() for a non-blocking peek — decode each box::event with
// box::key::from_touch. This is the "wait for a key" path; box::key_input only
// polls the ring. A process should use one OR the other, not both at once.
inline subscription keyboard_events() noexcept { return subscription(tag("keyboard")); }

}  // namespace box

#endif  // BOXCXX_BOX_KEYBOARD_H
