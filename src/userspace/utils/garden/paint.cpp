/* paint.cpp — the garden's picture: the sky and the plants over their names,
 * or the list of them; the line at the top, the plant chosen and the keys at
 * the bottom; a window of every key over them when asked. */

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "box/clock.h"
#include "painter/tui/glyph.h"
#include "painter/tui/text.h"
#include "painter/tui/window.h"

#include "air.h"
#include "flora.h"
#include "view.h"

namespace garden {

namespace {

namespace tui    = painter::tui;
namespace Glyphs = painter::tui::Glyphs;
using painter::core::Point;
using painter::core::Rect;

constexpr char32_t Club    = U'\u2663';   /* ♣ */
constexpr char32_t Diamond = U'\u2666';   /* ♦ */
constexpr char32_t Sun     = U'\u263C';   /* ☼ */
// Between the words of a line: a middle dot, as text.
constexpr std::string_view Between = "\xC2\xB7";
constexpr std::int32_t     LabelRows = 3;
// What the top line leaves to the notices of the programs that live on their own.
constexpr std::int32_t     NoticeRoom = 36;

Ink Mixed(Ink A, Ink B, double Share) noexcept
{
    return Ink::Between(A, B, static_cast<std::uint8_t>(std::clamp(Share, 0.0, 1.0) * 255.0));
}

std::int32_t Say(tui::Frame &Out, Point At, std::string_view Text, std::int32_t Width, Ink Color, Ink Under)
{
    return tui::PutText(Out, At, Text, Width, Color, Under);
}

void SayCentred(tui::Frame &Out, std::int32_t Left, std::int32_t Row, std::int32_t Width, std::string_view Text, Ink Color,
                Ink Under)
{
    const std::string Fit = tui::Shortened(Text, static_cast<std::size_t>(std::max(Width, 0)));
    const auto        Cells = static_cast<std::int32_t>(tui::CellsOf(Fit));
    Say(Out, Point{Left + (Width - Cells) / 2, Row}, Fit, Width, Color, Under);
}

const char *SeasonName(Season Year) noexcept
{
    switch (Year) {
    case Season::Spring: return "spring";
    case Season::Autumn: return "autumn";
    case Season::Winter: return "winter";
    case Season::Summer:
    case Season::Clock:  break;
    }
    return "summer";
}

// The parts of a line, a dot between them, as many as fit in Room cells.
std::string Joined(const std::vector<std::string> &Parts, std::int32_t Room)
{
    std::string Line;
    for (const std::string &Part : Parts) {
        const std::string More = Line.empty() ? Part : std::format("{}  {}  {}", Line, Between, Part);
        if (static_cast<std::int32_t>(tui::CellsOf(More)) > Room) break;
        Line = More;
    }
    return Line;
}

std::uint64_t Since(std::uint64_t Then)
{
    const std::uint64_t Now = clock_unix_now();
    return Now > Then ? Now - Then : 0;
}

}  // namespace

void View::Paint()
{
    tui::Frame &Out = Screen.FrameBuffer();
    Out.Clear({Shown.Text, Shown.Bars});
    const Layout Where = LayoutOf();
    const Light  Lit   = LightNow(Shown);
    if (List) PaintList(Out, Rect{0, 1, Out.Size().Width, Out.Size().Height - 3});
    else      PaintGarden(Out, Where, Lit);
    PaintTop(Out, Lit);
    PaintBottom(Out);
    if (Help) PaintHelp(Out);
}

void View::PaintGarden(tui::Frame &Out, const Layout &Where, const Light &Lit)
{
    Scroll(static_cast<std::size_t>(Where.Slots));
    const Palette Colors = PaletteIn(Shown, Lit);
    const Pen     Draw(Out, Where.Sky);
    PaintSky(Draw, Lit, Time);
    PaintGrass(Draw, Colors);
    Out.Fill(Rect{0, Where.LabelRow, Where.Sky.Width, LabelRows}, ' ', {Shown.Text, Colors.Soil});
    if (Ranking.empty()) {
        PaintFall(Draw, Colors, Time);
        PaintNothing(Out);
        return;
    }

    std::vector<Haunt> Over;
    std::int32_t       ChosenX = -1, ChosenY = 0;
    for (std::int32_t Slot = 0; Slot < Where.Slots && First + static_cast<std::size_t>(Slot) < Ranking.size(); Slot++) {
        const std::size_t Index = First + static_cast<std::size_t>(Slot);
        const Plant      &One   = *Ranking[Index];
        const Growth      Grown = GrowthOf(One, clock_unix_now(), Shown);
        const std::uint32_t Hash = Seed(One.Name);
        const std::int32_t  Left = 1 + Slot * Where.SlotWidth;
        const PlantPose     Pose{Hash, Grown.Reached, GrowerOf(One).Height, Grown.Fresh,
                                 Left + Where.SlotWidth / 2 + static_cast<std::int32_t>((Hash >> 9) % 3) - 1,
                                 GrowerOf(One).Pulse};
        const Crest Top = PaintPlant(Draw, Colors, Pose, Time);
        if (Shown.Creatures && Grown.Fresh >= 0.6 && Grown.Reached >= Stage::Young) Over.push_back(Haunt{Top, Hash, Index == 0});
        if (Index == Choice) {
            ChosenX = Top.X;
            ChosenY = Top.Y;
        }
        PaintWords(Out, Where, Index, Left, Colors);
    }
    PaintVisitors(Draw, Colors, Over, Time);
    PaintFall(Draw, Colors, Time);
    if (ChosenX < 0) return;
    const std::int32_t Bob = ChosenY - 6 + static_cast<std::int32_t>(std::lround(std::sin(Time * 3.0)));
    for (std::int32_t Step = 0; Step <= 2; Step++) {
        Draw.Dot(ChosenX - 2 + Step, Bob - 2 + Step, Shown.Title);
        Draw.Dot(ChosenX + 2 - Step, Bob - 2 + Step, Shown.Title);
    }
}

// The three lines under a plant: its name, its rank and starts, how lately.
void View::PaintWords(tui::Frame &Out, const Layout &Where, std::size_t Index, std::int32_t Left,
                      const Palette &Colors) const
{
    const Plant &One  = *Ranking[Index];
    const bool   Here = Index == Choice;
    const Ink    Under = Here ? Shown.Chosen : Colors.Soil;
    const std::int32_t Row = Where.LabelRow;
    if (Here) Out.Fill(Rect{Left, Row, Where.SlotWidth, LabelRows}, ' ', {Shown.Text, Shown.Chosen});
    SayCentred(Out, Left + 1, Row, Where.SlotWidth - 2, One.Name, Here ? Shown.Title : Shown.Text, Under);
    SayCentred(Out, Left, Row + 1, Where.SlotWidth, std::format("#{} {}x", Index + 1, One.Starts), Shown.Text, Under);
    SayCentred(Out, Left, Row + 2, Where.SlotWidth, Brief(Since(One.Last)), Shown.Quiet, Under);
    if (IsMarked(One)) Out.Put(Point{Left, Row}, Diamond, {Shown.Marked, Under});
}

// A garden with nothing in it yet, and why.
void View::PaintNothing(tui::Frame &Out) const
{
    const painter::core::Size Size = Out.Size();
    const Rect Area = tui::Centred(Size, std::min(Size.Width - 4, 64), Keeper ? 6 : 7);
    const Rect Inside = tui::DrawWindow(Out, Area, "an empty garden", "", "");
    Say(Out, Point{Inside.X + 2, Inside.Y + 1}, "Nothing grows here yet.", Inside.Width - 4, tui::TitleInk, tui::WindowPaper);
    Say(Out, Point{Inside.X + 2, Inside.Y + 2}, "Every program you start plants a seed;", Inside.Width - 4, tui::WindowInk,
        tui::WindowPaper);
    Say(Out, Point{Inside.X + 2, Inside.Y + 3}, "the more you use it, the taller it grows.", Inside.Width - 4, tui::WindowInk,
        tui::WindowPaper);
    if (!Keeper)
        Say(Out, Point{Inside.X + 2, Inside.Y + 4}, "The gardener is not running: start it with  gardener", Inside.Width - 4,
            Shown.Marked, tui::WindowPaper);
}

void View::PaintList(tui::Frame &Out, Rect Area)
{
    const std::int32_t Wide     = Area.Width;
    const std::int32_t NameWide = std::clamp(Wide / 5, 12, 22);
    const std::int32_t BarWide  = std::clamp(Wide - (NameWide + 13) - 24, 6, 24);
    const std::int32_t StartsAt = 4 + NameWide + 1;
    const std::int32_t BarAt    = StartsAt + 8;
    const std::int32_t LastAt   = BarAt + BarWide + 1;
    const bool         Planted  = LastAt + 15 + 15 + 9 <= Wide;
    const std::int32_t PlantedAt = LastAt + 15;
    const std::int32_t StateAt  = Planted ? PlantedAt + 15 : PlantedAt;
    const Ink          Paper    = Shown.Bars;
    Out.Fill(Area, ' ', {Shown.Text, Paper});
    Say(Out, Point{1, Area.Y}, "# program", NameWide + 4, Shown.Quiet, Paper);
    Say(Out, Point{StartsAt, Area.Y}, "starts", 7, Shown.Quiet, Paper);
    Say(Out, Point{BarAt, Area.Y}, "growth", BarWide, Shown.Quiet, Paper);
    Say(Out, Point{LastAt, Area.Y}, "last started", 14, Shown.Quiet, Paper);
    if (Planted) Say(Out, Point{PlantedAt, Area.Y}, "first started", 14, Shown.Quiet, Paper);
    Say(Out, Point{StateAt, Area.Y}, "state", Wide - StateAt, Shown.Quiet, Paper);

    const std::int32_t Rows = std::max(1, Area.Height - 1);
    Scroll(static_cast<std::size_t>(Rows));
    for (std::int32_t Row = 0; Row < Rows && First + static_cast<std::size_t>(Row) < Ranking.size(); Row++) {
        const std::size_t Index = First + static_cast<std::size_t>(Row);
        const Plant      &One   = *Ranking[Index];
        const Growth      Grown = GrowthOf(One, clock_unix_now(), Shown);
        const bool        Here  = Index == Choice;
        const Ink         Under = Here ? Shown.Chosen : Paper;
        const std::int32_t Y    = Area.Y + 1 + Row;
        if (Here) Out.Fill(Rect{0, Y, Wide, 1}, ' ', {Shown.Text, Shown.Chosen});
        if (IsMarked(One)) Out.Put(Point{0, Y}, Diamond, {Shown.Marked, Under});
        Say(Out, Point{1, Y}, std::format("{}", Index + 1), 3, Shown.Quiet, Under);
        Say(Out, Point{4, Y}, tui::Shortened(One.Name, static_cast<std::size_t>(NameWide)), NameWide, Here ? Shown.Title : Shown.Text, Under);
        Say(Out, Point{StartsAt, Y}, std::format("{}", One.Starts), 7, Shown.Text, Under);

        const Ink Leaf = Mixed(Shown.Leaf, Shown.Wilted, 1.0 - Grown.Fresh);
        const auto Filled = static_cast<std::int32_t>(std::clamp(std::lround(std::log2(One.Starts) / 7.0 * BarWide), 1L, static_cast<long>(BarWide)));
        for (std::int32_t Cell = 0; Cell < BarWide; Cell++)
            Out.Put(Point{BarAt + Cell, Y}, Cell < Filled ? Glyphs::Full : Glyphs::LightShade,
                    {Cell < Filled ? Leaf : Mixed(Paper, Shown.Quiet, 0.3), Under});
        if (Grown.Reached >= Stage::Blooming && Filled > 0)
            Out.Put(Point{BarAt + Filled - 1, Y}, Grown.Reached == Stage::Tree ? Club : Sun,
                    {Grown.Reached == Stage::Tree ? Shown.Fruit : Shown.BloomA, Under});
        Say(Out, Point{LastAt, Y}, Ago(Since(One.Last)), 14, Shown.Quiet, Under);
        if (Planted) Say(Out, Point{PlantedAt, Y}, Ago(Since(One.Planted)), 14, Shown.Quiet, Under);
        Say(Out, Point{StateAt, Y}, StateName(Grown.Fresh), Wide - StateAt, Mixed(Shown.Wilted, Leaf, Grown.Fresh), Under);
    }
}

void View::PaintTop(tui::Frame &Out, const Light &Lit) const
{
    const std::int32_t Wide = Out.Size().Width;
    Out.Fill(Rect{0, 0, Wide, 1}, ' ', {Shown.Text, Shown.Bars});
    // What is said here stands at the left: the right of the top line is where
    // the programs that live on their own lay their notices, and is left to them.
    Out.Put(Point{1, 0}, Club, {Shown.Leaf, Shown.Bars});
    Say(Out, Point{3, 0}, "garden", Wide - 3, Shown.Title, Shown.Bars);
    std::uint64_t Starts = 0;
    for (const Plant *One : Ranking) Starts += One->Starts;
    std::vector<std::string> Parts = {std::format("{} plants", Ranking.size()), std::format("{} starts", Starts),
                                      SeasonName(Lit.Year), std::format("ranked by {}", OrderName(Shown.Ranking))};
    if (!Marked.empty()) Parts.push_back(std::format("{} marked", Marked.size()));
    Say(Out, Point{10, 0}, Between, 1, Shown.Quiet, Shown.Bars);
    Say(Out, Point{12, 0}, Joined(Parts, std::max(10, Wide - 12 - NoticeRoom)), Wide - 12, Shown.Quiet, Shown.Bars);
}

void View::PaintBottom(tui::Frame &Out) const
{
    const painter::core::Size Size = Out.Size();
    const std::int32_t        Wide = Size.Width;
    const std::int32_t        Row  = Size.Height - 2;
    Out.Fill(Rect{0, Row, Wide, 2}, ' ', {Shown.Text, Shown.Bars});
    if (!Status.empty()) {
        Say(Out, Point{1, Row}, Status, Wide - 2, Shown.Marked, Shown.Bars);
    } else if (const Plant *One = Chosen()) {
        const Growth Grown = GrowthOf(*One, clock_unix_now(), Shown);
        std::int32_t At    = 1;
        At += Say(Out, Point{At, Row}, One->Name, Wide - At, Shown.Title, Shown.Bars) + 2;
        const std::vector<std::string> Parts = {std::format("{} starts", One->Starts), std::format("last {}", Ago(Since(One->Last))),
                                                std::format("planted {}", Ago(Since(One->Planted))),
                                                std::format("{}, {}", StageName(Grown.Reached), StateName(Grown.Fresh))};
        Say(Out, Point{At, Row}, Joined(Parts, Wide - At - 1), Wide - At - 1, Shown.Text, Shown.Bars);
    }
    const std::pair<std::string, std::string> Keys[] = {{"left right", "choose"},
                                                        {"space", "mark"},
                                                        {Spelled(KeyView), List ? "the garden" : "the list"},
                                                        {Spelled(KeyOrder), "rank"},
                                                        {Spelled(KeyHelp), "keys"},
                                                        {"esc", "leave"}};
    std::int32_t At = 1;
    for (const auto &[Key, Does] : Keys) {
        if (At + static_cast<std::int32_t>(tui::CellsOf(Key) + tui::CellsOf(Does)) + 2 > Wide) break;
        At += Say(Out, Point{At, Row + 1}, Key, Wide - At, Shown.Title, Shown.Bars) + 1;
        At += Say(Out, Point{At, Row + 1}, Does, Wide - At, Shown.Quiet, Shown.Bars) + 2;
    }
}

void View::PaintHelp(tui::Frame &Out) const
{
    const std::pair<std::string, std::string> Keys[] = {
        {"left, right", "choose a plant"},
        {"up, down", "the same"},
        {"page up, page down", "several plants at a time"},
        {"home, end", "the first and the last"},
        {"space", "mark it: the marked are given back as them"},
        {Spelled(KeyView), "the garden, or the list of its plants"},
        {Spelled(KeyOrder), "rank by starts, by how lately, by name"},
        {Spelled(KeyHelp), "these keys"},
        {"esc", "leave"},
    };
    constexpr std::int32_t    KeyWidth = 20;
    const painter::core::Size Size     = Out.Size();
    const Rect Area   = tui::Centred(Size, std::min(Size.Width - 2, 70), static_cast<std::int32_t>(std::size(Keys)) + 4);
    const Rect Inside = tui::DrawWindow(Out, Area, "the garden's keys", "settings garden keys", "any key closes this");
    std::int32_t Row = Inside.Y + 1;
    for (const auto &[Key, Does] : Keys) {
        if (Row >= Inside.Y + Inside.Height) break;
        Say(Out, Point{Inside.X + 2, Row}, Key, KeyWidth, tui::TitleInk, tui::WindowPaper);
        Say(Out, Point{Inside.X + 2 + KeyWidth, Row}, Does, Inside.Width - KeyWidth - 3, tui::WindowInk, tui::WindowPaper);
        Row++;
    }
}

}  // namespace garden
