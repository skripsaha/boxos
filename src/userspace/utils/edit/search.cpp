/* Finding and replacing. */

#include "search.h"

#include <algorithm>
#include <format>

namespace edit {

namespace {

/* The rows the window covers under the tab bar: a match there would be hidden. */
constexpr std::size_t CoveredRows(bool Swapping) { return Swapping ? 4 : 3; }

}  // namespace

void Search::Begin(Tab &Active, bool Swap, core::Size ScreenValue)
{
    Screen   = ScreenValue;
    Swapping = Swap;
    OnWith   = false;
    Origin   = Active.Caret;
    FoundIn  = nullptr;

    /* What is selected on one line is what is looked for; else what was
     * looked for last, offered to be typed over. */
    if (Active.HasSelection() && Active.Selection().first.Line == Active.Selection().second.Line) {
        What.Offer(Active.Selected());
        Origin = Active.Selection().first;
    } else {
        What.Offer(What.Value());
    }
    With.Offer(With.Value());
}

bool Search::Folded() const noexcept
{
    return std::none_of(What.Value().begin(), What.Value().end(), [](char Ch) { return Ch >= 'A' && Ch <= 'Z'; });
}

const std::vector<Position> &Search::Matches(const Tab &Active)
{
    if (FoundIn != &Active || FoundVersion != Active.Body.Version() || FoundNeedle != What.Value()) {
        Found        = Active.Body.FindAll(What.Value(), Folded());
        FoundIn      = &Active;
        FoundVersion = Active.Body.Version();
        FoundNeedle  = What.Value();
    }
    return Found;
}

bool Search::AtMatch(const Tab &Active)
{
    const std::vector<Position> &All = Matches(Active);
    return std::binary_search(All.begin(), All.end(), Active.Caret);
}

/* The caret to the match; the view moved when the match is out of it or
 * under this window. */
void Search::Seek(Tab &Active, Position From, bool Forward, std::size_t Rows)
{
    std::optional<Position> Next = Active.Body.Find(What.Value(), From, Forward, Folded());
    if (!Next) return;
    Active.MoveTo(*Next, false);
    if (Next->Line < Active.Top + CoveredRows(Swapping) || Next->Line >= Active.Top + Rows) Active.Centre(Rows);
}

void Search::SwapOne(Tab &Active, std::size_t Rows, std::string &Said)
{
    if (!AtMatch(Active)) {
        Seek(Active, Active.Caret, true, Rows);
        return;
    }
    const Position At = Active.Caret;
    Active.Replace(At, {At.Line, At.Byte + What.Value().size()}, With.Value(), Merge::Never);
    Said = "replaced one";
    Seek(Active, Active.Caret, true, Rows);
}

/* From the last match to the first, so every place still means what it meant. */
void Search::SwapAll(Tab &Active, std::string &Said)
{
    const std::vector<Position> All = Matches(Active);
    if (All.empty()) return;
    Active.Past.Begin(Active.Caret);
    for (auto Each = All.rbegin(); Each != All.rend(); ++Each)
        Active.Replace(*Each, {Each->Line, Each->Byte + What.Value().size()}, With.Value(), Merge::Never);
    Active.Past.End(Active.Caret);
    Said = std::format("replaced {}", All.size());
}

Search::Outcome Search::OnKey(const tui::KeyEvent &Key, Tab &Active, std::size_t Rows, std::string &Said)
{
    const char Ch    = Key.Ascii;
    const bool Enter = !Key.IsExtended() && (Ch == tui::KeyCodes::Enter || Ch == '\r');
    if (!Key.IsExtended() && Ch == tui::KeyCodes::Escape) return Outcome::Closed;

    if (Key.IsExtended() && (Key.Scancode == tui::KeyCodes::Up || Key.Scancode == tui::KeyCodes::Down)) {
        const bool Down = Key.Scancode == tui::KeyCodes::Down;
        Seek(Active, Down && AtMatch(Active) ? Active.Body.Next(Active.Caret) : Active.Caret, Down, Rows);
        return Outcome::Stay;
    }
    if (Enter && Key.IsShift()) {
        Seek(Active, Active.Caret, false, Rows);
        return Outcome::Stay;
    }
    if (Enter && Swapping) {
        if (Key.IsCtrl()) SwapAll(Active, Said);
        else              SwapOne(Active, Rows, Said);
        return Outcome::Stay;
    }
    if (Enter) {
        Seek(Active, AtMatch(Active) ? Active.Body.Next(Active.Caret) : Active.Caret, true, Rows);
        return Outcome::Stay;
    }
    if (Swapping && !Key.IsExtended() && Ch == tui::KeyCodes::Tab) {
        OnWith = !OnWith;
        return Outcome::Stay;
    }

    if (OnWith) {
        (void)With.OnKey(Key);
        return Outcome::Stay;
    }
    const std::string Before = What.Value();
    if (!What.OnKey(Key) || What.Value() == Before) return Outcome::Stay;
    Active.MoveTo(Origin, false);
    if (!What.Value().empty()) Seek(Active, Origin, true, Rows);
    return Outcome::Stay;
}

core::Rect Search::Area() const noexcept
{
    const std::int32_t Width = std::min(Screen.Width - 2, 52);
    return {Screen.Width - Width - 1, 1, Width, static_cast<std::int32_t>(CoveredRows(Swapping))};
}

void Search::Paint(tui::Frame &Out, Tab &Active)
{
    const std::vector<Position> &All = Matches(Active);
    std::string                  Count;
    if (!What.Value().empty()) {
        const auto Here = std::lower_bound(All.begin(), All.end(), Active.Caret);
        if (All.empty())                              Count = "none";
        else if (Here != All.end() && *Here == Active.Caret)
            Count = std::format("{} of {}", Here - All.begin() + 1, All.size());
        else                                          Count = std::format("{} found", All.size());
    }

    const char *Hint = Swapping ? "Enter one  Ctrl+Enter all  Tab field  Esc done"
                                : "Enter next  Shift+Enter back  Esc done";
    const core::Rect Inside = DrawWindow(Out, Area(), Swapping ? "Replace" : "Find", Count, Hint);
    What.Paint(Out, {Inside.X, Inside.Y}, Inside.Width, !OnWith);
    if (Swapping) With.Paint(Out, {Inside.X, Inside.Y + 1}, Inside.Width, OnWith);
}

}  // namespace edit
