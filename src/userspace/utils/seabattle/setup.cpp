/* setup.cpp — the setup: the players' names, the grid, the fleet and the
 * rules, chosen row by row before a game, with the fleet shown beside them. */

#include <algorithm>
#include <array>
#include <format>
#include <span>

#include "painter/tui/glyph.h"
#include "painter/tui/text.h"
#include "painter/tui/window.h"

#include "hud.h"
#include "pages.h"
#include "sea.h"
#include "ship_art.h"

namespace seabattle {

namespace {

namespace tui = painter::tui;
namespace core = painter::core;

enum Row : int { RowOne, RowOpponent, RowTwo, RowGrid, RowFleet, RowTouching, RowExtra, RowSalvo, RowSpeed, RowStart, RowCount };

constexpr std::array<int, 4> Grids = {6, 8, 10, 12};
constexpr int PreviewCell = 6;
constexpr int PreviewWide = 30;

constexpr std::array<const char *, RowCount> Labels = {"First player", "Opponent", "Second player", "Grid", "Fleet", "Ships may touch",
                                                       "Hit earns a shot", "Salvo", "Animation", ""};

constexpr std::array<const char *, RowCount> Notes = {
    "Type a name. Names are kept in the hall of records.",
    "Another person at this screen, or the computer, which lays its own fleet and hunts yours.",
    "Type a name.",
    "The size of each player's waters.",
    "The ships each player lays. The fleet is shown on the right.",
    "The classic rule: ships may not touch, not even at the corners. Yes lets them lie side by side.",
    "The classic rule: a player who hits shoots again.",
    "As many shots in a turn as ships still afloat; a hit then earns nothing extra.",
    "How fast the shells fly and the ships sink. Instant shows only where each shot fell.",
    "Begin the game."};

std::string PaceName(Pace Speed)
{
    switch (Speed) {
    case Pace::Slow:    return "Slow";
    case Pace::Fast:    return "Fast";
    case Pace::Instant: return "Instant";
    case Pace::Normal:  break;
    }
    return "Normal";
}

bool SameName(const std::string &One, const std::string &Other)
{
    return std::ranges::equal(One, Other, [](char A, char B) { return (A | 0x20) == (B | 0x20); });
}

// Where the setup's parts lie on a glass: the panel of rows at the left, the fleet's picture at the right.
struct Geometry {
    int        Left, Top, OptionWide, Rows, Pitch;
    core::Rect Preview;       // the cells inside the picture's frame
};

Geometry GeometryOf(core::Size Size)
{
    const int Wide = std::min(Size.Width - 6, 86);
    const int Left = (Size.Width - Wide) / 2;
    const int Pitch = Size.Height >= 36 ? 2 : 1;
    const int Rows = std::min(Size.Height - 9, RowCount * Pitch + 6);
    const int Top = std::max(3, (Size.Height - Rows - 2) / 2);
    const int OptionWide = Wide - PreviewWide - 3;
    return Geometry{Left, Top, OptionWide, Rows, Pitch, core::Rect{Left + OptionWide + 3, Top + 2, PreviewWide, Rows - 2}};
}

// The largest cell the whole fleet can be shown in, side by side in lines, inside the picture's frame.
int PreviewCellFor(std::span<const int> Fleet, const core::Rect &View)
{
    for (int Cell = PreviewCell; Cell > 2; Cell--) {
        int Lines = 1, Used = 2;
        for (const int Length : Fleet) {
            if (Used + Length * Cell > View.Width - 1) {
                Lines++;
                Used = 2;
            }
            Used += Length * Cell + 3;
        }
        if (Lines * (Cell + 3) + 3 <= View.Height * 2) return Cell;
    }
    return 2;
}

class SetupPage final : public Page {
public:
    explicit SetupPage(Context &Shared) : Page(Shared), Plan(Shared.Plan) { CheckFit(); }

    std::uint32_t FrameMs() const override { return 200; }

    void Key(const tui::KeyEvent &Pressed) override;

protected:
    void Pixels(Canvas &Out) override;
    void Words(tui::Frame &Out) override;

private:
    bool        Typing() const { return At == RowOne || (At == RowTwo && !Plan.VsComputer); }
    std::string &Typed() { return At == RowOne ? Plan.NameOne : Plan.NameTwo; }
    void        Move(int By);
    void        Change(int By);
    void        Type(char Letter);
    void        CheckFit();
    void        Begin();
    std::string ValueOf(int Which) const;
    void        PaintRow(tui::Frame &Out, const core::Rect &Inside, int Which, std::int32_t Line) const;

