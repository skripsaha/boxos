/* paint.cpp — the map's picture: a line at the top with all of memory in one
 * strip, the cells of the map and its legend of owners, what is under the
 * cursor, and the line of keys; a window of every key over them when asked.
 * On a glass of pixels a cell is a square of them; on the 80 by 25 text
 * screen it is a character, told from the next owner's by its shade. */

#include <algorithm>
#include <format>
#include <string>
#include <string_view>

#include "painter/tui/glyph.h"
#include "painter/tui/text.h"
#include "painter/tui/window.h"

#include "board.h"

namespace memmap {

namespace {

namespace tui    = painter::tui;
namespace Glyphs = painter::tui::Glyphs;
using painter::core::Point;
using painter::core::Rect;

// A glass this wide or wider is one of pixels: the text screen is 80 wide.
constexpr std::int32_t PixelGlassCols = 100;
// The right of the top line is where the programs that live on their own lay
// their notices: the load, the memory and the time.
constexpr std::int32_t NoticePixels   = 44;
constexpr std::int32_t NoticeText     = 34;
constexpr std::int32_t SizeCells      = 6;
constexpr std::int32_t DeltaCells     = 7;
// The share of the way toward the panel's paper that what is not chosen is
// drawn at, and toward white that what is new and what is chosen are.
constexpr std::uint8_t Dimmed  = 150;
constexpr std::uint8_t Brought = 120;
constexpr std::uint8_t Under   = 100;

// Between the words of a line: a middle dot, as text (the font folds it).
constexpr std::string_view Between = "\xC2\xB7";

std::string Bytes(std::uint64_t Count)
{
    constexpr std::uint64_t Kib = 1024, Mib = Kib * 1024, Gib = Mib * 1024;
    const auto Tenths = [](std::uint64_t N, std::uint64_t Unit) {
        const std::uint64_t T = N * 10 / Unit;
        return T >= 1000 ? std::format("{}", T / 10) : std::format("{}.{}", T / 10, T % 10);
    };
    if (Count >= Gib) return Tenths(Count, Gib) + "G";
    if (Count >= Mib) return Tenths(Count, Mib) + "M";
    if (Count >= Kib) return std::format("{}K", Count / Kib);
    return std::format("{}B", Count);
}

std::string Signed(std::int64_t Change)
{
    const std::uint64_t Size = static_cast<std::uint64_t>(Change < 0 ? -Change : Change);
    return (Change < 0 ? "-" : "+") + Bytes(Size);
}

std::string Lasted(std::uint64_t Us)
{
    const std::uint64_t Seconds = Us / 1000000u, Minutes = Seconds / 60u, Hours = Minutes / 60u, Days = Hours / 24u;
    if (Seconds < 60) return std::format("{}s", Seconds);
    if (Minutes < 60) return std::format("{}m{:02}s", Minutes, Seconds % 60u);
    if (Hours < 24) return std::format("{}h{:02}m", Hours, Minutes % 60u);
    return std::format("{}d{:02}h", Days, Hours % 24u);
}

std::int32_t Cells(std::string_view Text)
{
    return static_cast<std::int32_t>(tui::CellsOf(Text));
}

void Say(tui::Frame &Out, Point At, std::string_view Text, std::int32_t Width, Color With, Color Over)
{
    if (Width > 0) tui::PutText(Out, At, tui::Shortened(Text, static_cast<std::size_t>(Width)), Width, With, Over);
}

// Text ending at Right, the cell after its last.
void SayRight(tui::Frame &Out, std::int32_t Row, std::int32_t Left, std::int32_t Right, std::string_view Text,
              Color With, Color Over)
{
    const std::int32_t Width = std::min(Cells(Text), Right - Left);
    Say(Out, Point{Right - Width, Row}, Text, Width, With, Over);
}

// A panel over Area: its paper, a frame of rules, Title in the top edge at the
// left and Note at its right; what is inside the frame is returned.
Rect DrawPanel(tui::Frame &Out, const Look &Shown, Rect Area, std::string_view Title, std::string_view Note)
{
    Out.Fill(Area, ' ', {Shown.Text, Shown.Panel});
    if (Area.Width < 4 || Area.Height < 3) return Rect{Area.X, Area.Y, 0, 0};
    const std::int32_t Right = Area.X + Area.Width - 1, Bottom = Area.Y + Area.Height - 1;
    const tui::TextStyle Rule{Shown.Frame, Shown.Panel};
    for (std::int32_t X = Area.X + 1; X < Right; X++) {
        Out.Put(Point{X, Area.Y}, Glyphs::RuleAcross, Rule);
        Out.Put(Point{X, Bottom}, Glyphs::RuleAcross, Rule);
    }
    for (std::int32_t Y = Area.Y + 1; Y < Bottom; Y++) {
        Out.Put(Point{Area.X, Y}, Glyphs::RuleDown, Rule);
        Out.Put(Point{Right, Y}, Glyphs::RuleDown, Rule);
    }
    Out.Put(Point{Area.X, Area.Y}, Glyphs::CornerTL, Rule);
    Out.Put(Point{Right, Area.Y}, Glyphs::CornerTR, Rule);
    Out.Put(Point{Area.X, Bottom}, Glyphs::CornerBL, Rule);
    Out.Put(Point{Right, Bottom}, Glyphs::CornerBR, Rule);
    const std::string Named = std::format(" {} ", Title);
    Say(Out, Point{Area.X + 2, Area.Y}, Named, Area.Width - 4, Shown.Title, Shown.Panel);
    if (!Note.empty())
        SayRight(Out, Area.Y, Area.X + 3 + Cells(Named), Right - 1, std::format(" {} ", Note), Shown.Quiet, Shown.Panel);
    return Rect{Area.X + 1, Area.Y + 1, Area.Width - 2, Area.Height - 2};
}

const char *ViewName(ViewOf View) noexcept
{
    return View == ViewOf::Owners ? "by owner" : "by address";
}

const char *SortName(SortBy By) noexcept
{
    return By == SortBy::Age ? "by age" : By == SortBy::Name ? "by name" : "by size";
}

// A share, in whole percent, but "<1%" for one that is something.
std::string Percent(std::uint64_t Part, std::uint64_t Whole)
{
    if (Whole == 0 || Part == 0) return "0%";
    const std::uint64_t Share = Part * 100 / Whole;
    return Share == 0 ? "<1%" : std::format("{}%", Share);
}

}  // namespace

Board::Plan Board::Planned() const
{
    const painter::core::Size Size = Screen.Size();
    Plan                      Made;
    Made.Pixels                  = Size.Width >= PixelGlassCols;
    const std::int32_t Body      = std::max(0, Size.Height - 2);
    const std::int32_t InfoRows  = std::min(Body / 3, Made.Pixels ? 7 : 6);
    const std::int32_t LegendCols = Made.Pixels ? std::clamp(Size.Width * 28 / 100, 40, 58) : 38;
    Made.Map    = Rect{0, 1, Size.Width - LegendCols, Body - InfoRows};
    Made.Legend = Rect{Size.Width - LegendCols, 1, LegendCols, Body - InfoRows};
    Made.Info   = Rect{0, 1 + Body - InfoRows, Size.Width, InfoRows};
    Made.Inside = Rect{Made.Map.X + 1, Made.Map.Y + 1, std::max(0, Made.Map.Width - 2), std::max(0, Made.Map.Height - 2)};
    Made.Side   = Made.Pixels ? static_cast<std::int32_t>(std::clamp<std::uint32_t>(Shown.CellPixels, 1, 8)) : 1;
    Made.Cols   = Made.Inside.Width / Made.Side;
    Made.Rows   = (Made.Pixels ? Made.Inside.Height * 2 : Made.Inside.Height) / Made.Side;
    return Made;
}

void Board::Paint()
{
    tui::Frame &Out = Screen.FrameBuffer();
    Out.Clear({Shown.Text, Shown.Page});
    const painter::core::Size Size = Screen.Size();
    if (Size.Width <= 0 || Size.Height < 6) return;
    Grid = Planned();
    Lay();
    const Owner *Under = Chosen();
    Ticks              = Under && Under->IsProgram() && Shown.View == ViewOf::Owners;
    PaintHeader(Out, Rect{0, 0, Size.Width, 1});
    PaintStatus(Out, Rect{0, Size.Height - 1, Size.Width, 1});
    PaintMap(Out);
    PaintLegend(Out);
    PaintInfo(Out);
    if (Help) PaintHelp(Out);
}

void Board::PaintHeader(tui::Frame &Out, Rect Area) const
{
    Out.Fill(Area, ' ', {Shown.Text, Shown.Bars});
    Say(Out, Point{Area.X + 1, Area.Y}, "memmap", 6, Shown.Title, Shown.Bars);
    const std::int32_t Strip = Grid.Pixels ? 24 : 12;
    const std::int32_t Left  = Area.X + 8;
    const std::int32_t Room  = Area.Width - (Grid.Pixels ? NoticePixels : NoticeText);
    if (Left + Strip > Room) return;
    PaintStrip(Out, Rect{Left, Area.Y, Strip, 1});
    if (!Now) return;
    const std::uint64_t Free = Now->Total > Now->Used ? Now->Total - Now->Used : 0;
    const std::string   Line = Grid.Pixels
        ? std::format("{} memory {} {} in use ({}) {} {} free {} {} {} {}", Bytes(Now->Total), Between, Bytes(Now->Used),
                      Percent(Now->Used, Now->Total), Between, Bytes(Free), Between, ViewName(Shown.View), Between,
                      SortName(Shown.Sort))
        : std::format("{} of {} {} {}", Bytes(Now->Used), Bytes(Now->Total), Between, Percent(Now->Used, Now->Total));
    const std::int32_t From = Left + Strip + 2;
    Say(Out, Point{From, Area.Y}, Line, Room - From, Shown.Text, Shown.Bars);
    if (Paused) SayRight(Out, Area.Y, From, Room, "paused", Shown.Marked, Shown.Bars);
}

// All of memory in one strip: the programs, then what the kernel holds in its
// kinds, then the free — the same colors as the map's.
void Board::PaintStrip(tui::Frame &Out, Rect Area) const
{
    if (!Now || Now->Total == 0) return;
    struct Share {
        Color         Ink;
        std::uint64_t Bytes = 0;
    };
    std::vector<Share> Shares = {{Shown.Programs, 0}, {Shown.Image, 0}, {Shown.Kernel, 0},
                                 {Shown.Rings, 0},    {Shown.Shared, 0}, {Shown.Free, 0}};
    for (const Owner &One : Fine) {
        const std::size_t At = One.IsProgram() ? 0 : One.What == Kind::Image ? 1 : One.What == Kind::Kernel ? 2
                                                  : One.What == Kind::Rings ? 3 : One.What == Kind::Shared ? 4 : 5;
        Shares[At].Bytes += One.Bytes;
    }
    std::uint64_t Sum = 0;
    std::int32_t  X   = Area.X;
    for (const Share &One : Shares) {
        Sum += One.Bytes;
        const std::int32_t Until = Area.X + static_cast<std::int32_t>(Sum * static_cast<std::uint64_t>(Area.Width) / Now->Total);
        for (; X < std::min(Until, Area.X + Area.Width); X++)
            Out.Put(Point{X, Area.Y}, Glyphs::Full, {One.Ink, Shown.Bars});
    }
    for (; X < Area.X + Area.Width; X++) Out.Put(Point{X, Area.Y}, Glyphs::Full, {Shown.Frame, Shown.Bars});
}

void Board::PaintMap(tui::Frame &Out) const
{
    const std::string Title = Shown.View == ViewOf::Owners ? "memory by owner" : "address space";
    std::string       Note;
    if (Now && Laid.CellBytes != 0) Note = std::format("{} a cell", Bytes(Laid.CellBytes));
    DrawPanel(Out, Shown, Grid.Map, Title, Note);
    PaintCells(Out);
}

void Board::PaintCells(tui::Frame &Out) const
{
    const Rect &In = Grid.Inside;
    if (In.Empty() || Grid.Cols == 0 || Grid.Rows == 0) return;
    // The owner under the cursor stands out from the rest, unless it is the
    // free memory or the untold: those are most of the map, and pointing at
    // them would put out everything else.
    const Owner *Pick = Chosen();
    if (Pick && (Pick->What == Kind::Free || Pick->What == Kind::Untold)) Pick = nullptr;
    const auto InkOf = [&](const Cell &One, bool Here) {
        const Owner &Of  = Owners[One.Owner];
        Color        Ink = Of.Ink;
        if (Filter.empty() ? (Pick && &Of != Pick) : !Of.Holds(Filter)) Ink = Color::Mix(Ink, Shown.Panel, Dimmed);
        if (One.Fresh) Ink = Color::Mix(Ink, painter::core::Colors::White, Brought);
        if (Here) Ink = Color::Mix(Ink, painter::core::Colors::White, Under);
        return Ink;
    };
    const std::int32_t Side = Grid.Side;
    // Cells are centred in what they are drawn in; on pixels each leaves one
    // for the gap to the next, and the cursor's ring stands in the gaps.
    const std::int32_t Gap  = Grid.Pixels && Side >= 2 ? 1 : 0;
    const std::int32_t FromX = In.X + (In.Width - Grid.Cols * Side) / 2;
    const std::int32_t FromY = (Grid.Pixels ? In.Y * 2 : In.Y) + ((Grid.Pixels ? In.Height * 2 : In.Height) - Grid.Rows * Side) / 2;
    if (Grid.Pixels) Out.Fill(In, tui::Cell{Glyphs::UpperHalf, Shown.Panel, Shown.Panel});

    for (std::size_t Index = 0; Index < Laid.Cells.size(); Index++) {
        const Cell &One = Laid.Cells[Index];
        if (One.Owner == NoOwner) continue;
        const bool         Here = Index == Cursor;
        const Color        Ink  = InkOf(One, Here);
        const std::int32_t X    = FromX + static_cast<std::int32_t>(Index % static_cast<std::size_t>(Grid.Cols)) * Side;
        const std::int32_t Y    = FromY + static_cast<std::int32_t>(Index / static_cast<std::size_t>(Grid.Cols)) * Side;
        if (!Grid.Pixels) {
            Out.Put(Point{X, Y}, tui::Cell{Owners[One.Owner].Mark, Ink, Here ? Shown.Title : Shown.Panel});
            continue;
        }
        for (std::int32_t Down = 0; Down < Side - Gap; Down++)
            for (std::int32_t Across = 0; Across < Side - Gap; Across++) Out.PutPixel(Point{X + Across, Y + Down}, Ink);
    }
    if (!Grid.Pixels || Gap == 0 || Cursor >= Laid.Cells.size()) return;
    const std::int32_t X = FromX + static_cast<std::int32_t>(Cursor % static_cast<std::size_t>(Grid.Cols)) * Side;
    const std::int32_t Y = FromY + static_cast<std::int32_t>(Cursor / static_cast<std::size_t>(Grid.Cols)) * Side;
    const auto Ring = [&](std::int32_t PixelX, std::int32_t PixelY) {
        if (PixelX >= In.X && PixelX < In.X + In.Width && PixelY >= In.Y * 2 && PixelY < (In.Y + In.Height) * 2)
            Out.PutPixel(Point{PixelX, PixelY}, Shown.Title);
    };
    for (std::int32_t Step = -1; Step < Side; Step++) {
        Ring(X + Step, Y - 1);
        Ring(X + Step, Y + Side - 1);
        Ring(X - 1, Y + Step);
        Ring(X + Side - 1, Y + Step);
    }
}

void Board::PaintLegend(tui::Frame &Out) const
{
    const Rect In = DrawPanel(Out, Shown, Grid.Legend, Shown.View == ViewOf::Owners ? "who holds it" : "what is told",
                              std::format("{}", Owners.size()));
    if (In.Empty() || !Now) return;
    // Bars are drawn against the largest owner that is named, so that the
    // free memory does not leave every other bar a sliver.
    std::uint64_t Whole = 0;
    for (const Owner &One : Owners)
        if (One.What != Kind::Free && One.What != Kind::Untold) Whole = std::max(Whole, One.Bytes);
    if (Whole == 0) Whole = Shown.View == ViewOf::Owners ? Now->Total : Now->Span;
    const std::int32_t  Rows    = In.Height;
    const std::size_t   Listed  = Owners.size() > static_cast<std::size_t>(Rows) ? static_cast<std::size_t>(Rows) - 1 : Owners.size();
    const bool          Deltas  = Shown.View == ViewOf::Owners && In.Width >= 36;
    const std::int32_t  Rest    = In.Width - 3 - SizeCells - 1 - (Deltas ? DeltaCells : 0);
    std::int32_t        NameCells = std::clamp(Rest * 55 / 100, 12, 16);
    std::int32_t        BarCells  = Rest - NameCells - 1;
    if (BarCells < 3) {
        NameCells = Rest;
        BarCells  = 0;
    }
    const Owner *Pick = Chosen();
    for (std::size_t Index = 0; Index < Listed; Index++) {
        const Owner       &One   = Owners[Index];
        const std::int32_t Row   = In.Y + static_cast<std::int32_t>(Index);
        const Color        Paper = &One == Pick ? tui::ChosenPaper : Shown.Panel;
        Out.Fill(Rect{In.X, Row, In.Width, 1}, ' ', {Shown.Text, Paper});
        const bool IsMarked = std::any_of(Marked.begin(), Marked.end(), [&](const Mark &M) { return M.Key == One.Key; });
        if (IsMarked) Out.Put(Point{In.X, Row}, '*', {Shown.Marked, Paper});
        Out.Put(Point{In.X + 1, Row}, Grid.Pixels ? Glyphs::Full : One.Mark, {One.Ink, Paper});
        const bool Matches = Filter.empty() || One.Holds(Filter);
        std::int32_t X = In.X + 3;
        Say(Out, Point{X, Row}, One.Name, NameCells, &One == Pick ? Shown.Title : Matches ? Shown.Text : Shown.Quiet, Paper);
        X += NameCells + 1;
        const std::string Size = Bytes(One.Bytes);
        SayRight(Out, Row, X, X + SizeCells, Size, Shown.Text, Paper);
        X += SizeCells + 1;
        if (BarCells > 0) {
            for (std::int32_t Step = 0; Step < BarCells; Step++) Out.Put(Point{X + Step, Row}, Glyphs::Full, {Shown.Bars, Paper});
            const std::uint64_t Halves = std::clamp<std::uint64_t>(
                Whole == 0 ? 0 : (One.Bytes * static_cast<std::uint64_t>(BarCells) * 2 + Whole / 2) / Whole, 1,
                static_cast<std::uint64_t>(BarCells) * 2);
            for (std::uint64_t Half = 0; Half < Halves; Half += 2)
                Out.Put(Point{X + static_cast<std::int32_t>(Half / 2), Row}, Halves - Half >= 2 ? Glyphs::Full : Glyphs::LeftHalf,
                        {One.Ink, Shown.Bars});
            X += BarCells + 1;
        }
        if (Deltas && One.Since != 0)
            SayRight(Out, Row, X, In.X + In.Width, Signed(One.Since), One.Since > 0 ? Shown.Grew : Shown.Shrank, Paper);
    }
    if (Listed < Owners.size()) {
        std::uint64_t Others = 0;
        for (std::size_t Index = Listed; Index < Owners.size(); Index++) Others += Owners[Index].Bytes;
        Say(Out, Point{In.X + 3, In.Y + Rows - 1},
            std::format("{} more {} {}", Owners.size() - Listed, Between, Bytes(Others)), In.Width - 3, Shown.Quiet, Shown.Panel);
    }
}

// What the cursor is on, said in lines: who holds the cell and how much, then
// where it is or which of its owner's cells it is, then what it is.
std::vector<Board::InfoLine> Board::InfoLines() const
{
    const Owner *One = Chosen();
    if (!One)
        return {{Laid.Cells.empty() ? "nothing to show" : "no memory here: no region of the registry lies in this cell",
                 Shown.Quiet}};
    const bool          ByOwner = Shown.View == ViewOf::Owners;
    const std::uint64_t Whole   = ByOwner ? Now->Total : Now->Span;
    std::vector<InfoLine> Lines;
    std::string           Held = std::format("{} {} {} {} {} of {}", One->Name, Between, Bytes(One->Bytes), Between,
                                             Percent(One->Bytes, Whole), ByOwner ? "memory" : "the address space");
    if (ByOwner && One->Since != 0) Held += std::format(" {} {} since the map opened", Between, Signed(One->Since));
    Lines.push_back({Held, Shown.Title});

    if (ByOwner) {
        const Cell  &Here  = Laid.Cells[Cursor];
        std::string  Which = std::format("cell {} of {} of this owner {} {} each", Here.Nth + 1, Laid.Run[Here.Owner],
                                         Between, Bytes(Laid.CellBytes));
        if (One->IsProgram()) {
            const std::uint64_t Up = Now->AtUs > One->BeganUs ? Now->AtUs - One->BeganUs : 0;
            Which = std::format("pid {} {} up {} {} ", One->Pid, Between, Lasted(Up), Between) + Which;
        }
        Lines.push_back({Which, Shown.Text});
        if (One->IsProgram() && !One->Tags.empty()) Lines.push_back({"tags " + One->Tags, Shown.Quiet});
        Lines.push_back({One->Note, Shown.Quiet});
        return Lines;
    }
    const std::uint64_t        From   = Laid.Base(Cursor);
    const std::vector<Segment> Inside = PartsWithin(Parts, From, From + Laid.CellBytes);
    Lines.push_back({std::format("{:#x} to {:#x} {} {}", From, From + Laid.CellBytes, Between, Bytes(Laid.CellBytes)),
                     Shown.Text});
    std::string Told;
    for (const Segment &Part : Inside) {
        if (!Told.empty()) Told += ", ";
        Told += std::format("{} {}", KindName(Part.Region->What), Bytes(Part.Bytes));
    }
    Lines.push_back({"inside: " + Told, Shown.Text});
    const auto Of = std::find_if(Inside.begin(), Inside.end(), [&](const Segment &Part) {
        return OwnerOfRegion(Owners, *Part.Region) == static_cast<std::ptrdiff_t>(Laid.Cells[Cursor].Owner);
    });
    Lines.push_back({Of != Inside.end() ? "tags " + Of->Region->Tags : One->Note, Shown.Quiet});
    return Lines;
}

void Board::PaintInfo(tui::Frame &Out) const
{
    const std::string Where = Laid.Cells.empty() ? "" : std::format("cell {} of {}", Cursor + 1, Laid.Cells.size());
    const Rect        In    = DrawPanel(Out, Shown, Grid.Info, "under the cursor", Where);
    if (In.Empty() || !Now) return;
    const std::vector<InfoLine> Lines = InfoLines();
    for (std::size_t Index = 0; Index < Lines.size() && Index < static_cast<std::size_t>(In.Height); Index++)
        Say(Out, Point{In.X + 1, In.Y + static_cast<std::int32_t>(Index)}, Lines[Index].first, In.Width - 2,
            Lines[Index].second, Shown.Panel);
}

void Board::PaintStatus(tui::Frame &Out, Rect Area) const
{
    Out.Fill(Area, ' ', {Shown.Text, Shown.Bars});
    const std::int32_t Right = Area.X + Area.Width;
    if (!Marked.empty()) SayRight(Out, Area.Y, Area.X, Right - 1, std::format("{} marked", Marked.size()), Shown.Marked, Shown.Bars);
    const std::int32_t Room = Area.Width - 2 - (Marked.empty() ? 0 : 12);
    if (!Status.empty()) {
        Say(Out, Point{Area.X + 1, Area.Y}, Status, Room, Shown.Title, Shown.Bars);
        return;
    }
    const std::string Typed = Filter.empty() ? std::string() : "[" + Filter + "]";
    const std::pair<std::string, std::string> Keys[] = {
        {"\xE2\x86\x90\xE2\x86\x91\xE2\x86\x93\xE2\x86\x92", "cursor"}, {Spelled(KeyView), "view"},
        {Spelled(KeyHelp), "keys"},  {"esc", Filter.empty() ? "leave" : "clear " + Typed}, {"space", "mark"},
        {Spelled(KeyGroup), "group"}, {Spelled(KeySort), "order"}, {Spelled(KeyPause), Paused ? "go on" : "pause"}};
    std::int32_t X = Area.X + 1;
    for (const auto &[Key, Does] : Keys) {
        const std::int32_t Needs = Cells(Key) + 1 + Cells(Does) + 2;
        if (X + Needs > Area.X + 1 + Room) break;
        X += tui::PutText(Out, Point{X, Area.Y}, Key, Needs, Shown.Title, Shown.Bars) + 1;
        X += tui::PutText(Out, Point{X, Area.Y}, Does, Needs, Shown.Quiet, Shown.Bars) + 2;
    }
}

void Board::PaintHelp(tui::Frame &Out) const
{
    const std::pair<std::string, std::string> Keys[] = {
        {"arrows", "move the cursor; the owner under it stands out"},
        {"page up, page down", "a screenful of cells"},
        {"home, end", "the first cell, the last"},
        {"space", "mark the program: the marked are given back as them"},
        {"typing", "stand out the owners whose name or tags hold it"},
        {"backspace", "take back a letter of it"},
        {Spelled(KeyView), "the other view: by owner, or by address"},
        {Spelled(KeyGroup), "each program apart, or all programs as one kind"},
        {Spelled(KeySort), "order the owners by size, age or name"},
        {Spelled(KeyPause), "hold the picture still, or let it go on"},
        {Spelled(KeyHelp), "these keys"},
        {"esc", "what is typed taken back, then leave"},
    };
    constexpr std::int32_t KeyWidth = 20;
    const painter::core::Size Size = Screen.Size();
    const Rect Area   = tui::Centred(Size, std::min(Size.Width - 2, 82), static_cast<std::int32_t>(std::size(Keys)) + 4);
    const Rect Inside = tui::DrawWindow(Out, Area, "the map's keys", "settings memmap keys", "any key closes this");
    std::int32_t Row = Inside.Y + 1;
    for (const auto &[Key, Does] : Keys) {
        if (Row >= Inside.Y + Inside.Height) break;
        Say(Out, Point{Inside.X + 2, Row}, Key, KeyWidth, tui::TitleInk, tui::WindowPaper);
        Say(Out, Point{Inside.X + 2 + KeyWidth, Row}, Does, Inside.Width - KeyWidth - 3, tui::WindowInk, tui::WindowPaper);
        Row++;
    }
}

}  // namespace memmap
