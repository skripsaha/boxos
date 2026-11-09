/* A window that is a list. */

#include "lists.h"

#include <algorithm>

#include "kin/look.h"

namespace read {

void List::Begin(std::string TitleValue, std::string HintValue, std::vector<Entry> Entries, std::size_t Chosen,
                 bool DroppableValue, core::Size ScreenValue)
{
    Title     = std::move(TitleValue);
    Hint      = std::move(HintValue);
    Lines     = std::move(Entries);
    Droppable = DroppableValue;
    Screen    = ScreenValue;
    Row       = Lines.empty() ? 0 : std::min(Chosen, Lines.size() - 1);
    First     = 0;
    Follow();
}

/* As wide as its longest line or its title or hint wants, no wider than a
 * page; as tall as its lines, no taller than the glass leaves room for. */
core::Rect List::Area() const noexcept
{
    std::int32_t Widest = static_cast<std::int32_t>(std::max(tui::CellsOf(Title), tui::CellsOf(Hint))) + 6;
    for (const Entry &One : Lines)
        Widest = std::max(Widest, One.Indent + static_cast<std::int32_t>(tui::CellsOf(One.Text) + tui::CellsOf(One.Note)) + 8);
    const std::int32_t Width  = std::min({Widest, Screen.Width - 4, 76});
    const std::int32_t Height = std::min(static_cast<std::int32_t>(std::max<std::size_t>(Lines.size(), 1)) + 2,
                                         Screen.Height - 2);
    return tui::Centred(Screen, Width, Height);
}

std::size_t List::Rows() const noexcept
{
    const core::Rect Box = Area();
    return Box.Height > 2 ? static_cast<std::size_t>(Box.Height - 2) : 1;
}

void List::Follow() noexcept
{
    const std::size_t Shown = Rows();
    if (Row < First) First = Row;
    if (Row >= First + Shown) First = Row - Shown + 1;
}

List::Outcome List::OnKey(const tui::KeyEvent &Key)
{
    const std::size_t Shown = Rows();
    if (Key.IsExtended()) {
        switch (Key.Scancode) {
        case tui::KeyCodes::Up:       if (Row > 0) Row--; break;
        case tui::KeyCodes::Down:     if (Row + 1 < Lines.size()) Row++; break;
        case tui::KeyCodes::PageUp:   Row = Row > Shown ? Row - Shown : 0; break;
        case tui::KeyCodes::PageDown: Row = Lines.empty() ? 0 : std::min(Row + Shown, Lines.size() - 1); break;
        case tui::KeyCodes::Home:     Row = 0; break;
        case tui::KeyCodes::End:      Row = Lines.empty() ? 0 : Lines.size() - 1; break;
        case tui::KeyCodes::Delete:   return Droppable && !Lines.empty() ? Outcome::Dropped : Outcome::Stay;
        default:                      return Outcome::Stay;
        }
        Follow();
        return Outcome::Stay;
    }
    if (Key.Ascii == tui::KeyCodes::Enter || Key.Ascii == '\r')
        return Lines.empty() ? Outcome::Closed : Outcome::Chosen;
    return Key.Ascii == tui::KeyCodes::Escape ? Outcome::Closed : Outcome::Stay;
}

void List::Paint(tui::Frame &Out) const
{
    const core::Rect Inside = tui::DrawWindow(Out, Area(), Title, "", Hint);
    if (Lines.empty()) {
        tui::PutText(Out, {Inside.X, Inside.Y}, "nothing here yet", Inside.Width, tui::HintInk, tui::WindowPaper);
        return;
    }
    const std::size_t Shown = Rows();
    for (std::size_t Line = 0; Line < Shown && First + Line < Lines.size(); Line++) {
        const Entry       &One   = Lines[First + Line];
        const bool         Here  = First + Line == Row;
        const core::Color  Under = Here ? tui::ChosenPaper : tui::WindowPaper;
        const std::int32_t Y     = Inside.Y + static_cast<std::int32_t>(Line);
        Out.Fill({Inside.X - 1, Y, Inside.Width + 2, 1}, tui::Cell{' ', tui::WindowInk, Under});

        const std::int32_t NoteCells = static_cast<std::int32_t>(tui::CellsOf(One.Note));
        const std::int32_t Room      = Inside.Width - One.Indent - (NoteCells > 0 ? NoteCells + 2 : 0);
        tui::PutText(Out, {Inside.X + One.Indent, Y}, tui::Shortened(One.Text, static_cast<std::size_t>(std::max(Room, 1))),
                Room, Here ? tui::TitleInk : tui::WindowInk, Under);
        if (NoteCells > 0)
            tui::PutText(Out, {Inside.X + Inside.Width - NoteCells, Y}, One.Note, NoteCells, One.Odd ? TroubleInk : tui::HintInk,
                    Under);
    }
}

}  // namespace read
