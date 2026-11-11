/* game.h — the table in play: the roll, the picture, and the keys. */
#ifndef DICE_GAME_H
#define DICE_GAME_H

#include <cstdint>
#include <string>

#include "box/cxx/settings.h"
#include "painter/core/geometry.h"
#include "painter/tui/stage.h"

#include "expression.h"
#include "look.h"
#include "panel.h"
#include "roll.h"
#include "source.h"
#include "table.h"

namespace dice {

class Game {
public:
    // Start rolled on Screen; Notice, when it is not empty, is the first word said.
    Game(painter::tui::Stage Screen, Source &Dice, Expression Start, std::string Notice);
    // Until the person leaves, or the table is asked to finish.
    void Run();

private:
    void Arrange();
    void Again();
    void Submit();
    bool OnKey(const painter::tui::KeyEvent &Key);
    std::string Spelled(DiceKey Key) const;

    void Paint(std::uint64_t ElapsedMs);
    void PaintBars(painter::tui::Frame &Out, int Rows);
    void PaintHelp(painter::tui::Frame &Out);

    painter::tui::Stage  Screen;
    box::settings::watch Watch;
    Look                 Shown;
    Source              &Dice;
    Expression           Current;
    dice::Roll           Thrown;
    Table                Board;
    Panel                Content;
    painter::core::Rect  TableArea;
    painter::core::Rect  PanelArea;
    std::string          Typed;
    std::string          Said;           // the latest word, an error as a rule
    std::uint64_t        ThrownAtUs = 0;
    bool                 Help = false;
    bool                 Dirty = true;
};

}  // namespace dice

#endif
