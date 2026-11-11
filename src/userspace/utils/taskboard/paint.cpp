/* paint.cpp — the board's picture: a line at the top, the load and the cores,
 * the programs beside the machine, the journal, and the line of keys; a window
 * of every key over them when asked. */

#include <algorithm>
#include <format>
#include <string>
#include <string_view>

#include "painter/tui/glyph.h"
#include "painter/tui/text.h"
#include "painter/tui/window.h"

#include "board.h"
#include "graph.h"

namespace taskboard {

namespace {

namespace tui    = painter::tui;
namespace Glyphs = painter::tui::Glyphs;
using painter::core::Point;
using painter::core::Rect;

// Neutral grays, so the 16 colors of a text screen keep them black and gray:
// a color whose blue leads becomes blue there.
constexpr Color Page        = Color::Rgb(0x0F, 0x0F, 0x0F);   // behind the panels
constexpr Color Paper       = Color::Rgb(0x15, 0x15, 0x15);   // inside a panel
constexpr Color BarPaper    = Color::Rgb(0x1E, 0x1E, 0x1E);   // the lines at the top and the bottom
constexpr Color Rule        = Color::Rgb(0x46, 0x46, 0x46);   // a panel's frame
constexpr Color TitleInk    = Color::Rgb(0xEE, 0xEE, 0xEE);
constexpr Color Ink         = Color::Rgb(0xC8, 0xC8, 0xC8);
constexpr Color Quiet       = Color::Rgb(0x7C, 0x7C, 0x7C);
constexpr Color ChosenPaper = Color::Rgb(0x24, 0x3B, 0x5A);   // the chosen row
constexpr Color MarkInk     = Color::Rgb(0xFF, 0xC8, 0x57);
constexpr char  Opened      = static_cast<char>(0x1F);       // a triangle pointing down
// Between the words of a line: a middle dot, as text (the font folds it).
constexpr std::string_view Between = "\xC2\xB7";

// The least of each part of the picture; below them a part is left out.
constexpr std::int32_t LeastList    = 6;
constexpr std::int32_t LeastJournal = 4;
constexpr std::int32_t MeterCells   = 8;

Color LivingInk(Living Where)
{
    switch (Where) {
    case Living::OnScreen: return Color::Rgb(0x5F, 0xD7, 0xFF);
    case Living::Behind:   return Color::Rgb(0xB3, 0x9D, 0xFF);
    case Living::Keeper:   return Color::Rgb(0xFF, 0xD1, 0x66);
    case Living::OnItsOwn: return Color::Rgb(0x8B, 0xE3, 0x8B);
    case Living::Serving:  return Color::Rgb(0x8F, 0xA3, 0xB8);
    case Living::Task:     break;
    }
    return Ink;
}

Color HappenedInk(Happened What)
{
    switch (What) {
    case Happened::Began:  return Color::Rgb(0x8B, 0xE3, 0x8B);
    case Happened::Ended:  return Color::Rgb(0x8F, 0xA3, 0xB8);
    case Happened::Fell:   return Color::Rgb(0xFF, 0x6B, 0x6B);
    case Happened::Raised: return Color::Rgb(0xFF, 0xD1, 0x66);
    case Happened::Board:  break;
    }
    return Color::Rgb(0x5F, 0xD7, 0xFF);
}

const char *StateName(std::uint32_t State)
{
    switch (State) {
    case 0: return "new";
    case 1: return "working";
    case 2: return "waiting";
    case 3: return "stopped";
    case 4: return "done";
    case 5: return "fell";
    default: return "?";
    }
}

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

std::string Lasted(std::uint64_t Us)
{
    const std::uint64_t Seconds = Us / 1000000u, Minutes = Seconds / 60u, Hours = Minutes / 60u, Days = Hours / 24u;
    if (Seconds < 60) return std::format("{}s", Seconds);
    if (Minutes < 60) return std::format("{}m{:02}s", Minutes, Seconds % 60u);
    if (Hours < 24) return std::format("{}h{:02}m", Hours, Minutes % 60u);
    return std::format("{}d{:02}h", Days, Hours % 24u);
}

std::string Percent(double Share)
{
    return std::format("{}%", static_cast<long>(Share * 100.0 + 0.5));
}

std::int32_t Cells(std::string_view Text)
{
    return static_cast<std::int32_t>(tui::CellsOf(Text));
}

void Say(tui::Frame &Out, Point At, std::string_view Text, std::int32_t Width, Color With, Color Under)
{
    if (Width > 0) tui::PutText(Out, At, Text, Width, With, Under);
}

// Text ending at Right, the cell after its last; nothing left of Left.
void SayRight(tui::Frame &Out, std::int32_t Row, std::int32_t Left, std::int32_t Right, std::string_view Text,
              Color With, Color Under)
{
    const std::int32_t Width = std::min(Cells(Text), Right - Left);
    Say(Out, Point{Right - Width, Row}, tui::Shortened(Text, static_cast<std::size_t>(Width)), Width, With, Under);
}

// A panel over Area: its paper, a frame of rules, Title in the top edge at the
// left and Note at its right; what is inside the frame is returned.
Rect DrawPanel(tui::Frame &Out, Rect Area, std::string_view Title, std::string_view Note)
{
    Out.Fill(Area, ' ', {Ink, Paper});
    if (Area.Width < 4 || Area.Height < 3) return Rect{Area.X, Area.Y, 0, 0};
    const std::int32_t Right = Area.X + Area.Width - 1, Bottom = Area.Y + Area.Height - 1;
    for (std::int32_t X = Area.X + 1; X < Right; X++) {
        Out.Put(Point{X, Area.Y}, Glyphs::RuleAcross, {Rule, Paper});
        Out.Put(Point{X, Bottom}, Glyphs::RuleAcross, {Rule, Paper});
    }
    for (std::int32_t Y = Area.Y + 1; Y < Bottom; Y++) {
        Out.Put(Point{Area.X, Y}, Glyphs::RuleDown, {Rule, Paper});
        Out.Put(Point{Right, Y}, Glyphs::RuleDown, {Rule, Paper});
    }
    Out.Put(Point{Area.X, Area.Y}, Glyphs::CornerTL, {Rule, Paper});
    Out.Put(Point{Right, Area.Y}, Glyphs::CornerTR, {Rule, Paper});
    Out.Put(Point{Area.X, Bottom}, Glyphs::CornerBL, {Rule, Paper});
    Out.Put(Point{Right, Bottom}, Glyphs::CornerBR, {Rule, Paper});
    const std::string Named = std::format(" {} ", Title);
    Say(Out, Point{Area.X + 2, Area.Y}, Named, Area.Width - 4, TitleInk, Paper);
    if (!Note.empty())
        SayRight(Out, Area.Y, Area.X + 3 + Cells(Named), Right - 1, std::format(" {} ", Note), Quiet, Paper);
    return Rect{Area.X + 1, Area.Y + 1, Area.Width - 2, Area.Height - 2};
}

// A line across a panel's inside, joined to its frame.
void DrawSeparator(tui::Frame &Out, Rect Inside, std::int32_t Row)
{
    Out.Put(Point{Inside.X - 1, Row}, Glyphs::TeeLeft, {Rule, Paper});
    for (std::int32_t X = Inside.X; X < Inside.X + Inside.Width; X++) Out.Put(Point{X, Row}, Glyphs::RuleAcross, {Rule, Paper});
    Out.Put(Point{Inside.X + Inside.Width, Row}, Glyphs::TeeRight, {Rule, Paper});
}

const char *SortName(SortBy Sort)
{
    switch (Sort) {
    case SortBy::Memory:    return "by memory";
    case SortBy::Name:      return "by name";
    case SortBy::Age:       return "by age";
    case SortBy::Processor: break;
    }
    return "by processor";
}

}  // namespace

void Board::Paint()
{
    tui::Frame              &Out  = Screen.FrameBuffer();
    const painter::core::Size Size = Screen.Size();
    Out.Clear({Ink, Page});
    if (Size.Width <= 0 || Size.Height < 3) return;

    PaintHeader(Out, Rect{0, 0, Size.Width, 1});
    PaintStatus(Out, Rect{0, Size.Height - 1, Size.Width, 1});

    const std::int32_t Body    = Size.Height - 2;
    std::int32_t       Journal = Shown.Journal ? std::clamp(Body / 6, LeastJournal, 9) : 0;
    std::int32_t       Above   = std::clamp(Body * 30 / 100, 6, 16);
    // The programs come first: what does not leave them room is left out.
    if (Body - Above - Journal < LeastList + 4) Journal = 0;
    if (Body - Above - Journal < LeastList + 4) Above = std::max(0, Body - Journal - LeastList - 4);
    const std::int32_t Middle = Body - Above - Journal;

    if (Above >= 4) {
        const std::int32_t CoresWidth = Shown.Cores ? std::clamp(Size.Width * 36 / 100, 28, 54) : 0;
        PaintLoad(Out, Rect{0, 1, Size.Width - CoresWidth, Above});
        if (CoresWidth) PaintCores(Out, Rect{Size.Width - CoresWidth, 1, CoresWidth, Above});
    }
    const std::int32_t MachineWidth = Shown.Machine ? std::clamp(Size.Width * 26 / 100, 26, 40) : 0;
    PaintPrograms(Out, Rect{0, 1 + Above, Size.Width - MachineWidth, Middle});
    if (MachineWidth) PaintMachine(Out, Rect{Size.Width - MachineWidth, 1 + Above, MachineWidth, Middle});
    if (Journal) PaintJournal(Out, Rect{0, 1 + Above + Middle, Size.Width, Journal});
    if (Help) PaintHelp(Out);
}

void Board::PaintHeader(tui::Frame &Out, Rect Area) const
{
    // What is said here stands at the left: the right of the top line is where
    // the programs that live on their own lay their notices.
    Out.Fill(Area, ' ', {Ink, BarPaper});
    Out.Put(Point{Area.X + 1, Area.Y}, static_cast<char>(0xFE), {LoadColor(Shown, Load.Machine()), BarPaper});
    Say(Out, Point{Area.X + 3, Area.Y}, "taskboard", Area.Width - 3, TitleInk, BarPaper);
    if (!Now) return;
    std::size_t Strands = 0;
    for (const Program &One : Known.All()) Strands += One.Seen.Strands.size();
    const std::string Context = Now->Context.empty() ? std::string("no context") : "context " + Now->Context;
    const std::string Line    = std::format("{}  {}  {}  up {}  {}  {} programs, {} strands", Between, Context,
                                            Between, Lasted(Now->AtUs), Between, Known.All().size(), Strands);
    Say(Out, Point{Area.X + 13, Area.Y}, Line, Area.Width - 14, Quiet, BarPaper);
}

void Board::PaintLoad(tui::Frame &Out, Rect Area) const
{
    const std::uint32_t Seconds = Shown.HistorySeconds;
    const std::string   Span = Seconds % 60 == 0 ? std::format("{} min", Seconds / 60) : std::format("{} s", Seconds);
    const Rect Inside = DrawPanel(Out, Area, std::format("load {}", Percent(Load.Machine())), Span);
    if (Inside.Empty()) return;
    DrawGraph(Out, Inside, Load.Columns(std::nullopt, static_cast<std::size_t>(Inside.Width), Seconds), Shown, Paper);
    // The time the graph spans, in its bottom edge.
    const std::int32_t Bottom = Area.Y + Area.Height - 1;
    Say(Out, Point{Area.X + 2, Bottom}, std::format(" {} ago ", Span), Area.Width - 4, Quiet, Paper);
    SayRight(Out, Bottom, Area.X + 2, Area.X + Area.Width - 2, " now ", Quiet, Paper);
}

void Board::PaintCores(tui::Frame &Out, Rect Area) const
{
    std::size_t System = 0;
    const std::size_t Count = Now ? Now->Cores.size() : 0;
    for (std::size_t Core = 0; Core < Count; Core++) System += Now->Cores[Core].Kernel;
    const Rect Inside = DrawPanel(Out, Area, "cores", std::format("{} system {} {} programs", System, Between,
                                                                   Count - System));
    if (Inside.Empty() || Count == 0) return;
    // Each core a row — the system's first, under a heading each when there
    // is room for them; in two columns when there is not room for one.
    const bool         Headed  = static_cast<std::int32_t>(Count) + 2 <= Inside.Height;
    const std::int32_t Columns = static_cast<std::int32_t>(Count) + (Headed ? 2 : 0) <= Inside.Height ? 1 : 2;
    const std::int32_t Width   = Inside.Width / Columns;
    std::int32_t       Slot    = 0;
    const auto         Place   = [&](std::int32_t Index) {
        const std::int32_t Rows = Inside.Height;
        return Point{Inside.X + (Index / Rows) * Width, Inside.Y + Index % Rows};
    };
    for (int Kernel = 1; Kernel >= 0; Kernel--) {
        if (Headed) {
            Say(Out, Place(Slot++), Kernel ? "system" : "programs", Width, Quiet, Paper);
        }
        for (std::size_t Core = 0; Core < Count; Core++) {
            const CoreSeen &One = Now->Cores[Core];
            if (One.Kernel != (Kernel == 1)) continue;
            if (Slot >= Inside.Height * Columns) return;
            const Point                 At   = Place(Slot++);
            const std::optional<double> Used = Load.Core(Core);
            // A core whose counters do not move is not counted by the
            // scheduler — said so, and no load guessed for it.
            const bool        Resting = Used && One.Parked && *Used < 0.005;
            const std::string Name    = std::format("{}{}", One.Kernel ? 'K' : 'A', Core);
            Say(Out, At, Name, 4, Used && !Resting ? Ink : Quiet, Paper);
            const std::int32_t GraphWidth = Width - 15;
            if (GraphWidth > 0)
                DrawGraph(Out, Rect{At.X + 4, At.Y, GraphWidth, 1},
                          Load.Columns(Core, static_cast<std::size_t>(GraphWidth), Shown.HistorySeconds), Shown, Paper);
            SayRight(Out, At.Y, At.X + 4, At.X + Width - 1, !Used ? "uncounted" : Resting ? "rest" : Percent(*Used),
                     Used && !Resting ? LoadColor(Shown, *Used) : Quiet, Paper);
        }
    }
}

void Board::PaintMachine(tui::Frame &Out, Rect Area) const
{
    const Rect Inside = DrawPanel(Out, Area, "machine", "");
    if (Inside.Empty() || !Now) return;
    std::int32_t Row = Inside.Y;
    const std::int32_t Right = Inside.X + Inside.Width;
    const auto Fits = [&](std::int32_t Rows) { return Row + Rows <= Inside.Y + Inside.Height; };
    const auto Gauge = [&](std::string_view Label, std::uint64_t Used, std::uint64_t Total, std::string Said) {
        if (!Fits(2)) return;
        const double Share = Total ? static_cast<double>(Used) / static_cast<double>(Total) : 0;
        Say(Out, Point{Inside.X, Row}, Label, Inside.Width, Ink, Paper);
        SayRight(Out, Row, Inside.X + Cells(Label) + 1, Right, Said, Quiet, Paper);
        Row++;
        DrawMeter(Out, Point{Inside.X, Row}, Inside.Width - 5, Share, Shown, Paper);
        SayRight(Out, Row, Inside.X, Right, Percent(Share), LoadColor(Shown, Share), Paper);
        Row += 2;
    };
    Gauge("memory", Now->MemoryUsed, Now->MemoryTotal,
          std::format("{} of {}", Bytes(Now->MemoryUsed), Bytes(Now->MemoryTotal)));
    if (Now->VolumePresent)
        Gauge(Now->VolumeName.empty() ? "volume" : "volume " + Now->VolumeName, Now->VolumeUsed, Now->VolumeTotal,
              std::format("{} of {}", Bytes(Now->VolumeUsed), Bytes(Now->VolumeTotal)));
    else if (Fits(2)) {
        Say(Out, Point{Inside.X, Row}, "no volume", Inside.Width, Quiet, Paper);
        Row += 2;
    }
    if (Fits(1)) {
        Say(Out, Point{Inside.X, Row}, "context", Inside.Width, Ink, Paper);
        SayRight(Out, Row, Inside.X + 8, Right, Now->Context.empty() ? "none" : Now->Context,
                 Now->Context.empty() ? Quiet : LivingInk(Living::OnScreen), Paper);
        Row++;
    }
    if (Fits(1) && !Now->PlacesKnown)
        Say(Out, Point{Inside.X, Row}, "no display said where programs are", Inside.Width, Quiet, Paper);
}

void Board::PaintPrograms(tui::Frame &Out, Rect Area)
{
    const std::vector<Line> All = Lines();
    std::size_t             Programs = 0;
    for (const Line &One : All) Programs += One.What == Line::Kind::Program;
    std::string Note = std::format("{} {} {}", Programs, Between, SortName(Shown.Sort));
    if (!Filter.empty()) Note += std::format(" {} holding \"{}\"", Between, Filter);
    const Rect Inside = DrawPanel(Out, Area, "programs", Note);
    if (Inside.Height < 4) return;

    // The columns, as many as the width holds: the name takes what is left.
    const std::int32_t Width   = Inside.Width;
    const bool         Memory  = Width >= 52;
    const bool         Strands = Width >= 60;
    const bool         Age     = Width >= 68;
    const std::int32_t NameW   = std::max(4, Width - 3 - 12 - (MeterCells + 6) - (Memory ? 8 : 0) -
                                                 (Strands ? 8 : 0) - (Age ? 8 : 0));
    const std::int32_t WhereX  = Inside.X + 3 + NameW;
    const std::int32_t UseX    = WhereX + 12;
    const std::int32_t Right   = Inside.X + Width;
    const std::int32_t MemoryR = UseX + MeterCells + 6 + 8;
    const std::int32_t StrandR = MemoryR + (Memory ? 8 : 0);
    const std::int32_t AgeR    = StrandR + (Strands ? 8 : 0);

    std::int32_t Row = Inside.Y;
    Say(Out, Point{Inside.X + 3, Row}, "name", NameW, Quiet, Paper);
    // Grouped, the heading says where a program is: the column says its state.
    const bool Grouped = Shown.Group == GroupBy::Living;
    Say(Out, Point{WhereX, Row}, Grouped ? "state" : "where", 11, Quiet, Paper);
    Say(Out, Point{UseX, Row}, "processor", MeterCells + 6, Quiet, Paper);
    if (Memory) SayRight(Out, Row, UseX, MemoryR, "memory", Quiet, Paper);
    if (Strands) SayRight(Out, Row, MemoryR, StrandR, "strands", Quiet, Paper);
    if (Age) SayRight(Out, Row, StrandR, AgeR, "lived", Quiet, Paper);
    Row++;

    // The list between the heading and the chosen program's two lines below.
    const std::int32_t Visible = Inside.Height - 4;
    std::size_t        ChosenLine = 0;
    const Program     *Picked = Chosen();
    for (std::size_t Index = 0; Index < All.size(); Index++)
        if (All[Index].What == Line::Kind::Program && All[Index].Of == Picked) ChosenLine = Index;
    if (Visible > 0) {
        const std::size_t Shows = static_cast<std::size_t>(Visible);
        if (ChosenLine < Top) Top = ChosenLine > 0 && All[ChosenLine - 1].What == Line::Kind::Group ? ChosenLine - 1 : ChosenLine;
        if (ChosenLine >= Top + Shows) Top = ChosenLine + 1 - Shows;
        Top = std::min(Top, All.size() > Shows ? All.size() - Shows : 0);
    }
    const std::uint64_t NowUs = Now ? Now->AtUs : 0;
    for (std::size_t Index = Top; Index < All.size() && Row < Inside.Y + 1 + Visible; Index++, Row++) {
        const Line &One = All[Index];
        if (One.What == Line::Kind::Group) {
            for (std::int32_t X = Inside.X; X < Right; X++) Out.Put(Point{X, Row}, Glyphs::RuleAcross, {Rule, Paper});
            Say(Out, Point{Inside.X + 2, Row}, std::format(" {} ", LivingName(One.Group)), Width - 2,
                LivingInk(One.Group), Paper);
            continue;
        }
        const Program &Of       = *One.Of;
        const bool     IsChosen = One.What == Line::Kind::Program && &Of == Picked;
        const Color    Under    = IsChosen ? ChosenPaper : Paper;
        Out.Fill(Rect{Inside.X, Row, Width, 1}, ' ', {Ink, Under});
        if (One.What == Line::Kind::Strand) {
            const StrandSeen &Strand = Of.Seen.Strands[One.Strand];
            const bool        Last   = One.Strand + 1 == Of.Seen.Strands.size();
            const double      Used   = One.Strand < Of.StrandProcessor.size() ? Of.StrandProcessor[One.Strand] : 0;
            Out.Put(Point{Inside.X + 2, Row}, Last ? Glyphs::CornerBL : Glyphs::TeeLeft, {Rule, Under});
            Say(Out, Point{Inside.X + 4, Row},
                std::format("strand {}{}  {}  core {}", Strand.Pid, One.Strand == 0 ? " (main)" : "",
                            StateName(Strand.State), Strand.Core),
                WhereX + 11 - Inside.X - 4, Quiet, Under);
            DrawMeter(Out, Point{UseX, Row}, MeterCells, std::min(Used, 1.0), Shown, Under);
            SayRight(Out, Row, UseX + MeterCells, UseX + MeterCells + 6, Percent(Used), Quiet, Under);
            continue;
        }
        const bool IsMarked = std::any_of(Marked.begin(), Marked.end(), [&](const Mark &M) {
            return M.Program == Who{Of.Seen.Pid, Of.Seen.Generation};
        });
        const bool StrandsShown = Index + 1 < All.size() && All[Index + 1].What == Line::Kind::Strand;
        if (IsMarked) Out.Put(Point{Inside.X, Row}, static_cast<char>(0xFE), {MarkInk, Under});
        Out.Put(Point{Inside.X + 1, Row}, StrandsShown ? Opened : Glyphs::Pointer,
                {Of.Seen.Strands.size() > 1 ? Ink : Quiet, Under});
        Say(Out, Point{Inside.X + 3, Row}, tui::Shortened(Of.Name, static_cast<std::size_t>(NameW - 1)), NameW - 1,
            IsChosen ? TitleInk : Ink, Under);
        if (Grouped) Say(Out, Point{WhereX, Row}, Of.Seen.Strands.empty() ? "?" : StateName(Of.Seen.Strands[0].State),
                         11, Quiet, Under);
        else Say(Out, Point{WhereX, Row}, LivingName(Of.Where), 11, LivingInk(Of.Where), Under);
        DrawMeter(Out, Point{UseX, Row}, MeterCells, std::min(Of.Processor, 1.0), Shown, Under);
        SayRight(Out, Row, UseX + MeterCells, UseX + MeterCells + 6, Percent(Of.Processor),
                 LoadColor(Shown, std::min(Of.Processor, 1.0)), Under);
        if (Memory) SayRight(Out, Row, UseX, MemoryR, Bytes(Of.Seen.MemoryBytes), Ink, Under);
        if (Strands) SayRight(Out, Row, MemoryR, StrandR, std::format("{}", Of.Seen.Strands.size()), Ink, Under);
        if (Age && NowUs > Of.Seen.BeganUs) SayRight(Out, Row, StrandR, AgeR, Lasted(NowUs - Of.Seen.BeganUs), Quiet, Under);
    }
    if (All.empty())
        Say(Out, Point{Inside.X + 3, Inside.Y + 1}, Filter.empty() ? "no program is read yet" : "no program holds that",
            Width - 3, Quiet, Paper);
    DrawSeparator(Out, Inside, Inside.Y + Inside.Height - 3);
    PaintChosen(Out, Rect{Inside.X, Inside.Y + Inside.Height - 2, Width, 2});
}

void Board::PaintChosen(tui::Frame &Out, Rect Area) const
{
    const Program *One = Chosen();
    if (!One) return;
    const Program *From = One->Seen.SpawnerPid ? Known.OwnerOf(One->Seen.SpawnerPid, One->Seen.SpawnerGeneration) : nullptr;
    std::string    Line = std::format("{}  #{}  {}  ", One->Name, One->Seen.Pid, LivingName(One->Where));
    Line += One->Seen.SpawnerPid == 0 ? "started by the system" : From ? "started from " + From->Name : "started";
    if (Now && Now->AtUs > One->Seen.BeganUs) Line += std::format(", {} ago", Lasted(Now->AtUs - One->Seen.BeganUs));
    if (One->Seen.Raised) Line += std::format("  raised again {} times", One->Seen.Raised);
    Say(Out, Point{Area.X + 1, Area.Y}, tui::Shortened(Line, static_cast<std::size_t>(Area.Width - 2)), Area.Width - 2,
        TitleInk, Paper);
    Say(Out, Point{Area.X + 1, Area.Y + 1},
        tui::Shortened("tags " + One->Seen.Tags, static_cast<std::size_t>(Area.Width - 2)), Area.Width - 2, Quiet, Paper);
}

void Board::PaintJournal(tui::Frame &Out, Rect Area) const
{
    const std::deque<Entry> &Entries = Said.Entries();
    const Rect Inside = DrawPanel(Out, Area, "journal", Said.Hears() ? std::format("{}", Entries.size()) : "not heard");
    if (Inside.Empty()) return;
    const std::size_t Rows  = static_cast<std::size_t>(Inside.Height);
    const std::size_t First = Entries.size() > Rows ? Entries.size() - Rows : 0;
    std::int32_t      Row   = Inside.Y;
    for (std::size_t Index = First; Index < Entries.size(); Index++, Row++) {
        const Entry &One = Entries[Index];
        Say(Out, Point{Inside.X + 1, Row}, One.At, 9, Quiet, Paper);
        Say(Out, Point{Inside.X + 11, Row}, tui::Shortened(One.Text, static_cast<std::size_t>(Inside.Width - 12)),
            Inside.Width - 12, HappenedInk(One.What), Paper);
    }
    if (Entries.empty())
        Say(Out, Point{Inside.X + 1, Inside.Y}, "nothing has happened since the board began", Inside.Width - 1, Quiet,
            Paper);
}

void Board::PaintStatus(tui::Frame &Out, Rect Area) const
{
    Out.Fill(Area, ' ', {Ink, BarPaper});
    const std::int32_t Right = Area.X + Area.Width;
    if (!Marked.empty()) SayRight(Out, Area.Y, Area.X, Right - 1, std::format("{} marked", Marked.size()), MarkInk, BarPaper);
    const std::int32_t Room = Area.Width - 2 - (Marked.empty() ? 0 : 12);
    if (!Status.empty()) {
        Say(Out, Point{Area.X + 1, Area.Y}, tui::Shortened(Status, static_cast<std::size_t>(Room)), Room, TitleInk,
            BarPaper);
        return;
    }
    // The keys, each in the color of a key and what it does quietly after it.
    const std::pair<std::string, std::string> Keys[] = {
        {"\xE2\x86\x91\xE2\x86\x93", "choose"},      {"enter", "strands"},         {"space", "mark"},
        {Spelled(KeyEnd), "end"},    {Spelled(KeySort), "order"},  {Spelled(KeyGroup), "group"},
        {Spelled(KeyHelp), "keys"},  {"esc", Filter.empty() ? "leave" : "clear"}};
    std::int32_t X = Area.X + 1;
    for (const auto &[Key, Does] : Keys) {
        const std::int32_t Needs = Cells(Key) + 1 + Cells(Does) + 2;
        if (X + Needs > Area.X + 1 + Room) break;
        X += static_cast<std::int32_t>(tui::PutText(Out, Point{X, Area.Y}, Key, Needs, TitleInk, BarPaper)) + 1;
        X += static_cast<std::int32_t>(tui::PutText(Out, Point{X, Area.Y}, Does, Needs, Quiet, BarPaper)) + 2;
    }
}

void Board::PaintHelp(tui::Frame &Out) const
{
    const std::pair<std::string, std::string> Keys[] = {
        {"up, down", "choose a program"},
        {"page up, page down", "ten programs at a time"},
        {"home, end", "the first, the last"},
        {"enter", "its strands, shown or hidden"},
        {"space", "mark it: the files of the marked are given back as them"},
        {"typing", "keep those whose name or tags hold what is typed"},
        {"backspace", "take back a letter of it"},
        {Spelled(KeyEnd), "end it: asked to finish, then ended at once"},
        {Spelled(KeySort), "order by processor, memory, name or age"},
        {Spelled(KeyGroup), "grouped by where they live, or not"},
        {Spelled(KeyHelp), "these keys"},
        {"esc", "what is typed taken back, then leave"},
    };
    constexpr std::int32_t KeyWidth = 20;
    const painter::core::Size Size = Screen.Size();
    const Rect Area   = tui::Centred(Size, std::min(Size.Width - 2, 82), static_cast<std::int32_t>(std::size(Keys)) + 4);
    const Rect Inside = tui::DrawWindow(Out, Area, "the board's keys", "settings taskboard keys", "any key closes this");
    std::int32_t Row = Inside.Y + 1;
    for (const auto &[Key, Does] : Keys) {
        if (Row >= Inside.Y + Inside.Height) break;
        Say(Out, Point{Inside.X + 2, Row}, Key, KeyWidth, tui::TitleInk, tui::WindowPaper);
        Say(Out, Point{Inside.X + 2 + KeyWidth, Row}, Does, Inside.Width - KeyWidth - 3, tui::WindowInk, tui::WindowPaper);
        Row++;
    }
}

}  // namespace taskboard
