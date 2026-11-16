/* place.cpp — a player lays the fleet: a ship follows the cursor, turns,
 * is put down, taken back; the cells the rules forbid are marked; or the
 * whole fleet is laid at random. */

#include <algorithm>
#include <array>
#include <format>

#include "painter/tui/glyph.h"
#include "painter/tui/text.h"

#include "board_view.h"
#include "hud.h"
#include "keys.h"
#include "layout.h"
#include "pages.h"
#include "sea.h"
#include "ship_art.h"

namespace seabattle {

namespace {

namespace tui = painter::tui;
namespace core = painter::core;

class PlacePage final : public Page {
public:
    explicit PlacePage(Context &Shared);

    void Key(const tui::KeyEvent &Pressed) override;

protected:
    void Pixels(Canvas &Out) override;
    void Words(tui::Frame &Out) override;

private:
    Board &Mine() const { return Ctx.Game->At(Ctx.Placing).Waters; }
    const Rules &Played() const { return Ctx.Game->Chosen(); }
    int  Length() const { return Waiting.empty() ? 0 : Waiting[static_cast<std::size_t>(Choice)]; }
    bool Fine() const { return !Waiting.empty() && Mine().CanPlace(Row, Col, Length(), Vertical, Played().Touching); }
    void Fit();
    void Move(int Down, int Across);
    void Turn();
    void Put();
    void Undo();
    void Clear();
    void Shuffle();
    void Pick(int By);
    void Keep(std::string Words, bool Good) { Said = std::move(Words); SaidGood = Good; }
    std::string Reason() const;
    void PaintForbidden(Canvas &Out) const;
    void PaintFleet(tui::Frame &Out, const core::Rect &Inside) const;

