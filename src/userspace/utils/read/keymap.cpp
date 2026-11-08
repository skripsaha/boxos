/* The keys of read, in one table: heard from it, and the help written from it. */

#include "keymap.h"

#include <span>
#include <string_view>

namespace read {

namespace {

namespace KeyCodes = painter::tui::KeyCodes;

using Binding = kin::Binding<Command, Part>;

constexpr Binding Table[] = {
    {CtrlLetter('n'), Command::Open, Part::Files, "open a file in a new tab"},
    {CtrlLetter('t'), Command::Card, Part::Files, "the file's name and tags"},
    {CtrlLetter('q'), Command::CloseTab, Part::Files, "close this tab"},
    {CtrlDigit(), Command::GoToTab, Part::Files, "go to that tab; 0 is the tenth", "Ctrl+1..0"},
    {KeyOnly(KeyCodes::PageDown, true), Command::NextTab, Part::Files, "the next tab"},
    {KeyOnly(KeyCodes::PageUp, true), Command::PriorTab, Part::Files, "the tab before"},
    {CtrlLetter('e'), Command::Scope, Part::Files, "the whole volume, or the Use Context again"},
    {Plain(KeyCodes::Escape), Command::Leave, Part::Files, "leave read"},

    {KeyOnly(KeyCodes::Up), Command::Up, Part::Moving, "a line up or down; with Shift, select", "Up/Down"},
    {KeyOnly(KeyCodes::Down), Command::Down, Part::Moving, ""},
    {KeyOnly(KeyCodes::Up, true), Command::PageUp, Part::Moving, "a whole page up"},
    {KeyOnly(KeyCodes::PageUp), Command::PageUp, Part::Moving, ""},
    {Plain(' ', true), Command::PageUp, Part::Moving, ""},
    {KeyOnly(KeyCodes::Down, true), Command::PageDown, Part::Moving, "a whole page down"},
    {KeyOnly(KeyCodes::PageDown), Command::PageDown, Part::Moving, ""},
    {Plain(' '), Command::PageDown, Part::Moving, ""},
    {KeyOnly(KeyCodes::Up, true, true), Command::FileStart, Part::Moving, "the start of the file"},
    {KeyOnly(KeyCodes::Down, true, true), Command::FileEnd, Part::Moving, "the end of the file"},
    {Plain(KeyCodes::Tab), Command::NextHeading, Part::Moving, "the next heading"},
    {Plain(KeyCodes::Tab, true), Command::PriorHeading, Part::Moving, "the heading before"},
    {CtrlLetter('l'), Command::Contents, Part::Moving, "the contents: go to a heading"},
    {CtrlLetter('a'), Command::SelectAll, Part::Moving, "select everything"},

    {CtrlLetter('c'), Command::Copy, Part::Reading, "copy; nothing selected, the paragraph"},
    {CtrlLetter('m'), Command::Mark, Part::Reading, "a bookmark here, or none"},
    {CtrlLetter('m', true), Command::Marks, Part::Reading, "the bookmarks"},
    {CtrlLetter('='), Command::Wider, Part::Reading, "a wider column, a narrower one", "Ctrl+= / Ctrl+-"},
    {CtrlLetter('+'), Command::Wider, Part::Reading, ""},
    {CtrlLetter('-'), Command::Narrower, Part::Reading, ""},
    {CtrlLetter('w'), Command::Turn, Part::Reading, "prose, or every line as it stands"},

    {CtrlLetter('f'), Command::Find, Part::Finding, "find"},
    {CtrlLetter('?', true), Command::Help, Part::Finding, "these keys"},
    {CtrlLetter('/'), Command::Help, Part::Finding, ""},
};

bool Moves(Command Does) noexcept
{
    switch (Does) {
    case Command::Up: case Command::Down: case Command::PageUp: case Command::PageDown:
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
    case Part::Reading: return "Reading";
    case Part::Finding: return "Finding";
    }
    return "";
}

}  // namespace

std::optional<Heard> Hear(const painter::tui::KeyEvent &Key) noexcept
{
    return kin::Hear(std::span<const Binding>(Table), Key, Moves);
}

std::vector<KeyLine> HelpLines()
{
    return KeyLines(std::span<const Binding>(Table), PartName);
}

}  // namespace read
