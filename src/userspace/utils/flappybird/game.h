/* game.h — a game on the screen: the world, its picture and its words, the
 * keys, and the frames — one each time the display shows the one before,
 * while anything moves; none while nothing does. */
#ifndef FLAPPYBIRD_GAME_H
#define FLAPPYBIRD_GAME_H

#include <cstdint>
#include <optional>
#include <string>

#include "painter/gui/area.h"
#include "painter/tui/keyboard.h"

#include "boards.h"
#include "scene.h"
#include "world.h"

namespace flappybird {

class Game {
public:
    Game(gui::Area Screen, painter::tui::Keyboard Keys);

    /* Until the person leaves. */
    void Run();
    /* What went wrong with the record, said once the glass is given back. */
    const std::string &Trouble() const noexcept { return Problem; }

private:
    /* The world, its picture and its words made for the screen as it is. */
    void Build();
    /* Whether frames are wanted now: something moves. */
    bool Moving() const;
    void Step();
    void CountUp();
    bool OnKey(const painter::tui::KeyEvent &Key);
    void OnArea(const gui::Event &Told);
    void Flap();
    void Land();
    void Again();
    void SetPaused(bool On);

    gui::Area              Screen;
    painter::tui::Keyboard Keys;
    std::optional<World>   Field;
    std::optional<Scene>   Picture;
    std::optional<Boards>  Words;
    painter::core::PointF  BuiltFor{};
    unsigned               Best    = 0;
    unsigned               Counted = 0;      // of the score, how much the board has counted up
    bool                   Paused  = false;
    bool                   Hidden  = false;  // the game is behind another program
    bool                   Closing = false;  // the curtain is closing on the round
    std::uint64_t          LastUs  = 0;      // the moment of the last frame
    std::uint64_t          OverUs  = 0;      // when the bird landed
    std::string            Problem;
};

}  // namespace flappybird

#endif
