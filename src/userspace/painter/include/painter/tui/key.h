#ifndef PAINTER_TUI_KEY_H
#define PAINTER_TUI_KEY_H

// A key as the keyboard says it (box/keyboard.h): what it types, its scancode
// and the modifiers held — what painter::tui::Keyboard hears, and what
// painter::gui's Wait tells.

#include <cstdint>
#include <string>

#include "box/keyboard.h"

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

}  // namespace painter::tui

#endif  // PAINTER_TUI_KEY_H
