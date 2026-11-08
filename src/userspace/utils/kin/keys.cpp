/* Keys the way edit and read hear them and tell them. */

#include "keys.h"

#include <algorithm>

#include "glyph.h"

namespace kin {

namespace {

namespace KeyCodes = tui::KeyCodes;

char Lower(char Ch) noexcept
{
    return Ch >= 'A' && Ch <= 'Z' ? static_cast<char>(Ch - 'A' + 'a') : Ch;
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
    case ' ':                 return "Space";
    default: {
        const char Ch = static_cast<char>(Keys.Code);
        return std::string(1, Ch >= 'a' && Ch <= 'z' ? static_cast<char>(Ch - 'a' + 'A') : Ch);
    }
    }
}

/* The heading a second column begins with: the one that leaves the taller of
 * the two columns shortest — the blank line before it stays at the foot of
 * the first; 0 when there is none. */
std::size_t SplitOf(const std::vector<KeyLine> &Lines)
{
    auto        Tall  = [&](std::size_t Index) { return std::max(Index - 1, Lines.size() - Index); };
    std::size_t Split = 0;
    for (std::size_t Index = 1; Index < Lines.size(); Index++)
        if (Lines[Index].Heading && (Split == 0 || Tall(Index) < Tall(Split))) Split = Index;
    return Split;
}

}  // namespace

std::optional<Pressed> PressedOf(const tui::KeyEvent &Key) noexcept
{
    if (Key.IsAlt()) return std::nullopt;
    Pressed Down{Key.IsExtended(), Key.Scancode, Key.IsCtrl(), Key.IsShift(), std::nullopt};
    if (!Down.Key) {
        /* A terminal's Enter and Backspace on the serial line. */
        const char Ch = Key.Ascii == '\r' ? KeyCodes::Enter : Key.Ascii == 0x7F ? KeyCodes::Backspace : Key.Ascii;
        if (Down.Ctrl && !Down.Shift && Ch >= '0' && Ch <= '9') Down.Digit = static_cast<std::uint8_t>(Ch - '0');
        Down.Code = static_cast<std::uint8_t>(Ch);
    }
    return Down;
}

bool Fits(const Chord &Keys, const Pressed &Down, bool Shift) noexcept
{
    if (Keys.AnyDigit || Keys.Key != Down.Key || Keys.Ctrl != Down.Ctrl || Keys.Shift != Shift) return false;
    return Down.Key ? Keys.Code == Down.Code
                    : Lower(static_cast<char>(Keys.Code)) == Lower(static_cast<char>(Down.Code));
}

/* '?' is a shifted key on every keyboard: it is named without its Shift. */
std::string NameOf(const Chord &Keys, std::string_view Named)
{
    if (!Named.empty()) return std::string(Named);
    std::string Name;
    if (Keys.Ctrl) Name += "Ctrl+";
    if (Keys.Shift && Keys.Code != '?') Name += "Shift+";
    return Name + KeyName(Keys);
}

/* ---- the window of keys ------------------------------------------------- */

void Help::Begin(std::vector<KeyLine> LinesValue, std::string_view ProgramValue)
{
    Lines   = std::move(LinesValue);
    Program = std::string(ProgramValue);
    First   = 0;
}

Help::Outcome Help::OnKey(const tui::KeyEvent &Key) noexcept
{
    if (Key.IsExtended() && Key.Scancode == KeyCodes::Down) {
        First++;
        return Outcome::Stay;
    }
    if (Key.IsExtended() && Key.Scancode == KeyCodes::Up) {
        First = First > 0 ? First - 1 : 0;
        return Outcome::Stay;
    }
    return Outcome::Closed;
}

void Help::Paint(tui::Frame &Out, core::Size Screen) const
{
    std::size_t KeyCells = 0, SayCells = 0;
    for (const KeyLine &One : Lines) {
        if (One.Heading) continue;
        KeyCells = std::max(KeyCells, CellsOf(One.Keys));
        SayCells = std::max(SayCells, CellsOf(One.Says));
    }
    const auto LineCells = static_cast<std::int32_t>(KeyCells + 2 + SayCells);

    /* The blank line before the heading the second column begins with stays
     * behind at the foot of the first. */
    const std::size_t  Split  = SplitOf(Lines);
    const bool         Two    = Split != 0 && 2 * LineCells + 3 <= Screen.Width - 6;
    const std::size_t  Tall   = Two ? std::max(Split - 1, Lines.size() - Split) : Lines.size();
    const std::int32_t Width  = Two ? 2 * LineCells + 3 + 4 : LineCells + 4;
    const core::Rect   Area   = Centred(Screen, Width, static_cast<std::int32_t>(Tall) + 2);
    const auto         Rows   = static_cast<std::size_t>(Area.Height - 2);
    const std::size_t  From   = std::min(First, Tall > Rows ? Tall - Rows : 0);
    const core::Rect   Inside = DrawWindow(Out, Area, "Keys", Program,
                                           Tall > Rows ? "Up/Down scroll  any other key closes" : "any key closes");

    auto PaintColumn = [&](std::size_t Begin, std::size_t End, std::int32_t X) {
        for (std::size_t Row = 0; Row < Rows && Begin + From + Row < End; Row++) {
            const KeyLine     &One = Lines[Begin + From + Row];
            const std::int32_t Y   = Inside.Y + static_cast<std::int32_t>(Row);
            if (One.Heading) {
                PutText(Out, {X, Y}, One.Keys, LineCells, TabNumberInk, WindowPaper);
                continue;
            }
            PutText(Out, {X, Y}, One.Keys, static_cast<std::int32_t>(KeyCells), TitleInk, WindowPaper);
            PutText(Out, {X + static_cast<std::int32_t>(KeyCells) + 2, Y}, One.Says, static_cast<std::int32_t>(SayCells),
                    WindowInk, WindowPaper);
        }
    };
    PaintColumn(0, Two ? Split - 1 : Lines.size(), Inside.X);
    if (Two) PaintColumn(Split, Lines.size(), Inside.X + LineCells + 3);
}

}  // namespace kin
