/* The keys of edit, in one table: heard from it, and the help written from it. */

#include "keymap.h"

namespace edit {

namespace {

namespace KeyCodes = painter::tui::KeyCodes;

using Binding = kin::Binding<Command, Part>;

constexpr Binding Table[] = {
    {CtrlLetter('s'), Command::Save, Part::Files, "save"},
    {CtrlLetter('n'), Command::Open, Part::Files, "open a file in a new tab"},
    {CtrlLetter('t'), Command::Card, Part::Files, "the file's name and tags"},
    {CtrlLetter('q'), Command::CloseTab, Part::Files, "close this tab"},
    {CtrlDigit(), Command::GoToTab, Part::Files, "go to that tab; 0 is the tenth", "Ctrl+1..0"},
    {KeyOnly(KeyCodes::PageDown, true), Command::NextTab, Part::Files, "the next tab"},
    {KeyOnly(KeyCodes::PageUp, true), Command::PriorTab, Part::Files, "the tab before"},
    {CtrlLetter('e'), Command::Scope, Part::Files, "the whole volume, or the Use Context again"},
    {Plain(KeyCodes::Escape), Command::Leave, Part::Files, "leave edit"},

    {KeyOnly(KeyCodes::Left), Command::Left, Part::Moving, "move; with Shift, select", "Arrows"},
    {KeyOnly(KeyCodes::Right), Command::Right, Part::Moving, ""},
    {KeyOnly(KeyCodes::Up), Command::Up, Part::Moving, ""},
    {KeyOnly(KeyCodes::Down), Command::Down, Part::Moving, ""},
    {KeyOnly(KeyCodes::Left, true), Command::WordLeft, Part::Moving, "a word to the left or right", "Ctrl+Left/Right"},
    {KeyOnly(KeyCodes::Right, true), Command::WordRight, Part::Moving, ""},
    {KeyOnly(KeyCodes::Home), Command::LineStart, Part::Moving, "where the words begin, then the edge"},
    {KeyOnly(KeyCodes::End), Command::LineEnd, Part::Moving, "the end of the line"},
    {KeyOnly(KeyCodes::Up, true), Command::PageUp, Part::Moving, "a whole screen up"},
    {KeyOnly(KeyCodes::PageUp), Command::PageUp, Part::Moving, ""},
    {KeyOnly(KeyCodes::Down, true), Command::PageDown, Part::Moving, "a whole screen down"},
    {KeyOnly(KeyCodes::PageDown), Command::PageDown, Part::Moving, ""},
    {KeyOnly(KeyCodes::Up, true, true), Command::FileStart, Part::Moving, "the start of the file"},
    {KeyOnly(KeyCodes::Down, true, true), Command::FileEnd, Part::Moving, "the end of the file"},
    {CtrlLetter('l'), Command::GoToLine, Part::Moving, "go to a line"},
    {CtrlLetter('a'), Command::SelectAll, Part::Moving, "select everything"},

    {Plain(KeyCodes::Tab), Command::Indent, Part::Writing, "indent; a selection, every line"},
    {Plain(KeyCodes::Tab, true), Command::Unindent, Part::Writing, "take an indent back"},
    {Plain(KeyCodes::Enter), Command::NewLine, Part::Writing, "a new line, indented as this one"},
    {Plain(KeyCodes::Backspace), Command::Backspace, Part::Writing, ""},
    {KeyOnly(KeyCodes::Delete), Command::Delete, Part::Writing, ""},
    {CtrlLetter('z'), Command::Undo, Part::Writing, "undo"},
    {CtrlLetter('y'), Command::Redo, Part::Writing, "do again what was undone"},
    {CtrlLetter('c'), Command::Copy, Part::Writing, "copy; nothing selected, the line"},
    {CtrlLetter('x'), Command::Cut, Part::Writing, "cut; nothing selected, the line"},
    {CtrlLetter('v'), Command::Paste, Part::Writing, "paste the newest clip"},
    {CtrlLetter('v', true), Command::Clips, Part::Writing, "paste one chosen from the clipboard"},
    {CtrlLetter('i'), Command::Overtype, Part::Writing, "type over what is there"},
    {KeyOnly(KeyCodes::Insert), Command::Overtype, Part::Writing, ""},
    {CtrlLetter(KeyCodes::Tab), Command::Safety, Part::Writing, "safety: look, never write"},

    {CtrlLetter('f'), Command::Find, Part::Finding, "find"},
    {CtrlLetter('r'), Command::Replace, Part::Finding, "find and replace"},
    {CtrlLetter('?', true), Command::Help, Part::Finding, "these keys"},
    {CtrlLetter('/'), Command::Help, Part::Finding, ""},
};

bool Moves(Command Does) noexcept
{
    switch (Does) {
    case Command::Left: case Command::Right: case Command::Up: case Command::Down:
    case Command::WordLeft: case Command::WordRight: case Command::LineStart: case Command::LineEnd:
    case Command::PageUp: case Command::PageDown:
        return true;
    default:
        return false;
    }
}

std::string_view PartName(Part In) noexcept
{
    switch (In) {
    case Part::Files:   return "Files and tabs";
    case Part::Moving:  return "Moving";
    case Part::Writing: return "Writing";
    case Part::Finding: return "Finding";
    }
    return "";
}

}  // namespace

std::optional<Heard> Hear(const painter::tui::KeyEvent &Key) noexcept
{
    return kin::Hear(std::span<const Binding>(Table), Key, Moves);
}

bool Writes(Command Does) noexcept
{
    switch (Does) {
    case Command::NewLine: case Command::Backspace: case Command::Delete: case Command::Indent:
    case Command::Unindent: case Command::Undo: case Command::Redo: case Command::Cut: case Command::Paste:
        return true;
    default:
        return false;
    }
}

std::vector<KeyLine> HelpLines()
{
    return KeyLines(std::span<const Binding>(Table), PartName);
}

}  // namespace edit
