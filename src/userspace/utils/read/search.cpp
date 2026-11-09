/* Finding. */

#include "search.h"

#include <algorithm>
#include <format>

namespace read {

namespace {

char Lower(char Ch) noexcept
{
    return Ch >= 'A' && Ch <= 'Z' ? static_cast<char>(Ch - 'A' + 'a') : Ch;
}

std::string Lowered(std::string_view Text)
{
    std::string Out(Text);
    for (char &Ch : Out) Ch = Lower(Ch);
    return Out;
}

}  // namespace

/* What was looked for last is offered to be typed over, and found again at
 * once, so the count says how many there are before a key is pressed. */
void Search::Begin(Tab &Active, core::Size ScreenValue)
{
    Screen = ScreenValue;
    What.Offer(What.Value());
    if (!Active.Lines.Rows().empty()) {
        const Row &From = Active.Lines.Rows()[Active.Reading];
        OriginBlock     = From.Block;
        OriginByte      = From.From;
    }
    OriginTop     = Active.Top;
    OriginReading = Active.Reading;
    Refresh(Active);
}

void Search::Refresh(const Tab &Active)
{
    Found.clear();
    At.reset();
    const std::string &Needle = What.Value();
    if (Needle.empty()) return;

    const bool        Folded = std::none_of(Needle.begin(), Needle.end(), [](char Ch) { return Ch >= 'A' && Ch <= 'Z'; });
    const std::string Wanted = Folded ? Lowered(Needle) : Needle;
    for (std::size_t Block = 0; Block < Active.Body.Blocks.size(); Block++) {
        const std::string &Text = Active.Body.Blocks[Block].Text;
        const std::string  Hay  = Folded ? Lowered(Text) : Text;
        for (std::size_t From = Hay.find(Wanted); From != std::string::npos; From = Hay.find(Wanted, From + Wanted.size()))
            Found.push_back({Block, From, From + Wanted.size()});
    }
}

std::size_t Search::FirstFromOrigin() const noexcept
{
    for (std::size_t Index = 0; Index < Found.size(); Index++)
        if (Found[Index].Block > OriginBlock || (Found[Index].Block == OriginBlock && Found[Index].From >= OriginByte))
            return Index;
    return 0;
}

void Search::Seek(Tab &Active, std::size_t Index, std::size_t Rows)
{
    At = Index;
    Active.GoTo(Found[Index].Block, Found[Index].From, Rows, Covered);
}

Search::Outcome Search::OnKey(const tui::KeyEvent &Key, Tab &Active, std::size_t Rows)
{
    const char Ch    = Key.Ascii;
    const bool Enter = !Key.IsExtended() && (Ch == tui::KeyCodes::Enter || Ch == '\r');
    if (!Key.IsExtended() && Ch == tui::KeyCodes::Escape) return Outcome::Closed;

    const bool Down = (Enter && !Key.IsShift()) || (Key.IsExtended() && Key.Scancode == tui::KeyCodes::Down);
    const bool Up   = (Enter && Key.IsShift()) || (Key.IsExtended() && Key.Scancode == tui::KeyCodes::Up);
    if (Down || Up) {
        if (Found.empty()) return Outcome::Stay;
        const std::size_t Count = Found.size();
        if (!At)       Seek(Active, Down ? FirstFromOrigin() : (FirstFromOrigin() + Count - 1) % Count, Rows);
        else if (Down) Seek(Active, (*At + 1) % Count, Rows);
        else           Seek(Active, (*At + Count - 1) % Count, Rows);
        return Outcome::Stay;
    }

    const std::string Before = What.Value();
    if (!What.OnKey(Key) || What.Value() == Before) return Outcome::Stay;
    Refresh(Active);
    Active.Top     = OriginTop;
    Active.Reading = OriginReading;
    Active.Anchor.reset();
    if (!Found.empty()) Seek(Active, FirstFromOrigin(), Rows);
    return Outcome::Stay;
}

void Search::Paint(tui::Frame &Out) const
{
    std::string Count;
    if (!What.Value().empty()) {
        if (Found.empty()) Count = "none";
        else if (At)       Count = std::format("{} of {}", *At + 1, Found.size());
        else               Count = std::format("{} found", Found.size());
    }
    const std::int32_t Width  = std::min(Screen.Width - 2, 52);
    const core::Rect   Inside = tui::DrawWindow(Out, {Screen.Width - Width - 1, 1, Width, static_cast<std::int32_t>(Covered)},
                                           "Find", Count, "Enter next  Shift+Enter back  Esc done");
    What.Paint(Out, {Inside.X, Inside.Y}, Inside.Width, true);
}

}  // namespace read
