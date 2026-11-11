/* game.h — the coin on the glass: a run of flips, each tossed, turning and
 * falling until it lies, counted when it does; and the keys. */
#ifndef COINFLIP_GAME_H
#define COINFLIP_GAME_H

#include <cstdint>
#include <optional>
#include <string>

#include "box/cxx/settings.h"
#include "painter/tui/frame.h"
#include "painter/tui/stage.h"

#include "look.h"
#include "luck.h"
#include "tally.h"
#include "toss.h"

namespace coinflip {

// What was asked on the line: how many flips to a run, and a call made before
// the first, which stands until another.
struct Plan {
    std::uint64_t        Count;
    std::optional<Face>  Called;
};

class Game {
public:
    Game(painter::tui::Stage Opened, Plan Wanted);

    void Run();

private:
    // game.cpp
    void StartRun(std::uint64_t Now);
    void Begin(std::uint64_t Now);
    void Land(std::uint64_t Now);
    void Record(Face Landed);
    void Hurry(std::uint64_t Now);
    void Advance(std::uint64_t Now);
    bool OnKey(const painter::tui::KeyEvent &Key, std::uint64_t Now);
    bool Flying() const { return Aloft; }
    bool Running() const { return Left > 0; }

    // game_paint.cpp
    void        Paint(std::uint64_t Now);
    void        PaintScene(std::uint64_t Now);
    void        PaintBars();
    void        PaintResult();
    void        PaintTally();
    void        PaintHelp();
    std::string Spelled(CoinKey Key) const;
    std::string Verdict() const;

    painter::tui::Stage Screen;
    Look                Shown;
    box::settings::watch Watch;
    Luck                Chance;
    Tally               Counted;
    Plan                Wish;

    std::optional<Toss> Current;        // the toss being shown, or the last one, lying
    Face                Landing   = Face::Heads;
    bool                Aloft     = false;
    std::uint64_t       StartedUs = 0;
    std::uint64_t       LandedUs  = 0;
    std::uint64_t       Left      = 0;   // flips of the run still to come, the one in the air included
    std::uint64_t       Done      = 0;   // flips of the run that have landed
    bool                Help      = false;
};

}  // namespace coinflip

#endif
