/* The keys of edit, in one table: heard from it, and the help written from it. */

#include "keymap.h"

namespace edit {

namespace {

namespace KeyCodes = painter::tui::KeyCodes;

constexpr Chord CtrlLetter(char Letter) { return {true, false, false, static_cast<std::uint8_t>(Letter)}; }
constexpr Chord Plain(char Ch) { return {false, false, false, static_cast<std::uint8_t>(Ch)}; }
constexpr Chord KeyOnly(std::uint8_t Code, bool Ctrl = false, bool Shift = false) { return {Ctrl, Shift, true, Code}; }

/* A binding whose words are empty is said by the line before it: its key is
 * named there too, unless that line names its keys in words of its own. */
constexpr Binding Table[] = {
    {CtrlLetter('s'), Command::Save, Part::Files, "save"},
    {CtrlLetter('n'), Command::Open, Part::Files, "open a file in a new tab"},
    {CtrlLetter('t'), Command::Card, Part::Files, "the file's name and tags"},
    {CtrlLetter('q'), Command::CloseTab, Part::Files, "close this tab"},
    {{true, false, false, 0, true}, Command::GoToTab, Part::Files, "go to that tab; 0 is the tenth", "Ctrl+1..0"},
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
    {{false, true, false, static_cast<std::uint8_t>(KeyCodes::Tab)}, Command::Unindent, Part::Writing,
     "take an indent back"},
    {Plain(KeyCodes::Enter), Command::NewLine, Part::Writing, "a new line, indented as this one"},
    {Plain(KeyCodes::Backspace), Command::Backspace, Part::Writing, ""},
    {KeyOnly(KeyCodes::Delete), Command::Delete, Part::Writing, ""},
    {CtrlLetter('z'), Command::Undo, Part::Writing, "undo"},
    {CtrlLetter('y'), Command::Redo, Part::Writing, "do again what was undone"},
    {CtrlLetter('c'), Command::Copy, Part::Writing, "copy; nothing selected, the line"},
    {CtrlLetter('x'), Command::Cut, Part::Writing, "cut; nothing selected, the line"},
    {CtrlLetter('v'), Command::Paste, Part::Writing, "paste"},
    {CtrlLetter('i'), Command::Overtype, Part::Writing, "type over what is there"},
    {KeyOnly(KeyCodes::Insert), Command::Overtype, Part::Writing, ""},
    {{true, false, false, static_cast<std::uint8_t>(KeyCodes::Tab)}, Command::Safety, Part::Writing,
     "safety: look, never write"},

    {CtrlLetter('f'), Command::Find, Part::Finding, "find"},
    {CtrlLetter('r'), Command::Replace, Part::Finding, "find and replace"},
    {{true, true, false, '?'}, Command::Help, Part::Finding, "these keys"},
    {{true, false, false, '/'}, Command::Help, Part::Finding, ""},
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

char Lower(char Ch) noexcept
{
    return Ch >= 'A' && Ch <= 'Z' ? static_cast<char>(Ch - 'A' + 'a') : Ch;
}

/* Ctrl and a letter is the letter whether Caps Lock was on or not. */
const Binding *Matching(bool IsKey, std::uint8_t Code, bool Ctrl, bool Shift) noexcept
{
    for (const Binding &One : Table) {
        const Chord &Keys = One.Keys;
        if (Keys.AnyDigit || Keys.Key != IsKey || Keys.Ctrl != Ctrl || Keys.Shift != Shift) continue;
        if (IsKey ? Keys.Code == Code : Lower(static_cast<char>(Keys.Code)) == Lower(static_cast<char>(Code)))
            return &One;
    }
    return nullptr;
}

std::string KeyName(const Chord &Keys)
{
    if (Keys.Key) {
        switch (Keys.Code) {
        case KeyCodes::Left:     return "Left";
        case KeyCodes::Right:    return "Right";
        case KeyCodes::Up:       return "Up";
        case KeyCodes::Down:     return "Down";
        case KeyCodes::Home:     return "Home";
        case KeyCodes::End:      return "End";
        case KeyCodes::PageUp:   return "PgUp";
        case KeyCodes::PageDown: return "PgDn";
        case KeyCodes::Insert:   return "Insert";
        case KeyCodes::Delete:   return "Delete";
        default:                 return "?";
        }
    }
    switch (static_cast<char>(Keys.Code)) {
    case KeyCodes::Escape:    return "Esc";
    case KeyCodes::Tab:       return "Tab";
    case KeyCodes::Enter:     return "Enter";
    case KeyCodes::Backspace: return "Backspace";
    default: {
        const char Ch = static_cast<char>(Keys.Code);
        return std::string(1, Ch >= 'a' && Ch <= 'z' ? static_cast<char>(Ch - 'a' + 'A') : Ch);
    }
    }
}

}  // namespace

std::span<const Binding> Bindings() noexcept
{
    return Table;
}

/* Shift on a key that moves makes the selection grow. On any other key it
 * means nothing more than the key without it — unless that very chord is
 * bound, as Ctrl+Shift+Up is. */
std::optional<Heard> Hear(const painter::tui::KeyEvent &Key) noexcept
{
    if (Key.IsAlt()) return std::nullopt;
    const bool Ctrl = Key.IsCtrl(), Shift = Key.IsShift();

    std::uint8_t Code = Key.Scancode;
    if (!Key.IsExtended()) {
        /* A terminal's Enter and Backspace on the serial line. */
        char Ch = Key.Ascii == '\r' ? KeyCodes::Enter : Key.Ascii == 0x7F ? KeyCodes::Backspace : Key.Ascii;
        if (Ctrl && !Shift && Ch >= '0' && Ch <= '9')
            return Heard{Command::GoToTab, false, static_cast<std::uint8_t>(Ch - '0')};
        Code = static_cast<std::uint8_t>(Ch);
    }

    if (const Binding *Exact = Matching(Key.IsExtended(), Code, Ctrl, Shift)) return Heard{Exact->Does};
    if (!Shift) return std::nullopt;
    const Binding *Unshifted = Matching(Key.IsExtended(), Code, Ctrl, false);
    if (!Unshifted) return std::nullopt;
    return Heard{Unshifted->Does, Moves(Unshifted->Does)};
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

std::string NameOf(const Binding &One)
{
    if (!One.Named.empty()) return std::string(One.Named);
    std::string Name;
    if (One.Keys.Ctrl) Name += "Ctrl+";
    if (One.Keys.Shift && One.Keys.Code != '?') Name += "Shift+";
    return Name + KeyName(One.Keys);
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

}  // namespace edit
