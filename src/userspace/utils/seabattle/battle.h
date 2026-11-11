/* battle.h — one turn of the battle: the player at the screen sees the enemy
 * waters, where the aim is taken, and their own fleet, where the enemy's
 * shots fell. A shot is aimed, fired, flies, falls, and is counted; the turn
 * goes on while the rules give another shot and ends when they do not. Against
 * the computer its turns are shown to the person in the same way, shot by shot. */
#ifndef SEABATTLE_BATTLE_H
#define SEABATTLE_BATTLE_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "board_view.h"
#include "effects.h"
#include "keys.h"
#include "layout.h"
#include "page.h"

namespace seabattle {

enum class Phase : std::uint8_t { Aim, Thinking, Flight, Impact, Sinking, Settle, Hold };

class BattlePage final : public Page {
public:
    explicit BattlePage(Context &Shared);

    void Step(std::uint32_t ElapsedMs) override;
    void Key(const painter::tui::KeyEvent &Pressed) override;
    std::uint32_t FrameMs() const override { return Fx.Busy() ? 60 : 125; }

protected:
    void Pixels(Canvas &Out) override;
    void Shapes(painter::tui::FineCanvas &Out) override;
    void Words(painter::tui::Frame &Out) override;

private:
    // battle.cpp: the turn.
    Match &Game() const { return *Ctx.Game; }
    bool   PersonShoots() const { return Game().Current() == Viewer; }
    const GridView &Target() const { return PersonShoots() ? Place.Aim : Place.Own; }
    void   Enter(Phase Next, std::uint32_t Ms);
    void   Aimed();
    void   Fire(int Row, int Col);
    void   Land();
    void   Impacted();
    void   Settle();
    void   Next();
    void   Close();
    void   Move(int Down, int Across);
    std::string Spoken(const Report &Result) const;

    // battle_paint.cpp: the picture. Until a shot has fallen, what it did is not yet shown: not on the
    // waters, not in the fleets afloat, not in the figures.
    const Board &WatersOf(int Side) const;
    const Stats &FiguresOf(int Side) const;
    BoardScene SceneOf(bool Enemy) const;
    void PaintCursor(Canvas &Out) const;
    void PaintRoster(painter::tui::Frame &Out, const Board &Waters, const GridView &At, bool Own) const;
    void PaintFigures(painter::tui::Frame &Out) const;
    void PaintHelp(painter::tui::Frame &Out) const;

    int                  Viewer;
    Arena                Place;
    Phase                Now = Phase::Aim;
    std::uint64_t        Since = 0;
    std::uint32_t        Span = 0;
    int                  CursorRow = 0;
    int                  CursorCol = 0;
    Report               Last;
    std::optional<Board> Before;       // the waters shot at, as they were before the shot, until it has fallen
    std::optional<std::array<Stats, 2>> Counted;   // and the figures of both players
    Effects              Fx;
    std::string          Said;
    bool                 SaidGood = true;
    std::vector<std::string> Calls;     // this turn's shots, for whoever is next
    bool                 Help = false;
    LeavePrompt          Leaving;
};

}  // namespace seabattle

#endif
