/* boards.h — what the game says over the world: the score while it flies,
 * "Get Ready" before, "Game Over" after with the board of the score, the best
 * and the medal, the pause, the flash of a crash and the curtain between two
 * rounds. Their comings and goings the display plays in time. */
#ifndef FLAPPYBIRD_BOARDS_H
#define FLAPPYBIRD_BOARDS_H

#include <cstdint>
#include <vector>

#include "painter/gui/area.h"
#include "painter/gui/items.h"

namespace flappybird {

namespace gui = painter::gui;

/* The number the curtain's closing is told back by, as an animation's end. */
inline constexpr std::uint32_t CurtainClosed = 1;

class Boards {
public:
    Boards(gui::Area &Screen, float Unit, float Wide);

    /* Before the first flap: "Get Ready", how to fly, the best so far. */
    void Ready(unsigned Best);
    /* The round begins: the words go, the score comes. */
    void Flying();
    void Score(unsigned Points);
    /* The bird struck something: the screen flashes white. */
    void Flash();
    /* On the ground: "Game Over" drops in and the board rises, its score
     * counted up from nothing (Count); Medal by the score, New when it beat
     * the best. */
    void Over(unsigned Points, unsigned Best, bool New);
    void Count(unsigned Shown);
    void Paused(bool On);
    /* The curtain closes over the round, told back as CurtainClosed, and
     * opens on the next. */
    void CloseCurtain();
    void OpenCurtain();

private:
    gui::Text Line(std::string_view Words, float Size, painter::core::Color With, float Y);
    void      MakeBoard();
    void      MakeMedal();
    void      ShowMedal(unsigned Points);

    gui::Area &Screen;
    float      U, Wide;
    gui::Text  Points, Title, Hint, Best, Pause, PauseHint, OverTitle, OverHint;
    gui::Group Board;
    gui::Text  ScoreLabel, ScoreNumber, BestLabel, BestNumber, MedalLabel, NewWord;
    gui::Shape Plate, NewBadge, Socket;
    gui::Group Medal;
    gui::Shape Disc, Ring, Star, Sparkle;
    gui::Shape Blink, Curtain;
};

}  // namespace flappybird

#endif
