/* The clipboard in edit. */

#include "clips.h"

#include <algorithm>
#include <format>

#include "box/clock.h"

namespace edit {

namespace {

/* What a row shows of a clip and what typing searches: its first bytes. */
constexpr std::size_t HeadBytes = 1024;

std::string Folded(std::string_view Text)
{
    std::string Out(Text);
    for (char &Ch : Out)
        if (Ch >= 'A' && Ch <= 'Z') Ch = static_cast<char>(Ch - 'A' + 'a');
    return Out;
}

/* How long ago, in as few characters as say it: "now", "40s", "12m", "3h", "2d". */
std::string Ago(std::uint64_t ThenUs, std::uint64_t NowUs)
{
    const std::uint64_t Seconds = NowUs > ThenUs ? (NowUs - ThenUs) / 1000000 : 0;
    if (Seconds < 5) return "now";
    if (Seconds < 60) return std::format("{}s", Seconds);
    if (Seconds < 3600) return std::format("{}m", Seconds / 60);
    if (Seconds < 86400) return std::format("{}h", Seconds / 3600);
    return std::format("{}d", Seconds / 86400);
}

}  // namespace

std::optional<std::string> Clips::Put(const Clip &Took)
{
    std::expected<std::uint64_t, std::string> Number = PutClip(Took.Bytes);
    if (!Number) return Number.error();
    if (Took.WholeLine) WholeLines.push_back(*Number);
    return std::nullopt;
}

Clip Clips::AsPasted(std::uint64_t Number, std::string Bytes) const
{
    const bool WholeLine = std::find(WholeLines.begin(), WholeLines.end(), Number) != WholeLines.end();
    return {std::move(Bytes), WholeLine};
}

std::expected<std::optional<Clip>, std::string> Clips::Newest()
{
    std::expected<std::optional<Taken>, std::string> Newest = TakeNewest();
    if (!Newest) return std::unexpected(Newest.error());
    if (!*Newest) return std::optional<Clip>();
    return std::optional<Clip>(AsPasted((*Newest)->Number, std::move((*Newest)->Bytes)));
}

void Clips::Begin(core::Size ScreenValue)
{
    Screen = ScreenValue;
    Filter.Clear();
    Load();

    Row = Shown.size() > 1 ? 1 : 0;
    if (LastPasted != 0 && !Entries.empty() && Entries.front().Facts.number == NewestThen)
        for (std::size_t Index = 1; Index < Shown.size(); Index++)
            if (Entries[Shown[Index]].Facts.number == LastPasted) Row = Index - 1;
    Follow();
}

/* A clip that would not be looked at — gone between the list and the look —
 * is left out. */
void Clips::Load()
{
    OpenedUs = clock_uptime_us();
    Entries.clear();
    Trouble.clear();

    std::expected<std::vector<box::clip>, std::string> Listed = ListClips();
    if (!Listed) Trouble = Listed.error();
    for (const box::clip &One : Listed ? *Listed : std::vector<box::clip>()) {
        Entry Kept{One, {}, {}};
        if (One.takes_left == 0) {
            std::optional<std::string> Head = TakeHead(One, HeadBytes);
            if (!Head) continue;
            Kept.Head   = std::move(*Head);
            Kept.Folded = Folded(Kept.Head);
        }
        Entries.push_back(std::move(Kept));
    }
    Narrow();
}

/* A clip with takes left has nothing to be found by. */
void Clips::Narrow()
{
    const std::string Wanted = Folded(Filter.Value());
    Shown.clear();
    for (std::size_t Index = 0; Index < Entries.size(); Index++)
        if (Wanted.empty() || Entries[Index].Folded.find(Wanted) != std::string::npos) Shown.push_back(Index);
    Row   = 0;
    First = 0;
}

void Clips::Drop(std::string &Said)
{
    if (Row >= Shown.size()) return;
    const std::size_t Index = Shown[Row];
    if (std::optional<std::string> Refused = DropClip(Entries[Index].Facts.number)) {
        Said = *Refused;
        return;
    }
    Entries.erase(Entries.begin() + static_cast<std::ptrdiff_t>(Index));
    const std::size_t Kept = Row;
    Narrow();
    Row  = Shown.empty() ? 0 : std::min(Kept, Shown.size() - 1);
    Said = "the clip is dropped";
}

void Clips::Follow() noexcept
{
    const std::size_t Rows = ListRows();
    if (Row < First) First = Row;
    if (Row >= First + Rows) First = Row - Rows + 1;
}

Clips::Outcome Clips::OnKey(const tui::KeyEvent &Key, bool MayPaste, std::string &Said)
{
    const std::size_t Rows = ListRows();
    if (Key.IsExtended() && !Key.IsCtrl()) {
        switch (Key.Scancode) {
        case tui::KeyCodes::Up:       if (Row > 0) Row--; break;
        case tui::KeyCodes::Down:     if (Row + 1 < Shown.size()) Row++; break;
        case tui::KeyCodes::PageUp:   Row = Row > Rows ? Row - Rows : 0; break;
        case tui::KeyCodes::PageDown: Row = Shown.empty() ? 0 : std::min(Row + Rows, Shown.size() - 1); break;
        case tui::KeyCodes::Delete:   Drop(Said); break;
        default:
            if (Filter.OnKey(Key)) Narrow();
            return Outcome::Stay;
        }
        Follow();
        return Outcome::Stay;
    }

    const char Ch = Key.Ascii;
    if (Ch == tui::KeyCodes::Escape) return Outcome::Closed;
    if (Ch == tui::KeyCodes::Enter || Ch == '\r') {
        if (!MayPaste) return Outcome::Closed;
        if (Row >= Shown.size()) return Outcome::Stay;
        const Entry                            &One   = Entries[Shown[Row]];
        std::expected<std::string, std::string> Bytes = TakeWhole(One.Facts);
        if (!Bytes) {
            Said = Bytes.error();
            Load();
            return Outcome::Stay;
        }
        Picked     = AsPasted(One.Facts.number, std::move(*Bytes));
        LastPasted = One.Facts.number;
        NewestThen = Entries.front().Facts.number;
        return Outcome::Chosen;
    }
    if (Filter.OnKey(Key)) Narrow();
    return Outcome::Stay;
}

/* No wider than a text screen and no taller than one, as the file chooser. */
core::Rect Clips::Area() const noexcept
{
    return tui::Centred(Screen, std::min(Screen.Width - 4, 80), std::min(Screen.Height - 2, 24));
}

std::size_t Clips::ListRows() const noexcept
{
    const core::Rect Box = Area();
    return Box.Height > 4 ? static_cast<std::size_t>(Box.Height - 4) : 1;
}

void Clips::PaintRow(tui::Frame &Out, core::Rect Inside, std::int32_t Y, std::size_t Index) const
{
    const bool        Here  = Index == Row;
    const core::Color Under = Here ? ChosenPaper : tui::WindowPaper;
    Out.Fill({Inside.X, Y, Inside.Width, 1}, tui::Cell{' ', tui::WindowInk, Under});

    const Entry       &One    = Entries[Shown[Index]];
    const std::string  Age    = Ago(One.Facts.put_us, OpenedUs);
    const std::string  Size   = SizeText(One.Facts.bytes);
    const std::int32_t TextAt = Inside.X + 6;
    const std::int32_t SizeAt = Inside.X + Inside.Width - 8;
    const std::int32_t Cols   = SizeAt - 1 - TextAt;
    tui::PutText(Out, {Inside.X + 4 - static_cast<std::int32_t>(Age.size()), Y}, Age, 4, tui::HintInk, Under);
    if (One.Facts.takes_left != 0) {
        const std::uint32_t Left = One.Facts.takes_left;
        tui::PutText(Out, {TextAt, Y}, std::format("a secret: {} take{} left", Left, Left == 1 ? "" : "s"), Cols, tui::HintInk,
                Under);
    } else {
        tui::PutText(Out, {TextAt, Y}, tui::Shortened(FirstLine(One.Head), static_cast<std::size_t>(Cols)), Cols,
                Here ? tui::TitleInk : tui::WindowInk, Under);
    }
    tui::PutText(Out, {SizeAt + 8 - static_cast<std::int32_t>(Size.size()), Y}, Size, 8, tui::HintInk, Under);
}

void Clips::Paint(tui::Frame &Out) const
{
    const std::string Count  = Entries.size() == 1 ? std::string("1 clip") : std::format("{} clips", Entries.size());
    const core::Rect  Inside = tui::DrawWindow(Out, Area(), "Clipboard", Count, "Enter paste  Del drop  Esc close");
    Filter.Paint(Out, {Inside.X, Inside.Y}, Inside.Width, true);
    tui::DrawRule(Out, Inside, Inside.Y + 1);

    if (Shown.empty()) {
        const std::string_view Why = !Trouble.empty()  ? std::string_view(Trouble)
                                     : Entries.empty() ? std::string_view("the clipboard is empty")
                                                       : std::string_view("no clip's beginning holds that");
        tui::PutText(Out, {Inside.X + 2, Inside.Y + 2}, Why, Inside.Width - 2, Trouble.empty() ? tui::HintInk : TroubleInk,
                tui::WindowPaper);
        return;
    }
    const std::size_t Rows = ListRows();
    for (std::size_t Line = 0; Line < Rows && First + Line < Shown.size(); Line++)
        PaintRow(Out, Inside, Inside.Y + 2 + static_cast<std::int32_t>(Line), First + Line);
}

}  // namespace edit
