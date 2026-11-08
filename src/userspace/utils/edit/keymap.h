#ifndef EDIT_KEYMAP_H
#define EDIT_KEYMAP_H

/* Every key edit answers while the text is in front, in one table. The keys
 * are heard from it and the help is written from it, so the help can never
 * say a key does what it does not. The keys a window answers while it is open
 * are that window's own, and its bottom edge names them.
 */

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "painter/tui/stage.h"

namespace edit {

enum class Command : std::uint8_t {
    Save, Open, CloseTab, Card, GoToTab, NextTab, PriorTab, Leave,
    Left, Right, Up, Down, WordLeft, WordRight, LineStart, LineEnd, PageUp, PageDown,
    FileStart, FileEnd, GoToLine, SelectAll,
    NewLine, Backspace, Delete, Indent, Unindent, Undo, Redo, Copy, Cut, Paste, Overtype, Safety,
    Find, Replace, Help,
};

/* Where the help puts a key. */
enum class Part : std::uint8_t { Files, Moving, Writing, Finding };

/* A key with its modifiers: a character, or one of the keys that are no
 * character (Key true, Code its scancode). AnyDigit takes Ctrl and any digit. */
struct Chord {
    bool          Ctrl  = false;
    bool          Shift = false;
    bool          Key   = false;
    std::uint8_t  Code  = 0;
    bool          AnyDigit = false;
};

struct Binding {
    Chord            Keys;
    Command          Does;
    Part             In;
    std::string_view Says;        /* the help's words; empty: said with the key before it */
    std::string_view Named = {};  /* how the help names the key, when not by its chord */
};

/* What a key asks for. Extend: Shift was held on a key that moves, so the
 * selection grows. Digit: the digit of a Ctrl and digit. */
struct Heard {
    Command      Does;
    bool         Extend = false;
    std::uint8_t Digit  = 0;
};

std::span<const Binding> Bindings() noexcept;
std::optional<Heard>     Hear(const painter::tui::KeyEvent &Key) noexcept;
/* A command that changes the text, refused while the tab's safety is on. */
bool        Writes(Command Does) noexcept;
std::string NameOf(const Binding &One);
std::string_view PartName(Part In) noexcept;

}  // namespace edit

#endif