    Setup &Plan;
    int    At = RowOne;
    bool   Fits = true;
    std::string Warning;
};

void SetupPage::Move(int By)
{
    do At = (At + By + RowCount) % RowCount;
    while (At == RowTwo && Plan.VsComputer);
}

template <typename Item, std::size_t Count>
Item Cycled(const std::array<Item, Count> &Choices, Item Now, int By)
{
    const auto Found = std::ranges::find(Choices, Now);
    const auto Place = static_cast<int>(Found == Choices.end() ? 0 : Found - Choices.begin());
    return Choices[static_cast<std::size_t>((Place + By + static_cast<int>(Count)) % static_cast<int>(Count))];
}

void SetupPage::Change(int By)
{
    constexpr std::array<FleetKind, FleetKindCount> Fleets = {FleetKind::Classic, FleetKind::Skirmish, FleetKind::Armada, FleetKind::Duel};
    constexpr std::array<Pace, 4> Paces = {Pace::Slow, Pace::Normal, Pace::Fast, Pace::Instant};
    switch (At) {
    case RowOpponent: Plan.VsComputer = !Plan.VsComputer; break;
    case RowGrid:     Plan.Chosen.Grid = Cycled(Grids, Plan.Chosen.Grid, By); break;
    case RowFleet:    Plan.Chosen.Fleet = Cycled(Fleets, Plan.Chosen.Fleet, By); break;
    case RowTouching: Plan.Chosen.Touching = !Plan.Chosen.Touching; break;
    case RowExtra:    Plan.Chosen.ExtraShot = !Plan.Chosen.ExtraShot; break;
    case RowSalvo:    Plan.Chosen.Salvo = !Plan.Chosen.Salvo; break;
    case RowSpeed:    Plan.Speed = Cycled(Paces, Plan.Speed, By); break;
    default: return;
    }
    CheckFit();
}

void SetupPage::Type(char Letter)
{
    std::string &Name = Typed();
    if (Letter == tui::KeyCodes::Backspace) {
        if (!Name.empty()) Name.pop_back();
    } else if (Letter >= ' ' && Letter <= '~' && Name.size() < MostName) {
        Name += Letter;
    }
}

// Whether the fleet can be laid on the grid by the rules: tried the way the game will do it.
void SetupPage::CheckFit()
{
    Board Trial(Plan.Chosen.Grid);
    Fits = Trial.Fill(Ctx.Chance, FleetLengths(Plan.Chosen.Fleet), Plan.Chosen.Touching);
    Warning = Fits ? "" : "This fleet does not fit this grid by these rules: choose a larger grid, a smaller fleet, or let ships touch.";
}

void SetupPage::Begin()
{
    const auto Trimmed = [](std::string Name, const char *Fallback) {
        while (!Name.empty() && Name.back() == ' ') Name.pop_back();
        const std::size_t First = Name.find_first_not_of(' ');
        return First == std::string::npos ? std::string(Fallback) : Name.substr(First);
    };
    Plan.NameOne = Trimmed(Plan.NameOne, "Captain 1");
    Plan.NameTwo = Trimmed(Plan.NameTwo, "Captain 2");
    if (!Fits) return;
    if (!Plan.VsComputer && SameName(Plan.NameOne, Plan.NameTwo)) {
        Warning = "The two players need different names.";
        At = RowTwo;
        return;
    }
    Ctx.StartGame();
}

void SetupPage::Key(const tui::KeyEvent &Pressed)
{
    if (Pressed.IsExtended()) {
        switch (Pressed.Scancode) {
        case tui::KeyCodes::Up:    Move(-1); break;
        case tui::KeyCodes::Down:  Move(1); break;
        case tui::KeyCodes::Left:  Change(-1); break;
        case tui::KeyCodes::Right: Change(1); break;
        case tui::KeyCodes::Home:  At = RowOne; break;
        case tui::KeyCodes::End:   At = RowStart; break;
        default: break;
        }
        return;
    }
    if (Pressed.IsCtrl() || Pressed.IsAlt()) return;
    if (Pressed.Ascii == tui::KeyCodes::Escape) {
        Ctx.Go(PageId::Title);
    } else if (Pressed.Ascii == tui::KeyCodes::Enter) {
        if (At == RowStart) Begin();
        else Move(1);
    } else if (Pressed.Ascii == tui::KeyCodes::Tab) {
        Move(1);
    } else if (Typing()) {
        Type(Pressed.Ascii);
    } else if (Pressed.Ascii == ' ') {
        Change(1);
    }
}

std::string SetupPage::ValueOf(int Which) const
{
    switch (Which) {
    case RowOne:      return Plan.NameOne;
    case RowOpponent: return Plan.VsComputer ? "The computer" : "Another player";
    case RowTwo:      return Plan.VsComputer ? std::string(ComputerName) : Plan.NameTwo;
    case RowGrid:     return std::format("{} x {}", Plan.Chosen.Grid, Plan.Chosen.Grid);
    case RowFleet:    return std::format("{}: {}", FleetName(Plan.Chosen.Fleet), FleetSummary(Plan.Chosen.Fleet));
    case RowTouching: return Plan.Chosen.Touching ? "Yes" : "No (classic)";
    case RowExtra:    return Plan.Chosen.Salvo ? "Not in a salvo" : (Plan.Chosen.ExtraShot ? "Yes (classic)" : "No");
    case RowSalvo:    return Plan.Chosen.Salvo ? "On" : "Off";
    case RowSpeed:    return PaceName(Plan.Speed);
    default:          return "";
    }
}

void SetupPage::PaintRow(tui::Frame &Out, const core::Rect &Inside, int Which, std::int32_t Line) const
{
    const Look &Shown = Ctx.Chosen;
    const Color Paper = Mixed(Shown.Bars, Shown.Sea, 0.16f);
    const bool  Here = At == Which;
    const bool  Name = Which == RowOne || Which == RowTwo;
    const bool  Dim = Which == RowTwo && Plan.VsComputer;
    const Color Row = Here ? Mixed(Paper, Shown.Title, 0.16f) : Paper;
    Out.Fill(core::Rect{Inside.X, Line, Inside.Width, 1}, ' ', {Shown.Text, Row});
    if (Which == RowStart) {
        const std::string Words = Fits ? "[ Start the battle ]" : "[ Start the battle ]  (the fleet does not fit)";
        PutCentred(Out, Inside.X, Inside.Width, Line, Words, Fits ? (Here ? Shown.Good : Shown.Text) : Shown.Bad, Row);
        return;
    }
    if (Here) Out.Put({Inside.X, Line}, tui::Glyphs::Pointer, {Shown.Title, Row});
    tui::PutText(Out, core::Point{Inside.X + 2, Line}, Labels[static_cast<std::size_t>(Which)], 16, Dim ? Shown.Quiet : Shown.Text, Row);
    const std::int32_t Left = Inside.X + 19;
    std::string        Value = ValueOf(Which);
    if (Which == RowFleet && static_cast<std::int32_t>(Value.size()) > Inside.Width - 22) Value = std::string(FleetName(Plan.Chosen.Fleet));
    if (Name) {
        const Color Field = Here ? Darker(Shown.Sea, 0.62f) : Darker(Shown.Sea, 0.78f);
        Out.Fill(core::Rect{Left, Line, static_cast<std::int32_t>(MostName) + 2, 1}, ' ', {Shown.Text, Field});
        tui::PutText(Out, core::Point{Left + 1, Line}, Value, static_cast<std::int32_t>(MostName), Dim ? Shown.Quiet : Shown.Title, Field);
        if (Here && Typing()) Out.SetCaret(core::Point{Left + 1 + static_cast<std::int32_t>(Value.size()), Line});
        return;
    }
    if (Here) {
        Out.Put({Left - 1, Line}, tui::Glyphs::Left, {Shown.Title, Row});
        Out.Put({Left + 1 + static_cast<std::int32_t>(tui::CellsOf(Value)), Line}, tui::Glyphs::Right, {Shown.Title, Row});
    }
    tui::PutText(Out, core::Point{Left + 1, Line}, Value, Inside.Width - 22, Here ? Shown.Title : Shown.Text, Row);
}

void SetupPage::Pixels(Canvas &Out)
{
    PaintBackdrop(Out, Ctx.Chosen.Sea, Ctx.Text(), Ctx.Now);
    const core::Rect View = GeometryOf({Out.Width, Out.Height / 2}).Preview;
    PaintSea(Out, SeaPatch{View.X, View.Y * 2, View.Width, View.Height * 2, Ctx.Chosen.Sea, 0.5f, 0.9f, Ctx.Text()}, Ctx.Now);
    const std::span<const int> Fleet = FleetLengths(Plan.Chosen.Fleet);
    const int Cell = PreviewCellFor(Fleet, View);
    int       X = View.X + 2, Y = View.Y * 2 + 3;
    for (const int Length : Fleet) {
        if (X + Length * Cell > View.X + View.Width - 1) {
            X = View.X + 2;
            Y += Cell + 3;
        }
        DrawShip(Out, ShipDraw{.X = X, .Y = Y, .Cell = Cell, .Length = Length, .Steel = Ctx.Chosen.Ship, .Water = Ctx.Chosen.Sea, .Ms = Ctx.Now, .Plain = Ctx.Text()});
        X += Length * Cell + 3;
    }
}

void SetupPage::Words(tui::Frame &Out)
{
    const Look &Shown = Ctx.Chosen;
    const core::Size Size = Out.Size();
    PaintBar(Out, Shown, "SEA BATTLE", "set up the game");
    PaintHints(Out, Shown, {{"\xE2\x86\x95", "row", 1}, {"\xE2\x86\x94", "change", 2}, {"enter", At == RowStart ? "start" : "next", 0, At == RowStart && Fits},
                            {"tab", "next", 4}, {"esc", "back", 3}});

    const Geometry Place = GeometryOf(Size);
    const core::Rect Inside = PaintPanel(Out, Shown, core::Rect{Place.Left, Place.Top, Place.OptionWide, Place.Rows + 2}, "SETUP", "");
    const std::int32_t First = Inside.Y + 1;
    for (int Which = 0; Which < RowCount; Which++) PaintRow(Out, Inside, Which, First + Which * Place.Pitch + (Which == RowStart ? 1 : 0));

    const std::vector<std::string> Hint = Wrapped(Warning.empty() ? Notes[static_cast<std::size_t>(At)] : Warning, static_cast<std::size_t>(Inside.Width - 2));
    for (std::size_t Line = 0; Line < Hint.size() && Line < 3; Line++)
        tui::PutText(Out, core::Point{Inside.X + 1, Inside.Y + Inside.Height - 3 + static_cast<std::int32_t>(Line)}, Hint[Line], Inside.Width - 2,
                     Warning.empty() ? Shown.Quiet : Shown.Bad, Mixed(Shown.Bars, Shown.Sea, 0.16f));

    const core::Rect View{Place.Preview.X - 1, Place.Preview.Y - 1, Place.Preview.Width + 2, Place.Preview.Height + 2};
    PaintFrame(Out, View, Mixed(Shown.Title, Shown.Bars, 0.45f), Shown.Bars);
    const std::string Count = std::format("{} ships, {} cells of {}", FleetLengths(Plan.Chosen.Fleet).size(), FleetCells(Plan.Chosen.Fleet),
                                          Plan.Chosen.Grid * Plan.Chosen.Grid);
    Out.Fill(core::Rect{View.X, View.Y + View.Height, View.Width, 1}, ' ', {Shown.Text, Shown.Bars});
    tui::PutText(Out, core::Point{View.X + 1, View.Y + View.Height}, Count, View.Width - 2, Shown.Quiet, Shown.Bars);
    Out.Fill(core::Rect{View.X, 3, View.Width, 1}, ' ', {Shown.Text, Shown.Bars});
    tui::PutText(Out, core::Point{View.X + 1, 3}, std::format("The {} fleet", FleetName(Plan.Chosen.Fleet)), View.Width - 2, Shown.Title, Shown.Bars);
}

}  // namespace

std::unique_ptr<Page> MakeSetup(Context &Shared)
{
    return std::make_unique<SetupPage>(Shared);
}

}  // namespace seabattle
