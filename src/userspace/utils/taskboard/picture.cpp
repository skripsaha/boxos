/* picture.cpp — the board's picture on a framebuffer, described a frame at a
 * time (painter::gui): the parts of the cells' picture (paint.cpp) in the
 * same places — a line at the top, the load and the cores, the programs
 * beside the machine, the journal, the line of keys, and the keys' window
 * when asked — laid on the grid of the code face's characters and lines, and
 * drawn as panels, bars, graphs and text. Each part is a group of its own and
 * each line of the list a group keyed by what it shows, so a frame sends only
 * what changed. */

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "painter/core/path.h"
#include "painter/gui/measure.h"
#include "painter/tui/text.h"

#include "board.h"
#include "paint_words.h"

namespace taskboard {

namespace {

namespace gui = painter::gui;
using painter::core::PointF;

// The top line's lead over the grid's line, in points: the notices of the
// programs that live on their own lie in its right, a little taller than a
// line (apps/panel.h), and the panels begin below them.
constexpr float Lead = 12;

// The least of each part of the picture, in lines; below them a part is left out.
constexpr std::int32_t LeastList    = 6;
constexpr std::int32_t LeastJournal = 4;
constexpr std::int32_t MeterCells   = 8;

/* The grid the board is laid on: a character of the code face across, a
 * line of it down, in points; a place on it in characters and lines. */
struct Grid {
    float Across = 8, Down = 18;
    float X(float Col) const { return Col * Across; }
    float Y(float Row) const { return Row * Down; }
};

/* A part's room on the grid. */
struct Room {
    std::int32_t Col = 0, Row = 0, Cols = 0, Rows = 0;
    bool         Empty() const { return Cols <= 0 || Rows <= 0; }
};

gui::TextStyle Ink(Color Of)
{
    gui::TextStyle Made;
    Made.Color = Of;
    return Made;
}

/* Words from a place on the grid in a colour; Width characters at most, cut
 * with an ellipsis, laid as Align says. */
void Say(gui::Group &In, const Grid &On, float Col, float Row, std::string_view Words, Color With, float Width = 0,
         std::uint8_t Align = ITEM_TEXT_LEFT)
{
    gui::Text One = In.Text(Words, gui::Role::Code);
    One.SetPlace({On.X(Col), On.Y(Row)}).SetStyle(Ink(With));
    if (Width <= 0) return;
    gui::TextBox Box;
    Box.Align    = Align;
    Box.Overflow = ITEM_OVERFLOW_ELLIPSIS;
    Box.Wrap     = false;
    Box.MaxLines = 1;
    One.SetSize({On.X(Width), gui::ByContent}).SetBox(Box);
}

/* A plain rectangle on the grid's points. */
gui::Shape Block(gui::Group &In, PointF At, PointF Size, Color With, float Round = 0, float Opacity = 1)
{
    gui::Shape One = In.Rectangle();
    One.SetPlace(At).SetSize(Size).SetFill(gui::Fill::Of(With));
    if (Round > 0) One.SetCorners(painter::core::Corners::All(Round));
    if (Opacity < 1) One.SetOpacity(Opacity);
    return One;
}

/* A panel over Area: its paper, rounded, framed; Title at its top left and
 * Note at its top right. The room inside it. */
Room Panel(gui::Group &In, const Grid &On, const Look &Shown, Room Area, std::string_view Title, std::string_view Note)
{
    gui::Shape  Paper = Block(In, {On.X(Area.Col) + 2, On.Y(Area.Row) + 2}, {On.X(Area.Cols) - 4, On.Y(Area.Rows) - 4}, Shown.Panel, 8);
    gui::Stroke Edge;
    Edge.Width = 1;
    Edge.With  = gui::Fill::Of(Shown.Frame);
    Paper.SetStroke(Edge);
    if (Area.Cols < 4 || Area.Rows < 3) return Room{Area.Col, Area.Row, 0, 0};
    Say(In, On, static_cast<float>(Area.Col) + 1.5f, static_cast<float>(Area.Row) + 0.08f, Title, Shown.Title);
    if (!Note.empty())
        Say(In, On, static_cast<float>(Area.Col) + 3.0f + static_cast<float>(Title.size()), static_cast<float>(Area.Row) + 0.08f, Note,
            Shown.Quiet, static_cast<float>(Area.Cols) - 4.5f - static_cast<float>(Title.size()), ITEM_TEXT_RIGHT);
    return Room{Area.Col + 1, Area.Row + 1, Area.Cols - 2, Area.Rows - 2};
}

/* Share, 0 to 1, as a bar Width characters long: lit as far as the share, in
 * the colour of its load, on a faint track. */
void Meter(gui::Group &In, const Grid &On, const Look &Shown, float Col, float Row, float Width, double Share)
{
    const PointF At{On.X(Col), On.Y(Row) + On.Down * 0.32f};
    const float  Long = On.X(Width), Thick = On.Down * 0.36f;
    (void)Block(In, At, {Long, Thick}, Shown.Frame, Thick / 2, 0.6f);
    const float Lit = Long * static_cast<float>(std::clamp(Share, 0.0, 1.0));
    gui::Shape  Bar = Block(In, At, {std::max(Lit, Thick), Thick}, LoadColor(Shown, Share), Thick / 2);
    if (Lit <= 0.5f) Bar.Hide();
}

/* Columns of load, 0 to 1, as a graph over Area: the newest at the right, the
 * ground under the line coloured from the person's cool at the floor to hot at
 * the top, the line over it; faint lines at a quarter, a half and three
 * quarters when Lines and the room is tall enough. */
void Graph(gui::Group &In, const Grid &On, const Look &Shown, Room Area, const std::vector<std::optional<float>> &Columns, bool Lines)
{
    const PointF At{On.X(Area.Col), On.Y(Area.Row)};
    const float  Wide = On.X(Area.Cols), High = On.Y(Area.Rows) - 2;
    if (Lines && Area.Rows >= 3)
        for (const float Share : {0.25f, 0.5f, 0.75f}) (void)Block(In, {At.X, At.Y + High * (1 - Share)}, {Wide, 1}, Shown.Frame, 0, 0.5f);
    painter::core::Path Ground, Line;
    const std::size_t   Count = Columns.size();
    bool                Open  = false;
    float               Last  = 0;
    for (std::size_t I = 0; I < Count; I++) {
        const float X = (static_cast<float>(I) + 0.5f) * Wide / static_cast<float>(std::max<std::size_t>(Count, 1));
        if (!Columns[I]) {
            if (Open) Ground.LineTo({Last, High}), Ground.Close();
            Open = false;
            continue;
        }
        const float Y = High * (1 - std::clamp(*Columns[I], 0.0f, 1.0f));
        if (!Open) {
            Ground.MoveTo({X, High}), Ground.LineTo({X, Y});
            Line.MoveTo({X, Y});
            Open = true;
        } else {
            Ground.LineTo({X, Y});
            Line.LineTo({X, Y});
        }
        Last = X;
    }
    if (Open) Ground.LineTo({Last, High}), Ground.Close();
    gui::Fill Heat;
    Heat.What  = gui::Fill::Kind::Linear;
    Heat.From  = {0, 0};
    Heat.To    = {0, High};
    Heat.Stops = {gui::Stop{0, Shown.Hot}, gui::Stop{0.5f, Shown.Warm}, gui::Stop{1, Shown.Cool}};
    gui::Fill Faint = Heat;
    Faint.Opacity   = 0.35f;
    In.Outline(Ground).SetPlace(At).SetSize({Wide, High}).SetFill(Faint);
    gui::Stroke Edge;
    Edge.Width = 1.5f;
    Edge.Join  = ITEM_JOIN_ROUND;
    Edge.Cap   = ITEM_CAP_ROUND;
    Edge.With  = Heat;
    In.Outline(Line).SetPlace(At).SetSize({Wide, High}).SetFills({}).SetStroke(Edge);
}

}  // namespace

void Board::Describe(gui::Frame &Out)
{
    const painter::core::PointF Letter = gui::Measure(*ItemScreen, "0", gui::Role::Code);
    const Grid                  On{std::max(1.0f, Letter.X), std::max(1.0f, Letter.Y)};
    const PointF                Size = ItemScreen->Size();
    const std::int32_t          Cols = static_cast<std::int32_t>(Size.X / On.Across);
    const std::int32_t          Rows = static_cast<std::int32_t>((Size.Y - Lead) / On.Down);

    /* The page, and the lines at the top and the bottom; everything below the
     * top line is laid a lead lower (the groups' places). */
    gui::Group Page = Out.Group();
    Page.Key(1);
    (void)Block(Page, {0, 0}, Size, Shown.Page);
    (void)Block(Page, {0, 0}, {Size.X, On.Y(1) + Lead}, Shown.Bars);
    (void)Block(Page, {0, Lead + On.Y(static_cast<float>(Rows - 1))}, {Size.X, Size.Y - Lead - On.Y(static_cast<float>(Rows - 1))}, Shown.Bars);
    if (Cols <= 0 || Rows < 3) return;
    const PointF Below{0, Lead};

    /* The top line: what is said here stands at the left — the right is where
     * the programs that live on their own lay their notices. */
    gui::Group TopLine = Out.Group();
    TopLine.Key(2).SetPlace({0, Lead / 2});
    (void)Block(TopLine, {On.X(1), On.Y(0) + On.Down * 0.25f}, {On.Down * 0.5f, On.Down * 0.5f}, LoadColor(Shown, Load.Machine()), 2);
    Say(TopLine, On, 3, 0, "taskboard", Shown.Title);
    if (Now) {
        std::size_t Strands = 0;
        for (const Program &One : Known.All()) Strands += One.Seen.Strands.size();
        const std::string Context = Now->Context.empty() ? std::string("no context") : "context " + Now->Context;
        Say(TopLine, On, 13, 0,
            std::format("{}  {}  {}  up {}  {}  {} programs, {} strands", Between, Context, Between, Lasted(Now->AtUs), Between,
                        Known.All().size(), Strands),
            Shown.Quiet, static_cast<float>(Cols - 14));
    }

    /* The bottom line: the board's latest word, or its keys; how many are
     * marked at the right. */
    gui::Group Bottom = Out.Group();
    Bottom.Key(3).SetPlace(Below);
    const float Last = static_cast<float>(Rows - 1);
    if (!Marked.empty())
        Say(Bottom, On, 1, Last, std::format("{} marked", Marked.size()), Shown.Marked, static_cast<float>(Cols - 2), ITEM_TEXT_RIGHT);
    const std::int32_t Words = Cols - 2 - (Marked.empty() ? 0 : 12);
    if (!Status.empty()) {
        Say(Bottom, On, 1, Last, Status, Shown.Title, static_cast<float>(Words));
    } else {
        const std::pair<std::string, std::string> Hint[] = {
            {"\xE2\x86\x91\xE2\x86\x93", "choose"}, {"enter", "strands"},         {"space", "mark"},
            {Spelled(KeyEnd), "end"},                {Spelled(KeySort), "order"},  {Spelled(KeyGroup), "group"},
            {Spelled(KeyHelp), "keys"},              {"esc", Filter.empty() ? "leave" : "clear"}};
        float X = 1;
        for (const auto &[Key, Does] : Hint) {
            const float Needs = static_cast<float>(painter::tui::CellsOf(Key) + 1 + painter::tui::CellsOf(Does) + 2);
            if (X + Needs > 1 + static_cast<float>(Words)) break;
            Say(Bottom, On, X, Last, Key, Shown.Title);
            X += static_cast<float>(painter::tui::CellsOf(Key)) + 1;
            Say(Bottom, On, X, Last, Does, Shown.Quiet);
            X += static_cast<float>(painter::tui::CellsOf(Does)) + 2;
        }
    }

    /* The body, as the cells' picture shares it out. */
    const std::int32_t Body    = Rows - 2;
    std::int32_t       Journal = Shown.Journal ? std::clamp(Body / 6, LeastJournal, 9) : 0;
    std::int32_t       Above   = std::clamp(Body * 30 / 100, 6, 16);
    if (Body - Above - Journal < LeastList + 4) Journal = 0;
    if (Body - Above - Journal < LeastList + 4) Above = std::max(0, Body - Journal - LeastList - 4);
    const std::int32_t Middle = Body - Above - Journal;

    if (Above >= 4) {
        const std::int32_t CoresWidth = Shown.Cores ? std::clamp(Cols * 36 / 100, 28, 54) : 0;
        /* The load over the time the graphs span. */
        gui::Group Loads = Out.Group();
        Loads.Key(4).SetPlace(Below);
        const std::uint32_t Seconds = Shown.HistorySeconds;
        const std::string   Span    = Seconds % 60 == 0 ? std::format("{} min", Seconds / 60) : std::format("{} s", Seconds);
        const Room          Inside  = Panel(Loads, On, Shown, Room{0, 1, Cols - CoresWidth, Above},
                                            std::format("load {}", Percent(Load.Machine())), Span);
        if (!Inside.Empty()) {
            const Room Chart{Inside.Col, Inside.Row, Inside.Cols, Inside.Rows - 1};
            Graph(Loads, On, Shown, Chart, Load.Columns(std::nullopt, static_cast<std::size_t>(std::max(2.0f, On.X(Chart.Cols) / 3)), Seconds),
                  true);
            Say(Loads, On, static_cast<float>(Inside.Col), static_cast<float>(Inside.Row + Inside.Rows - 1), std::format("{} ago", Span),
                Shown.Quiet);
            Say(Loads, On, static_cast<float>(Inside.Col), static_cast<float>(Inside.Row + Inside.Rows - 1), "now", Shown.Quiet,
                static_cast<float>(Inside.Cols), ITEM_TEXT_RIGHT);
        }
        if (CoresWidth) {
            /* Each core a row — the system's first, under a heading each when
             * there is room — its load graphed and said. */
            gui::Group        Cores = Out.Group();
            Cores.Key(5).SetPlace(Below);
            std::size_t       System = 0;
            const std::size_t Count  = Now ? Now->Cores.size() : 0;
            for (std::size_t Core = 0; Core < Count; Core++) System += Now->Cores[Core].Kernel;
            const Room In = Panel(Cores, On, Shown, Room{Cols - CoresWidth, 1, CoresWidth, Above}, "cores",
                                  std::format("{} system {} {} programs", System, Between, Count - System));
            if (!In.Empty() && Count) {
                const bool         Headed  = static_cast<std::int32_t>(Count) + 2 <= In.Rows;
                const std::int32_t Columns = static_cast<std::int32_t>(Count) + (Headed ? 2 : 0) <= In.Rows ? 1 : 2;
                const std::int32_t Width   = In.Cols / Columns;
                std::int32_t       Slot    = 0;
                const auto         Place   = [&](std::int32_t Index) {
                    return std::pair{In.Col + (Index / In.Rows) * Width, In.Row + Index % In.Rows};
                };
                for (int Kernel = 1; Kernel >= 0; Kernel--) {
                    if (Headed) {
                        const auto [C, R] = Place(Slot++);
                        Say(Cores, On, static_cast<float>(C), static_cast<float>(R), Kernel ? "system" : "programs", Shown.Quiet);
                    }
                    for (std::size_t Core = 0; Core < Count; Core++) {
                        const CoreSeen &One = Now->Cores[Core];
                        if (One.Kernel != (Kernel == 1) || Slot >= In.Rows * Columns) continue;
                        const auto [C, R]                = Place(Slot++);
                        const std::optional<double> Used = Load.Core(Core);
                        const bool                  Resting = Used && One.Parked && *Used < 0.005;
                        Say(Cores, On, static_cast<float>(C), static_cast<float>(R), std::format("{}{}", One.Kernel ? 'K' : 'A', Core),
                            Used && !Resting ? Shown.Text : Shown.Quiet);
                        const std::int32_t GraphWidth = Width - 15;
                        if (GraphWidth > 0)
                            Graph(Cores, On, Shown, Room{C + 4, R, GraphWidth, 1},
                                  Load.Columns(Core, static_cast<std::size_t>(std::max(2.0f, On.X(GraphWidth) / 3)), Shown.HistorySeconds), false);
                        Say(Cores, On, static_cast<float>(C + 4), static_cast<float>(R), !Used ? "uncounted" : Resting ? "rest" : Percent(*Used),
                            Used && !Resting ? LoadColor(Shown, *Used) : Shown.Quiet, static_cast<float>(Width - 5), ITEM_TEXT_RIGHT);
                    }
                }
            }
        }
    }

    /* The programs, beside the machine. */
    const std::int32_t MachineWidth = Shown.Machine ? std::clamp(Cols * 26 / 100, 26, 40) : 0;
    {
        gui::Group              List = Out.Group();
        List.Key(6).SetPlace(Below);
        const std::vector<Line> All = Lines();
        std::size_t             Programs = 0;
        for (const Line &One : All) Programs += One.What == Line::Kind::Program;
        std::string Note = std::format("{} {} {}", Programs, Between, SortName(Shown.Sort));
        if (!Filter.empty()) Note += std::format(" {} holding \"{}\"", Between, Filter);
        const Room Inside = Panel(List, On, Shown, Room{0, 1 + Above, Cols - MachineWidth, Middle}, "programs", Note);
        if (Inside.Rows >= 4) {
            const std::int32_t Width   = Inside.Cols;
            const bool         Memory  = Width >= 52;
            const bool         Strands = Width >= 60;
            const bool         Age     = Width >= 68;
            const std::int32_t NameW   = std::max(4, Width - 3 - 12 - (MeterCells + 6) - (Memory ? 8 : 0) - (Strands ? 8 : 0) - (Age ? 8 : 0));
            const std::int32_t WhereX  = 3 + NameW;
            const std::int32_t UseX    = WhereX + 12;
            const std::int32_t MemoryR = UseX + MeterCells + 6 + 8;
            const std::int32_t StrandR = MemoryR + (Memory ? 8 : 0);
            const std::int32_t AgeR    = StrandR + (Strands ? 8 : 0);
            const bool         Grouped = Shown.Group == GroupBy::Living;
            const float        Head    = static_cast<float>(Inside.Row);
            const float        X0      = static_cast<float>(Inside.Col);
            Say(List, On, X0 + 3, Head, "name", Shown.Quiet);
            Say(List, On, X0 + static_cast<float>(WhereX), Head, Grouped ? "state" : "where", Shown.Quiet);
            Say(List, On, X0 + static_cast<float>(UseX), Head, "processor", Shown.Quiet);
            if (Memory) Say(List, On, X0 + static_cast<float>(UseX), Head, "memory", Shown.Quiet, static_cast<float>(MemoryR - UseX), ITEM_TEXT_RIGHT);
            if (Strands) Say(List, On, X0 + static_cast<float>(MemoryR), Head, "strands", Shown.Quiet, 8, ITEM_TEXT_RIGHT);
            if (Age) Say(List, On, X0 + static_cast<float>(StrandR), Head, "lived", Shown.Quiet, 8, ITEM_TEXT_RIGHT);

            /* The lines that fit between the heading and the chosen
             * program's two lines below, the chosen one among them. */
            const std::int32_t Visible    = Inside.Rows - 4;
            std::size_t        ChosenLine = 0;
            const Program     *Picked     = Chosen();
            for (std::size_t Index = 0; Index < All.size(); Index++)
                if (All[Index].What == Line::Kind::Program && All[Index].Of == Picked) ChosenLine = Index;
            if (Visible > 0) {
                const std::size_t Shows = static_cast<std::size_t>(Visible);
                if (ChosenLine < Top) Top = ChosenLine > 0 && All[ChosenLine - 1].What == Line::Kind::Group ? ChosenLine - 1 : ChosenLine;
                if (ChosenLine >= Top + Shows) Top = ChosenLine + 1 - Shows;
                Top = std::min(Top, All.size() > Shows ? All.size() - Shows : 0);
            }
            const std::uint64_t NowUs = Now ? Now->AtUs : 0;
            std::int32_t        Row   = Inside.Row + 1;
            for (std::size_t Index = Top; Index < All.size() && Row < Inside.Row + 1 + Visible; Index++, Row++) {
                const Line &One = All[Index];
                /* Each line a group of its own, keyed by what it shows: a
                 * line that moves is the same line moved. */
                gui::Group  Item = List.Subgroup();
                Item.SetPlace({On.X(X0), On.Y(static_cast<float>(Row))});
                if (One.What == Line::Kind::Group) {
                    Item.Key((std::uint64_t{1} << 62) | static_cast<std::uint64_t>(One.Group));
                    (void)Block(Item, {0, On.Down * 0.5f}, {On.X(static_cast<float>(Width)), 1}, Shown.Frame);
                    (void)Block(Item, {On.X(1.5f), 0}, {On.X(static_cast<float>(std::string_view(LivingName(One.Group)).size()) + 1), On.Down}, Shown.Panel);
                    Say(Item, On, 2, 0, LivingName(One.Group), LivingInk(Shown, One.Group));
                    continue;
                }
                const Program &Of       = *One.Of;
                const bool     IsChosen = One.What == Line::Kind::Program && &Of == Picked;
                if (One.What == Line::Kind::Strand) {
                    const StrandSeen &Strand = Of.Seen.Strands[One.Strand];
                    const double      Used   = One.Strand < Of.StrandProcessor.size() ? Of.StrandProcessor[One.Strand] : 0;
                    Item.Key((std::uint64_t{1} << 63) | Strand.Pid);
                    Say(Item, On, 2, 0, One.Strand + 1 == Of.Seen.Strands.size() ? "\xE2\x94\x94" : "\xE2\x94\x9C", Shown.Frame);
                    Say(Item, On, 4, 0,
                        std::format("strand {}{}  {}  core {}", Strand.Pid, One.Strand == 0 ? " (main)" : "", StateName(Strand.State), Strand.Core),
                        Shown.Quiet, static_cast<float>(WhereX + 11 - 4));
                    Meter(Item, On, Shown, static_cast<float>(UseX), 0, MeterCells, std::min(Used, 1.0));
                    Say(Item, On, static_cast<float>(UseX + MeterCells), 0, Percent(Used), Shown.Quiet, 6, ITEM_TEXT_RIGHT);
                    continue;
                }
                Item.Key((static_cast<std::uint64_t>(Of.Seen.Pid) << 32) | Of.Seen.Generation);
                if (IsChosen) (void)Block(Item, {0, 0}, {On.X(static_cast<float>(Width)), On.Down}, Shown.Chosen, 4);
                const bool IsMarked = std::any_of(Marked.begin(), Marked.end(),
                                                  [&](const Mark &M) { return M.Program == Who{Of.Seen.Pid, Of.Seen.Generation}; });
                const bool StrandsShown = Index + 1 < All.size() && All[Index + 1].What == Line::Kind::Strand;
                if (IsMarked) (void)Block(Item, {On.X(0.2f), On.Down * 0.3f}, {On.Down * 0.4f, On.Down * 0.4f}, Shown.Marked, 2);
                Say(Item, On, 1, 0, StrandsShown ? "\xE2\x96\xBC" : "\xE2\x96\xBA", Of.Seen.Strands.size() > 1 ? Shown.Text : Shown.Quiet);
                Say(Item, On, 3, 0, Of.Name, IsChosen ? Shown.Title : Shown.Text, static_cast<float>(NameW - 1));
                if (Grouped) Say(Item, On, static_cast<float>(WhereX), 0, Of.Seen.Strands.empty() ? "?" : StateName(Of.Seen.Strands[0].State), Shown.Quiet, 11);
                else Say(Item, On, static_cast<float>(WhereX), 0, LivingName(Of.Where), LivingInk(Shown, Of.Where), 11);
                Meter(Item, On, Shown, static_cast<float>(UseX), 0, MeterCells, std::min(Of.Processor, 1.0));
                Say(Item, On, static_cast<float>(UseX + MeterCells), 0, Percent(Of.Processor), LoadColor(Shown, std::min(Of.Processor, 1.0)), 6,
                    ITEM_TEXT_RIGHT);
                if (Memory) Say(Item, On, static_cast<float>(UseX), 0, Bytes(Of.Seen.MemoryBytes), Shown.Text, static_cast<float>(MemoryR - UseX), ITEM_TEXT_RIGHT);
                if (Strands) Say(Item, On, static_cast<float>(MemoryR), 0, std::format("{}", Of.Seen.Strands.size()), Shown.Text, 8, ITEM_TEXT_RIGHT);
                if (Age && NowUs > Of.Seen.BeganUs)
                    Say(Item, On, static_cast<float>(StrandR), 0, Lasted(NowUs - Of.Seen.BeganUs), Shown.Quiet, static_cast<float>(AgeR - StrandR), ITEM_TEXT_RIGHT);
            }
            if (All.empty())
                Say(List, On, X0 + 3, Head + 1, Filter.empty() ? "no program is read yet" : "no program holds that", Shown.Quiet);

            /* The chosen program, under a line across. */
            const float Under = static_cast<float>(Inside.Row + Inside.Rows - 3);
            (void)Block(List, {On.X(X0), On.Y(Under) + On.Down * 0.5f}, {On.X(static_cast<float>(Width)), 1}, Shown.Frame);
            if (const Program *One = Chosen()) {
                const Program *From = One->Seen.SpawnerPid ? Known.OwnerOf(One->Seen.SpawnerPid, One->Seen.SpawnerGeneration) : nullptr;
                std::string    Said = std::format("{}  #{}  {}  ", One->Name, One->Seen.Pid, LivingName(One->Where));
                Said += One->Seen.SpawnerPid == 0 ? "started by the system" : From ? "started from " + From->Name : "started";
                if (Now && Now->AtUs > One->Seen.BeganUs) Said += std::format(", {} ago", Lasted(Now->AtUs - One->Seen.BeganUs));
                if (One->Seen.Raised) Said += std::format("  raised again {} times", One->Seen.Raised);
                Say(List, On, X0 + 1, Under + 1, Said, Shown.Title, static_cast<float>(Width - 2));
                Say(List, On, X0 + 1, Under + 2, "tags " + One->Seen.Tags, Shown.Quiet, static_cast<float>(Width - 2));
            }
        }
    }

    /* The machine: memory, the volume, the Use Context. */
    if (MachineWidth) {
        gui::Group Machine = Out.Group();
        Machine.Key(7).SetPlace(Below);
        const Room Inside = Panel(Machine, On, Shown, Room{Cols - MachineWidth, 1 + Above, MachineWidth, Middle}, "machine", "");
        if (!Inside.Empty() && Now) {
            std::int32_t Row   = Inside.Row;
            const float  X0    = static_cast<float>(Inside.Col);
            const float  Wide  = static_cast<float>(Inside.Cols);
            const auto   Fits  = [&](std::int32_t Journaled) { return Row + Journaled <= Inside.Row + Inside.Rows; };
            const auto   Gauge = [&](std::string_view Label, std::uint64_t Used, std::uint64_t Total, const std::string &Said) {
                if (!Fits(2)) return;
                const double Share = Total ? static_cast<double>(Used) / static_cast<double>(Total) : 0;
                Say(Machine, On, X0, static_cast<float>(Row), Label, Shown.Text);
                Say(Machine, On, X0, static_cast<float>(Row), Said, Shown.Quiet, Wide, ITEM_TEXT_RIGHT);
                Row++;
                Meter(Machine, On, Shown, X0, static_cast<float>(Row), Wide - 6, Share);
                Say(Machine, On, X0, static_cast<float>(Row), Percent(Share), LoadColor(Shown, Share), Wide, ITEM_TEXT_RIGHT);
                Row += 2;
            };
            Gauge("memory", Now->MemoryUsed, Now->MemoryTotal, std::format("{} of {}", Bytes(Now->MemoryUsed), Bytes(Now->MemoryTotal)));
            if (Now->VolumePresent)
                Gauge(Now->VolumeName.empty() ? "volume" : "volume " + Now->VolumeName, Now->VolumeUsed, Now->VolumeTotal,
                      std::format("{} of {}", Bytes(Now->VolumeUsed), Bytes(Now->VolumeTotal)));
            else if (Fits(2)) {
                Say(Machine, On, X0, static_cast<float>(Row), "no volume", Shown.Quiet);
                Row += 2;
            }
            if (Fits(1)) {
                Say(Machine, On, X0, static_cast<float>(Row), "context", Shown.Text);
                Say(Machine, On, X0, static_cast<float>(Row), Now->Context.empty() ? "none" : Now->Context,
                    Now->Context.empty() ? Shown.Quiet : LivingInk(Shown, Living::OnScreen), Wide, ITEM_TEXT_RIGHT);
                Row++;
            }
            if (Fits(1) && !Now->PlacesKnown) Say(Machine, On, X0, static_cast<float>(Row), "no display said where programs are", Shown.Quiet, Wide);
        }
    }

    /* The journal: what happened, the latest at the bottom. */
    if (Journal) {
        gui::Group               Journaled  = Out.Group();
        Journaled.Key(8).SetPlace(Below);
        const std::deque<Entry> &Entries = Said.Entries();
        const Room Inside = Panel(Journaled, On, Shown, Room{0, 1 + Above + Middle, Cols, Journal}, "journal",
                                  Said.Hears() ? std::format("{}", Entries.size()) : "not heard");
        if (!Inside.Empty()) {
            const std::size_t Shows = static_cast<std::size_t>(Inside.Rows);
            const std::size_t First = Entries.size() > Shows ? Entries.size() - Shows : 0;
            std::int32_t      Row   = Inside.Row;
            for (std::size_t Index = First; Index < Entries.size(); Index++, Row++) {
                const Entry &One = Entries[Index];
                Say(Journaled, On, static_cast<float>(Inside.Col + 1), static_cast<float>(Row), One.At, Shown.Quiet);
                Say(Journaled, On, static_cast<float>(Inside.Col + 11), static_cast<float>(Row), One.Text, HappenedInk(Shown, One.What),
                    static_cast<float>(Inside.Cols - 12));
            }
            if (Entries.empty())
                Say(Journaled, On, static_cast<float>(Inside.Col + 1), static_cast<float>(Inside.Row), "nothing has happened since the board began", Shown.Quiet);
        }
    }

    /* Every key, in a window over the rest, when asked. */
    if (Help) {
        gui::Group Window = Out.Group();
        Window.Key(9).SetPlace(Below);
        const std::pair<std::string, std::string> Every[] = {
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
        const std::int32_t Wide  = std::min(Cols - 2, 82);
        const std::int32_t High  = static_cast<std::int32_t>(std::size(Every)) + 5;
        const Room         Over{(Cols - Wide) / 2, (Rows - High) / 2, Wide, High};
        gui::Shape         Shade = Block(Window, {On.X(static_cast<float>(Over.Col)), On.Y(static_cast<float>(Over.Row))},
                                         {On.X(static_cast<float>(Wide)), On.Y(static_cast<float>(High))}, Shown.Panel, 12);
        Shade.SetRole(gui::Role::Panel);
        Say(Window, On, static_cast<float>(Over.Col + 2), static_cast<float>(Over.Row + 1), "the board's keys", Shown.Title);
        Say(Window, On, static_cast<float>(Over.Col + 2), static_cast<float>(Over.Row + 1), "settings taskboard keys", Shown.Quiet,
            static_cast<float>(Wide - 4), ITEM_TEXT_RIGHT);
        std::int32_t Row = Over.Row + 3;
        for (const auto &[Key, Does] : Every) {
            Say(Window, On, static_cast<float>(Over.Col + 3), static_cast<float>(Row), Key, Shown.Title);
            Say(Window, On, static_cast<float>(Over.Col + 23), static_cast<float>(Row), Does, Shown.Text, static_cast<float>(Wide - 25));
            Row++;
        }
        Say(Window, On, static_cast<float>(Over.Col + 2), static_cast<float>(Over.Row + High - 1), "any key closes this", Shown.Quiet);
    }
}

}  // namespace taskboard
