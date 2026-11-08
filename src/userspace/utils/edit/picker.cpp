/* The window a file is chosen in. */

#include "picker.h"

#include <algorithm>
#include <format>

#include "painter/tui/glyph.h"

namespace edit {

namespace {

std::string Folded(std::string_view Text)
{
    std::string Out(Text);
    for (char &Ch : Out)
        if (Ch >= 'A' && Ch <= 'Z') Ch = static_cast<char>(Ch - 'A' + 'a');
    return Out;
}

std::string SizeText(std::uint64_t Bytes)
{
    if (Bytes < 1024) return std::format("{} B", Bytes);
    if (Bytes < 1024 * 1024) return std::format("{} KB", (Bytes + 1023) / 1024);
    return std::format("{}.{} MB", Bytes / (1024 * 1024), Bytes % (1024 * 1024) * 10 / (1024 * 1024));
}

}  // namespace

void Picker::Begin(core::Size ScreenValue, std::vector<file_id_t> Open, Scope LookingValue)
{
    Screen      = ScreenValue;
    AlreadyOpen = std::move(Open);
    Filter.Clear();
    Rescope(LookingValue);
}

void Picker::Rescope(Scope LookingValue)
{
    Looking = LookingValue;
    Where   = ScopeNote(Looking);
    Load();
}

void Picker::Load()
{
    Entries.clear();
    Trouble.clear();

    std::expected<std::vector<file_id_t>, std::string> Seen = FilesSeen(Looking);
    if (!Seen) Trouble = Seen.error();
    for (const file_id_t File : Seen ? *Seen : std::vector<file_id_t>()) {
        std::expected<FileFacts, std::string> Facts = ReadFacts(File);
        if (!Facts || Facts->Trashed) continue;
        Entries.push_back({File, Facts->Name, Folded(Facts->Name), TagWords(Facts->Tags), Facts->Size});
    }

    std::sort(Entries.begin(), Entries.end(), [](const Entry &A, const Entry &B) {
        return A.Folded != B.Folded ? A.Folded < B.Folded : A.File < B.File;
    });
    Narrow();
}

/* A word fits a file when it is a piece of its name or begins one of its tags. */
bool Picker::Fits(const Entry &One, const std::vector<std::string> &Words) const
{
    for (const std::string &Word : Words) {
        const bool InName = One.Folded.find(Word) != std::string::npos;
        const bool InTags = std::any_of(One.Tags.begin(), One.Tags.end(),
                                        [&](const std::string &Tag) { return Folded(Tag).starts_with(Word); });
        if (!InName && !InTags) return false;
    }
    return true;
}

void Picker::Narrow()
{
    std::vector<std::string> Words = TagWords(Folded(Filter.Value()));
    Shown.clear();
    for (std::size_t Index = 0; Index < Entries.size(); Index++)
        if (Fits(Entries[Index], Words)) Shown.push_back(Index);

    /* One word that is no file's whole name may become a new file's name. */
    const std::string &Typed = Filter.Value();
    const bool         OneWord = !Typed.empty() && Typed.find_first_of(" ,") == std::string::npos;
    const bool         Taken = std::any_of(Entries.begin(), Entries.end(),
                                           [&](const Entry &One) { return One.Name == Typed; });
    NewName = OneWord && !Taken ? Typed : std::string();

    Row   = 0;
    First = 0;
}

/* No wider than a text screen and no taller than one, so on a wide glass the
 * list reads as a list rather than as a line across the room. */
core::Rect Picker::Area() const noexcept
{
    return Centred(Screen, std::min(Screen.Width - 4, 80), std::min(Screen.Height - 2, 24));
}

std::size_t Picker::ListRows() const noexcept
{
    const core::Rect Box = Area();
    return Box.Height > 4 ? static_cast<std::size_t>(Box.Height - 4) : 1;
}

Picker::Outcome Picker::OnKey(const tui::KeyEvent &Key)
{
    const std::size_t Rows = ListRows();
    if (Key.IsExtended() && !Key.IsCtrl()) {
        switch (Key.Scancode) {
        case tui::KeyCodes::Up:       if (Row > 0) Row--; break;
        case tui::KeyCodes::Down:     if (Row + 1 < RowCount()) Row++; break;
        case tui::KeyCodes::PageUp:   Row = Row > Rows ? Row - Rows : 0; break;
        case tui::KeyCodes::PageDown: Row = RowCount() == 0 ? 0 : std::min(Row + Rows, RowCount() - 1); break;
        default:
            if (Filter.OnKey(Key)) Narrow();
            return Outcome::Stay;
        }
        if (Row < First) First = Row;
        if (Row >= First + Rows) First = Row - Rows + 1;
        return Outcome::Stay;
    }

    const char Ch = Key.Ascii;
    if (Ch == tui::KeyCodes::Escape) return Outcome::Closed;
    if (Ch == tui::KeyCodes::Enter || Ch == '\r') {
        if (Row < Shown.size()) {
            Picked = {Entries[Shown[Row]].File, Entries[Shown[Row]].Name};
            return Outcome::Chosen;
        }
        if (OffersNew()) {
            Picked = {0, NewName};
            return Outcome::Chosen;
        }
        return Outcome::Stay;
    }
    if (Filter.OnKey(Key)) Narrow();
    return Outcome::Stay;
}

void Picker::PaintRow(tui::Frame &Out, core::Rect Inside, std::int32_t Y, std::size_t Index) const
{
    const bool        Here  = Index == Row;
    const core::Color Under = Here ? ChosenPaper : WindowPaper;
    Out.Fill({Inside.X, Y, Inside.Width, 1}, tui::Cell{' ', WindowInk, Under});

    if (Index == Shown.size()) {
        const std::int32_t Said = PutText(Out, {Inside.X + 2, Y}, "+ new file ", Inside.Width - 2, GoodInk, Under);
        PutText(Out, {Inside.X + 2 + Said, Y}, NewName, Inside.Width - 2 - Said, TitleInk, Under);
        return;
    }

    const Entry       &One      = Entries[Shown[Index]];
    const bool         IsOpen   = std::find(AlreadyOpen.begin(), AlreadyOpen.end(), One.File) != AlreadyOpen.end();
    const std::int32_t NameCols = std::min(32, Inside.Width / 2);
    if (IsOpen) Out.Put({Inside.X, Y}, tui::Cell{tui::Glyphs::Bullet, DirtyInk, Under});
    PutText(Out, {Inside.X + 2, Y}, Shortened(One.Name, static_cast<std::size_t>(NameCols)), NameCols,
            Here ? TitleInk : WindowInk, Under);

    const std::string  Size   = SizeText(One.Size);
    const std::int32_t SizeAt = Inside.X + 2 + NameCols + 1;
    PutText(Out, {SizeAt + 8 - static_cast<std::int32_t>(Size.size()), Y}, Size, 8, HintInk, Under);
    const std::int32_t TagsAt = SizeAt + 10;
    if (TagsAt < Inside.X + Inside.Width)
        PutText(Out, {TagsAt, Y}, Shortened(Joined(One.Tags, ", "), static_cast<std::size_t>(Inside.X + Inside.Width - TagsAt)),
                Inside.X + Inside.Width - TagsAt, HintInk, Under);
}

void Picker::Paint(tui::Frame &Out) const
{
    const char *Hint = Looking == Scope::Everywhere ? "Enter open  Ctrl+E the Use Context  Esc close"
                                                    : "Enter open  Ctrl+E the whole volume  Esc close";
    const core::Rect Inside = DrawWindow(Out, Area(), "Open", Where, Hint);
    Filter.Paint(Out, {Inside.X, Inside.Y}, Inside.Width, true);
    DrawRule(Out, Inside, Inside.Y + 1);

    const std::size_t Rows = ListRows();
    if (RowCount() == 0) {
        const std::string_view Why = !Trouble.empty()    ? std::string_view(Trouble)
                                     : Entries.empty()   ? std::string_view("there are no files here")
                                                         : std::string_view("no file here fits those words");
        PutText(Out, {Inside.X + 2, Inside.Y + 2}, Why, Inside.Width - 2, Trouble.empty() ? HintInk : TroubleInk,
                WindowPaper);
        return;
    }
    for (std::size_t Line = 0; Line < Rows && First + Line < RowCount(); Line++)
        PaintRow(Out, Inside, Inside.Y + 2 + static_cast<std::int32_t>(Line), First + Line);
}

}  // namespace edit
