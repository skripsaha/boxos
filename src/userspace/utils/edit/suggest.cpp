/* The tags a tags field can take, shown under it. */

#include "suggest.h"

#include <algorithm>
#include <format>

namespace edit {

void Suggest::Begin(Scope Looking)
{
    Where   = ScopeNote(Looking);
    All     = TagsWorn(Looking);
    Showing = true;
    Shown.clear();
    Row = 0;
}

void Suggest::Narrow(std::string_view Word, const std::vector<std::string> &Typed)
{
    Shown.clear();
    for (std::size_t Index = 0; Index < All.size(); Index++) {
        const std::string Tag = Spelled(All[Index].what);
        if (!std::string_view(Tag).starts_with(Word)) continue;
        if (std::find(Typed.begin(), Typed.end(), Tag) != Typed.end()) continue;
        Shown.push_back(Index);
    }
    Row = 0;
}

Suggest::Outcome Suggest::OnKey(const tui::KeyEvent &Key)
{
    if (Key.IsExtended() && Key.Scancode == tui::KeyCodes::Up) {
        if (Row > 0) Row--;
        return Outcome::Stay;
    }
    if (Key.IsExtended() && Key.Scancode == tui::KeyCodes::Down) {
        if (Row + 1 < Shown.size()) Row++;
        return Outcome::Stay;
    }
    if (Key.IsExtended() || Key.IsCtrl()) return Outcome::Passed;
    if (Key.Ascii == tui::KeyCodes::Escape) {
        Close();
        return Outcome::Closed;
    }
    if (Key.Ascii == tui::KeyCodes::Enter || Key.Ascii == '\r') {
        if (Row >= Shown.size()) return Outcome::Stay;
        Picked = Spelled(All[Shown[Row]].what);
        Close();
        return Outcome::Chosen;
    }
    return Outcome::Passed;
}

/* The chosen row stays in sight: past the rows the window has, the list is
 * shown from where it ends on it. */
void Suggest::Paint(tui::Frame &Out, core::Rect Area) const
{
    const core::Rect Inside = DrawWindow(Out, Area, "tags", Where, "Up/Down Enter take  Esc close");
    if (Shown.empty()) {
        PutText(Out, {Inside.X, Inside.Y}, All.empty() ? "no file wears a tag here" : "no tag begins so",
                Inside.Width, HintInk, WindowPaper);
        return;
    }

    const auto        Rows  = static_cast<std::size_t>(std::max(1, Inside.Height));
    const std::size_t First = Row < Rows ? 0 : Row - Rows + 1;
    for (std::size_t Line = 0; Line < Rows && First + Line < Shown.size(); Line++) {
        const box::tagfs::tag_count &One   = All[Shown[First + Line]];
        const bool                   Here  = First + Line == Row;
        const core::Color            Under = Here ? ChosenPaper : WindowPaper;
        const std::int32_t           Y     = Inside.Y + static_cast<std::int32_t>(Line);
        const std::string            Count = std::format("{}", One.files);
        const auto                   Room  = Inside.Width - static_cast<std::int32_t>(Count.size()) - 1;
        Out.Fill({Inside.X, Y, Inside.Width, 1}, tui::Cell{' ', WindowInk, Under});
        PutText(Out, {Inside.X, Y}, Shortened(Spelled(One.what), static_cast<std::size_t>(std::max(0, Room))),
                Room, Here ? TitleInk : WindowInk, Under);
        PutText(Out, {Inside.X + Inside.Width - static_cast<std::int32_t>(Count.size()), Y}, Count,
                static_cast<std::int32_t>(Count.size()), HintInk, Under);
    }
}

}  // namespace edit
