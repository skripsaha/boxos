#ifndef EDIT_KEYMAP_H
#define EDIT_KEYMAP_H

/* Every key edit answers while the text is in front, in one table (kin/keys.h
 * says how a table is heard and told). The keys a window answers while it is
 * open are that window's own, and its bottom edge names them.
 */

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "kin/keys.h"

namespace edit {

using namespace kin;

enum class Command : std::uint8_t {
    Save, Open, CloseTab, Card, GoToTab, NextTab, PriorTab, Scope, Leave,
    Left, Right, Up, Down, WordLeft, WordRight, LineStart, LineEnd, PageUp, PageDown,
    FileStart, FileEnd, GoToLine, SelectAll,
    NewLine, Backspace, Delete, Indent, Unindent, Undo, Redo, Copy, Cut, Paste, Clips, Overtype, Safety,
    Find, Replace, Help,
};

/* Where the help puts a key. */
enum class Part : std::uint8_t { Files, Moving, Writing, Finding };

using Heard = kin::Heard<Command>;

std::optional<Heard> Hear(const painter::tui::KeyEvent &Key) noexcept;
/* A command that changes the text: while the tab's safety is on, it does
 * nothing. */
bool                 Writes(Command Does) noexcept;
/* The help, written from the table. */
std::vector<KeyLine> HelpLines();

}  // namespace edit

#endif
