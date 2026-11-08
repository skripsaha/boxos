#ifndef PAINTER_TUI_KEYBOARD_H
#define PAINTER_TUI_KEYBOARD_H

#include <expected>
#include <memory>

#include "painter/tui/stage.h"

namespace painter::tui {

/* Keyboard owns only the keyboard subscription. It does not claim the screen,
 * clear output, or impose a full-screen lifecycle; pair it with Surface for
 * interactive overlays and regional applications. */
class Keyboard {
public:
    static std::expected<Keyboard, StageError> Open() noexcept;

    Keyboard(Keyboard &&) noexcept;
    Keyboard &operator=(Keyboard &&) noexcept;
    Keyboard(const Keyboard &)            = delete;
    Keyboard &operator=(const Keyboard &) = delete;
    ~Keyboard();

    bool  IsOpen() const noexcept;
    Event Poll() noexcept;
    Event Wait(std::uint32_t TimeoutMs = 0) noexcept;
    box::status Close() noexcept;

private:
    struct Impl;
    explicit Keyboard(std::unique_ptr<Impl> State) noexcept;

    std::unique_ptr<Impl> State;
};

}  // namespace painter::tui

#endif  // PAINTER_TUI_KEYBOARD_H
