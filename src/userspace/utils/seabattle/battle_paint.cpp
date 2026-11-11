/* battle_paint.cpp — the battle as a picture: the two waters, the cursor,
 * the shell and its blast, the fleets still afloat, the figures and the words (battle.h). */

#include <algorithm>
#include <format>

#include "painter/tui/glyph.h"
#include "painter/tui/text.h"

#include "battle.h"
#include "hud.h"
#include "sea.h"

namespace seabattle {

namespace {

namespace tui = painter::tui;
namespace core = painter::core;

constexpr int GuideShare = 22;

}  // namespace

const Board &BattlePage::WatersOf(int Side) const
{
    return Before && Side == 1 - Game().Current() ? *Before : Game().At(Side).Waters;
}

const Stats &BattlePage::FiguresOf(int Side) const
{
    return Counted ? (*Counted)[static_cast<std::size_t>(Side)] : Game().At(Side).Record;
}

BoardScene BattlePage::SceneOf(bool Enemy) const
{
    const GridView &At = Enemy ? Place.Aim : Place.Own;
    const bool      Fired = Enemy == PersonShoots();
    const Board    &Waters = WatersOf(Enemy ? 1 - Viewer : Viewer);
    BoardScene      Scene{.Waters = Waters, .At = At, .Shown = Ctx.Chosen, .Ms = Ctx.Now, .Text = Ctx.Text(), .ShowShips = !Enemy};
    if (!Fired) return Scene;
    if (Before) {
        Scene.HideRow = Last.Row;
        Scene.HideCol = Last.Col;
    }
    if (Now == Phase::Sinking) {
        Scene.Sinking = Last.Shot.ShipIndex;
        Scene.Sink = Clamped(static_cast<float>(Ctx.Now - Since) / static_cast<float>(std::max<std::uint32_t>(1, Span)));
    }
    return Scene;
}

// The cursor over the waters being aimed at: its row and column lit, and brackets round its cell.
void BattlePage::PaintCursor(Canvas &Out) const
{
    const GridView &At = Place.Aim;
    const Color     Ink = Game().CanFire(CursorRow, CursorCol) ? Ctx.Chosen.Title : Ctx.Chosen.Bad;
    for (int Step = 0; Step < At.Pixels(); Step++) {
        Out.Blend(At.X + Step, At.Y + CursorRow * At.Cell + At.Cell / 2, Ink, GuideShare);
        Out.Blend(At.X + CursorCol * At.Cell + At.Cell / 2, At.Y + Step, Ink, GuideShare);
    }
    const int X0 = At.X + CursorCol * At.Cell, Y0 = At.Y + CursorRow * At.Cell, Edge = At.Cell - 1;
    const int Arm = std::max(1, At.Cell / 3);
    const float Level = 0.6f + 0.4f * Beat(Ctx.Now, 900);
    for (int Step = 0; Step <= Arm; Step++) {
        for (const int Along : {0, Edge}) {
            Out.Cover(X0 + Step, Y0 + Along, Ink, Level);
            Out.Cover(X0 + Edge - Step, Y0 + Along, Ink, Level);
            Out.Cover(X0 + Along, Y0 + Step, Ink, Level);
            Out.Cover(X0 + Along, Y0 + Edge - Step, Ink, Level);
        }
    }
}

void BattlePage::Pixels(Canvas &Out)
{
    PaintBackdrop(Out, Ctx.Chosen.Sea, Ctx.Text(), Ctx.Now);
    PaintBoard(Out, SceneOf(true));
    PaintBoard(Out, SceneOf(false));
    if (Now == Phase::Aim && PersonShoots()) PaintCursor(Out);
    Fx.DrawPixels(Out, Ctx.Now);
}

void BattlePage::Shapes(tui::FineCanvas &Out)
{
    ShapeBoard(Out, SceneOf(true));
    ShapeBoard(Out, SceneOf(false));
    Fx.DrawFine(Out, Ctx.Now);
    if (Now == Phase::Aim && PersonShoots()) {
        const GridView &At = Place.Aim;
        const float Size = static_cast<float>(At.Cell);
        const float Outer = Size * (0.36f + 0.04f * Beat(Ctx.Now, 900));
        Out.Ring(static_cast<float>(At.CentreX(CursorCol)), static_cast<float>(At.CentreY(CursorRow)), Outer, Outer - 0.9f, Ctx.Chosen.Title, 0.75f);
    }
}

// The ships of a fleet as rows of blocks: afloat, wounded or gone. One's own fleet shows its wounds; the enemy's only what is sunk.
void BattlePage::PaintRoster(tui::Frame &Out, const Board &Waters, const GridView &At, bool Own) const
{
    const Look &Shown = Ctx.Chosen;
    const core::Rect Box = At.Frame();
    std::int32_t Row = At.RowBelow(), Column = Box.X;
    for (const Ship &One : Waters.Ships()) {
        if (Column + One.Length > Box.X + Box.Width) {
            Row++;
            Column = Box.X;
        }
        for (int Cell = 0; Cell < One.Length; Cell++) {
            const bool Wounded = Own && Cell < One.Hits;
            const Color Ink = One.Sunk() ? Darker(Shown.Bad, 0.3f) : (Wounded ? Shown.Fire : Shown.Ship);
            Out.Put({Column + Cell, Row}, One.Sunk() ? tui::Glyphs::MidShade : tui::Glyphs::Full, {Ink, Shown.Bars});
        }
        Column += One.Length + 1;
    }
}

void BattlePage::PaintFigures(tui::Frame &Out) const
{
    const Look &Shown = Ctx.Chosen;
    const std::int32_t Row = Out.Size().Height - 3;
    // The streak is left out where the line would not fit beside the other player's.
    const auto Figures = [&](int Side, bool Streak) {
        const Stats &Held = FiguresOf(Side);
        std::string  Line = std::format("{}  shots {}  hits {}  {}%", Game().At(Side).Name, Held.Shots, Held.Hits, Held.AccuracyPercent());
        return Streak ? Line + std::format("  streak {}", Held.Streak) : Line;
    };
    Out.Fill(core::Rect{0, Row, Out.Size().Width, 1}, ' ', {Shown.Text, Shown.Bars});
    const std::int32_t Left = std::max(0, Place.Aim.Frame().X - 2);
    const std::int32_t Room = Out.Size().Width / 2 - Left - 1;
    const bool Streak = static_cast<std::int32_t>(std::max(tui::CellsOf(Figures(Viewer, true)), tui::CellsOf(Figures(1 - Viewer, true)))) <= Room;
    tui::PutText(Out, core::Point{Left, Row}, Figures(Viewer, Streak), Room, Game().Current() == Viewer ? Shown.Title : Shown.Text, Shown.Bars);
    const std::string Other = Figures(1 - Viewer, Streak);
    const auto        Wide = std::min(Room, static_cast<std::int32_t>(tui::CellsOf(Other)));
    tui::PutText(Out, core::Point{Out.Size().Width - Wide - Left, Row}, Other, Wide, Game().Current() != Viewer ? Shown.Title : Shown.Text, Shown.Bars);
}

void BattlePage::PaintHelp(tui::Frame &Out) const
{
    const Look &Shown = Ctx.Chosen;
    const KeyLine Lines[] = {{"arrows", "move the aim over the enemy waters; home, end, pgup and pgdn go to an edge"},
                             {"enter, space", "fire at the cell aimed at; on a hit the rules may give another shot"},
                             {"enter", "while the turn ends: do not wait for the screen to be passed"},
                             {Spelled(Shown, KeyHelp), "these keys"},
                             {"esc", "leave the game"}};
    PaintKeys(Out, Shown, "KEYS IN THE BATTLE", Lines);
}

void BattlePage::Words(tui::Frame &Out)
{
    const Look &Shown = Ctx.Chosen;
    const core::Size Size = Out.Size();
    const Player &Mine = Game().At(Viewer), &Theirs = Game().At(1 - Viewer);
    const std::string Note = Game().Chosen().Salvo && PersonShoots()
        ? std::format("turn {}: {} to fire, {} shots left", Game().TurnNumber(), Game().Shooting().Name, Game().ShotsLeft())
        : std::format("turn {}: {} to fire", Game().TurnNumber(), Game().Shooting().Name);
    PaintBar(Out, Shown, "SEA BATTLE", Note);
    const bool Aiming = Now == Phase::Aim && PersonShoots();
    const auto Afloat = [&](int Side) {
        return std::format("{} of {} ships afloat", WatersOf(Side).Afloat(), WatersOf(Side).Ships().size());
    };
    LabelBoard(Out, Shown, Place.Aim, std::format("{}'s waters", Theirs.Name), Aiming ? CursorRow : -1, Aiming ? CursorCol : -1, Afloat(1 - Viewer));
    LabelBoard(Out, Shown, Place.Own, std::format("{}'s fleet", Mine.Name), -1, -1, Afloat(Viewer));
    PaintRoster(Out, WatersOf(1 - Viewer), Place.Aim, false);
    PaintRoster(Out, WatersOf(Viewer), Place.Own, true);
    PaintFigures(Out);
    PaintSaid(Out, Size.Height - 2, Said, SaidGood ? Shown.Text : Shown.Bad, Shown.Bars);
    if (Aiming) PaintHints(Out, Shown, {{"\xE2\x86\x95\xE2\x86\x94", "aim", 1}, {"enter", "fire", 0, true}, {Spelled(Shown, KeyHelp), "keys", 2}, {"esc", "leave", 3}});
    else if (Now == Phase::Hold) PaintHints(Out, Shown, {{"enter", Game().VsComputer() ? "go on" : "pass the screen now", 0}, {"esc", "leave", 1}});
    else PaintHints(Out, Shown, {{"esc", "leave", 0}});
    if (Help) PaintHelp(Out);
    Leaving.Paint(Out, Shown);
}

}  // namespace seabattle