    GridView         View;
    std::vector<int> Waiting;
    int              Choice = 0;
    int              Row = 0, Col = 0;
    bool             Vertical = false;
    std::string      Said;
    bool             SaidGood = true;
    bool             Help = false;
    LeavePrompt      Leaving;
};

PlacePage::PlacePage(Context &Shared) : Page(Shared)
{
    View = PlacementFor(Shared.Screen.Size(), Shared.Game->Chosen().Grid);
    const std::span<const int> Lengths = FleetLengths(Played().Fleet);
    Waiting.assign(Lengths.begin(), Lengths.end());
    Row = Col = Shared.Game->Chosen().Grid / 2 - 1;
    Fit();
    Keep(std::format("{}, lay your fleet. Move the ship, turn it, put it down.", Shared.Game->At(Shared.Placing).Name), true);
}

void PlacePage::Fit()
{
    const int Size = Played().Grid;
    const int Long = std::max(1, Length());
    Row = std::clamp(Row, 0, Size - (Vertical ? Long : 1));
    Col = std::clamp(Col, 0, Size - (Vertical ? 1 : Long));
}

void PlacePage::Move(int Down, int Across)
{
    Row += Down;
    Col += Across;
    Fit();
}

void PlacePage::Turn()
{
    Vertical = !Vertical;
    Fit();
}

void PlacePage::Pick(int By)
{
    if (Waiting.empty()) return;
    const int Now = Length();
    const int Count = static_cast<int>(Waiting.size());
    int       Next = Choice;
    for (int Step = 0; Step < Count; Step++) {
        Next = (Next + By + Count) % Count;
        if (Waiting[static_cast<std::size_t>(Next)] != Now) break;
    }
    Choice = Next;
    Fit();
}

std::string PlacePage::Reason() const
{
    for (int At = 0; At < Length(); At++)
        if (Mine().ShipAt(Vertical ? Row + At : Row, Vertical ? Col : Col + At) >= 0) return "Another ship is already there.";
    return "Too close to another ship: ships may not touch, even at the corners.";
}

void PlacePage::Put()
{
    if (Waiting.empty()) return;
    if (!Fine()) {
        Keep(Reason(), false);
        return;
    }
    Mine().Place(Row, Col, Length(), Vertical);
    Keep(std::format("{} laid.", ShipName(Length())), true);
    Waiting.erase(Waiting.begin() + Choice);
    Choice = std::min(Choice, std::max(0, static_cast<int>(Waiting.size()) - 1));
    if (Waiting.empty()) Keep("The fleet is laid. Enter confirms it; backspace takes the last ship back; a lays it again at random.", true);
    Fit();
}

void PlacePage::Undo()
{
    if (Mine().Ships().empty()) return;
    const int Taken = Mine().Ships().back().Length;
    Mine().UnplaceLast();
    Waiting.insert(std::ranges::find_if(Waiting, [Taken](int Length) { return Length <= Taken; }), Taken);
    Choice = static_cast<int>(std::ranges::find(Waiting, Taken) - Waiting.begin());
    Keep(std::format("{} taken back.", ShipName(Taken)), true);
    Fit();
}

void PlacePage::Clear()
{
    Mine().ClearShips();
    const std::span<const int> Lengths = FleetLengths(Played().Fleet);
    Waiting.assign(Lengths.begin(), Lengths.end());
    Choice = 0;
    Keep("Every ship is back in the harbour.", true);
    Fit();
}

void PlacePage::Shuffle()
{
    Mine().ClearShips();
    if (!Mine().Fill(Ctx.Chance, FleetLengths(Played().Fleet), Played().Touching)) {
        Clear();
        Keep("No room was found for the fleet.", false);
        return;
    }
    Waiting.clear();
    Choice = 0;
    Keep("The fleet is laid at random. Enter confirms it; a lays it again.", true);
}

void PlacePage::Key(const tui::KeyEvent &Pressed)
{
    if (Leaving.Open()) {
        if (Leaving.Answer(Pressed) == LeavePrompt::Verdict::Leave) Ctx.Go(PageId::Title);
        return;
    }
    if (Help) {
        Help = false;
        return;
    }
    if (Chose(Ctx.Chosen, KeyHelp, Pressed)) Help = true;
    else if (Chose(Ctx.Chosen, KeyRotate, Pressed)) Turn();
    else if (Chose(Ctx.Chosen, KeyAuto, Pressed)) Shuffle();
    else if (Chose(Ctx.Chosen, KeyUndo, Pressed)) Undo();
    else if (Chose(Ctx.Chosen, KeyClear, Pressed)) Clear();
    else if (Pressed.IsExtended()) {
        switch (Pressed.Scancode) {
        case tui::KeyCodes::Up:       Move(-1, 0); break;
        case tui::KeyCodes::Down:     Move(1, 0); break;
        case tui::KeyCodes::Left:     Move(0, -1); break;
        case tui::KeyCodes::Right:    Move(0, 1); break;
        case tui::KeyCodes::PageUp:   Pick(-1); break;
        case tui::KeyCodes::PageDown: Pick(1); break;
        default: break;
        }
    } else if (Pressed.IsCtrl() || Pressed.IsAlt()) {
        return;
    } else if (Pressed.Character == tui::KeyCodes::Escape) {
        Leaving.Ask();
    } else if (Pressed.Character == tui::KeyCodes::Enter) {
        if (Waiting.empty()) Ctx.FleetLaid();
        else Put();
    } else if (Pressed.Character == ' ') {
        Put();
    } else if (Pressed.Character == tui::KeyCodes::Tab || Pressed.Character == 'r' || Pressed.Character == 'R') {
        Turn();
    } else if (Pressed.Character == tui::KeyCodes::Backspace || Pressed.Character == 'u' || Pressed.Character == 'U') {
        Undo();
    } else if (Pressed.Character == 'a' || Pressed.Character == 'A') {
        Shuffle();
    } else if (Pressed.Character == 'c' || Pressed.Character == 'C') {
        Clear();
    } else if (Pressed.Character == '+' || Pressed.Character == '=') {
        Pick(1);
    } else if (Pressed.Character == '-') {
        Pick(-1);
    }
}

// Where no ship may be laid: round the ships already down when they may not touch.
void PlacePage::PaintForbidden(Canvas &Out) const
{
    if (Played().Touching) return;
    for (int Down = 0; Down < View.Size; Down++) {
        for (int Across = 0; Across < View.Size; Across++) {
            if (Mine().ShipAt(Down, Across) >= 0 || !Mine().NextToShip(Down, Across)) continue;
            for (int Y = 0; Y < View.Cell; Y++) {
                for (int X = 0; X < View.Cell; X++) {
                    const bool Stripe = ((X + Y) % 4) == 0;
                    Out.Blend(View.X + Across * View.Cell + X, View.Y + Down * View.Cell + Y, Ctx.Chosen.Bad, Stripe ? 70 : 20);
                }
            }
        }
    }
}

void PlacePage::Pixels(Canvas &Out)
{
    PaintBackdrop(Out, Ctx.Chosen.Sea, Ctx.Text(), Ctx.Now);
    PaintBoard(Out, BoardScene{.Waters = Mine(), .At = View, .Shown = Ctx.Chosen, .Ms = Ctx.Now, .Text = Ctx.Text(), .ShowShips = true});
    PaintForbidden(Out);
    if (Waiting.empty()) return;
    const bool Fits = Fine();
    DrawShip(Out, ShipDraw{.X = View.X + Col * View.Cell, .Y = View.Y + Row * View.Cell, .Cell = View.Cell, .Length = Length(), .Vertical = Vertical,
                           .Steel = Ctx.Chosen.Ship, .Water = Ctx.Chosen.Sea, .Ms = Ctx.Now,
                           .Tint = Fits ? Ctx.Chosen.Good : Ctx.Chosen.Bad, .TintShare = Fits ? 40 : 110, .Alpha = 0.9f, .Plain = Ctx.Text()});
}

void PlacePage::PaintFleet(tui::Frame &Out, const core::Rect &Inside) const
{
    const Look &Shown = Ctx.Chosen;
    const Color Paper = Mixed(Shown.Bars, Shown.Sea, 0.16f);
    std::array<int, LongestShip + 1> Total{}, Left{};
    for (const int One : FleetLengths(Played().Fleet)) Total[static_cast<std::size_t>(One)]++;
    for (const int One : Waiting) Left[static_cast<std::size_t>(One)]++;
    std::int32_t Line = Inside.Y + 1;
    for (int Long = LongestShip; Long >= 1; Long--) {
        const auto Index = static_cast<std::size_t>(Long);
        if (Total[Index] == 0) continue;
        const bool Chosen = !Waiting.empty() && Long == Length();
        const Color Under = Chosen ? Mixed(Paper, Shown.Title, 0.2f) : Paper;
        Out.Fill(core::Rect{Inside.X, Line, Inside.Width, 1}, ' ', {Shown.Text, Under});
        if (Chosen) Out.Put({Inside.X, Line}, tui::Glyphs::Pointer, {Shown.Title, Under});
        for (int At = 0; At < Long; At++) Out.Put({Inside.X + 2 + At, Line}, tui::Glyphs::Full, {Left[Index] > 0 ? Shown.Ship : Shown.Quiet, Under});
        tui::PutText(Out, core::Point{Inside.X + 2 + LongestShip + 1, Line}, ShipName(Long), 12, Left[Index] > 0 ? Shown.Text : Shown.Quiet, Under);
        tui::PutText(Out, core::Point{Inside.X + Inside.Width - 11, Line}, std::format("{}/{} laid", Total[Index] - Left[Index], Total[Index]), 10,
                     Left[Index] > 0 ? Shown.Title : Shown.Good, Under);
        Line += 2;
    }
    Line++;
    for (const std::string &Rule : {std::string(Played().Touching ? "Ships may touch." : "Ships may not touch."),
                                    std::string(Played().Salvo ? "Salvo: a shot for each ship afloat." : (Played().ExtraShot ? "A hit earns another shot." : "One shot a turn."))})
        tui::PutText(Out, core::Point{Inside.X + 1, Line++}, Rule, Inside.Width - 2, Shown.Quiet, Paper);
}

void PlacePage::Words(tui::Frame &Out)
{
    const Look &Shown = Ctx.Chosen;
    const core::Size Size = Out.Size();
    const Player &Who = Ctx.Game->At(Ctx.Placing);
    PaintBar(Out, Shown, "PLACE YOUR FLEET", Who.Name);
    LabelBoard(Out, Shown, View, std::format("{}'s waters", Who.Name), Waiting.empty() ? -1 : Row, Waiting.empty() ? -1 : Col);

    const std::int32_t Left = View.Frame().X + View.Frame().Width + 4;
    const std::int32_t Wide = std::min(Size.Width - Left - 3, 50);
    if (Wide >= 30) {
        const core::Rect Inside = PaintPanel(Out, Shown, core::Rect{Left, 3, Wide, std::min(Size.Height - 8, 20)}, "YOUR FLEET", "");
        PaintFleet(Out, Inside);
    }
    PaintSaid(Out, Size.Height - 2, Said, SaidGood ? Shown.Text : Shown.Bad, Shown.Bars);
    PaintHints(Out, Shown, {{"\xE2\x86\x95\xE2\x86\x94", "move", 1}, {"enter", Waiting.empty() ? "ready" : "put down", 0, Waiting.empty()}, {"r", "turn", 2}, {"u", "undo", 4},
                            {"a", "random", 3}, {"c", "clear", 6}, {"pgup/dn", "ship", 5}, {Spelled(Shown, KeyHelp), "keys", 7}, {"esc", "leave", 8}});
    if (Help) {
        const KeyLine Lines[] = {{"arrows", "move the ship over the waters; it stays inside them"},
                                 {"enter, space", "put the ship down; with the whole fleet laid, enter says you are ready"},
                                 {"r, tab, " + Spelled(Shown, KeyRotate), "turn the ship across"},
                                 {"pgup, pgdn, +, -", "take another of the ships still in the harbour"},
                                 {"u, backspace, " + Spelled(Shown, KeyUndo), "take the last ship back"},
                                 {"c, " + Spelled(Shown, KeyClear), "take every ship back"},
                                 {"a, " + Spelled(Shown, KeyAuto), "lay the whole fleet at random"},
                                 {"esc", "leave the game"}};
        PaintKeys(Out, Shown, "KEYS WHILE LAYING THE FLEET", Lines);
    }
    Leaving.Paint(Out, Shown);
}

}  // namespace

std::unique_ptr<Page> MakePlace(Context &Shared)
{
    return std::make_unique<PlacePage>(Shared);
}

}  // namespace seabattle
