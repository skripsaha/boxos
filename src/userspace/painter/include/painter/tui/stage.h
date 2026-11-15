#ifndef PAINTER_TUI_STAGE_H
#define PAINTER_TUI_STAGE_H

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

#include "box/cxx/error.h"
#include "box/keyboard.h"

#include "painter/core/geometry.h"
#include "painter/tui/frame.h"

namespace painter::tui {

// The keyboard's own numbers (box/keyboard.h), named for Painter.
namespace Modifiers {
inline constexpr std::uint8_t Shift     = KB_MOD_SHIFT;
inline constexpr std::uint8_t Ctrl      = KB_MOD_CTRL;
inline constexpr std::uint8_t Alt       = KB_MOD_ALT;
inline constexpr std::uint8_t Extended  = KB_MOD_EXTENDED;
}  // namespace Modifiers

namespace KeyCodes {
inline constexpr char Escape    = KEY_ESCAPE;
inline constexpr char Backspace = KEY_BACKSPACE;
inline constexpr char Enter     = KEY_ENTER;
inline constexpr char Tab       = KEY_TAB;
inline constexpr std::uint8_t Home     = KEY_HOME;
inline constexpr std::uint8_t Up       = KEY_UP;
inline constexpr std::uint8_t PageUp   = KEY_PAGEUP;
inline constexpr std::uint8_t Left     = KEY_LEFT;
inline constexpr std::uint8_t Right    = KEY_RIGHT;
inline constexpr std::uint8_t End      = KEY_END;
inline constexpr std::uint8_t Down     = KEY_DOWN;
inline constexpr std::uint8_t PageDown = KEY_PAGEDN;
inline constexpr std::uint8_t Insert   = KEY_INSERT;
inline constexpr std::uint8_t Delete   = KEY_DELETE;
}  // namespace KeyCodes

enum class StageErrorKind {
    DimensionsUnavailable,
    NoConsole,
    TooSmall,
    FrameUnavailable,
    KeyboardUnavailable,
    InvalidState,
};

struct StageError {
    StageErrorKind Kind = StageErrorKind::InvalidState;
    box::error     Cause;
    core::Size     Actual;
    core::Size     Required;

    std::string Describe(std::string_view Subject) const;
};

struct KeyEvent {
    char32_t     Character = 0;   // what the key types, Unicode; 0 for a key that types none
    std::uint8_t Scancode  = 0;
    std::uint8_t Mods      = 0;

    bool HasCharacter() const noexcept { return Character != 0; }
    // A character that shows — none of the C0 or C1 controls, nor DEL, nor
    // what is no character at all.
    bool IsPrintable() const noexcept
    {
        return Character >= 0x20 && Character != 0x7F && !(Character >= 0x80 && Character < 0xA0) &&
               !(Character >= 0xD800 && Character <= 0xDFFF) && Character <= 0x10FFFF;
    }
    // The character as UTF-8 — what a line of text it is typed into holds;
    // empty for a key that types none.
    std::string Spelling() const;
    bool IsExtended() const noexcept { return (Mods & Modifiers::Extended) != 0; }
    bool IsShift() const noexcept { return (Mods & Modifiers::Shift) != 0; }
    bool IsCtrl() const noexcept { return (Mods & Modifiers::Ctrl) != 0; }
    bool IsAlt() const noexcept { return (Mods & Modifiers::Alt) != 0; }
};

// EndAsked: the program is asked to finish — Ctrl+Alt+End over it, said by
// its keeper through the display. Only a program that said it answers that
// itself (HearEnd) hears it; any other hears Gone, and ends as it ends when
// there is nothing left to hear.
enum class EventKind { Key, Quiet, Gone, EndAsked };

struct Event {
    EventKind Kind = EventKind::Gone;
    KeyEvent  Key;
    box::error Cause;
};

class Stage {
public:
    static std::expected<Stage, StageError> Open(std::int32_t LeastCols, std::int32_t LeastRows) noexcept;

    Stage(Stage &&Other) noexcept;
    Stage &operator=(Stage &&Other) noexcept;
    Stage(const Stage &)            = delete;
    Stage &operator=(const Stage &) = delete;
    ~Stage();

    core::Size Size() const noexcept;
    bool       IsOpen() const noexcept;
    // Whether the glass is a text page or pixels, as the display holds it.
    GlassKind  Glass() const noexcept;
    // The grids finer than the halves it draws exactly (box/display.h DisplayMosaic).
    std::uint8_t Mosaics() const noexcept;

    Frame &FrameBuffer() noexcept;
    const Frame &FrameBuffer() const noexcept;

    box::status Present() noexcept;
    Event       Poll() noexcept;
    Event       Wait(std::uint32_t TimeoutMs = 0) noexcept;
    // The program answers a request to finish itself: EndAsked, not Gone.
    void        HearEnd() noexcept;
    box::status Close() noexcept;

private:
    struct Impl;
    explicit Stage(std::unique_ptr<Impl> State) noexcept;

    std::unique_ptr<Impl> State;
};

}  // namespace painter::tui

#endif  // PAINTER_TUI_STAGE_H
