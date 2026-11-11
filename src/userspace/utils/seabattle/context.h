/* context.h — what every page of the sea battle shares: the glass, the
 * settings, the chances, the game being played and the way from one page to
 * the next. The pages are in page.h and pages.h, and each asks for the next through Go. */
#ifndef SEABATTLE_CONTEXT_H
#define SEABATTLE_CONTEXT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "box/cxx/settings.h"
#include "painter/tui/stage.h"

#include "canvas.h"
#include "look.h"
#include "luck.h"
#include "match.h"
#include "records.h"

namespace seabattle {

enum class PageId : std::uint8_t { Title, Setup, Rules, Hall, Handover, Place, Battle, Victory, Quit };

// What a hand-over of the screen is for.
enum class Errand : std::uint8_t { PlaceFleet, TakeTurn };

// What is being set up for the next game.
struct Setup {
    std::string NameOne;
    std::string NameTwo;
    bool        VsComputer = false;
    Rules       Chosen;
    Pace        Speed = Pace::Normal;
};

inline constexpr const char *ComputerName = "Admiral Bot";
inline constexpr std::size_t MostName = 14;

class Context {
public:
    Context(painter::tui::Stage Opened, const std::string &First, const std::string &Second);

    bool Text() const { return Screen.Glass() == painter::tui::GlassKind::Text; }
    // The page to show next; Quit leaves the program.
    void Go(PageId Page) { Asked = Page; }
    std::optional<PageId> TakeAsked();
    bool Asking() const { return Asked.has_value(); }

    // A new game from the setup: its fleets are to be laid.
    void StartGame();
    // The player placing has laid the fleet and said so.
    void FleetLaid();
    // The player's turn is over; the other is to shoot.
    void TurnPassed();
    // The game has been won: it is written in the hall and the winner shown.
    void GameEnded();
    // Whose hands the screen is to be passed to.
    int HandTo() const;
    // Who looks at the glass: the one shooting, or in a game against the computer the person.
    int Viewer() const;

    painter::tui::Stage  Screen;
    box::settings::watch Watch;
    Look                 Chosen;
    Luck                 Chance;
    Records              Hall;
    Setup                Plan;
    std::unique_ptr<Match> Game;
    Canvas               Picture;
    std::uint64_t        Now = 0;
    Errand               Purpose = Errand::PlaceFleet;
    int                  Placing = 0;        // the player laying a fleet
    std::string          Recap;              // what the last player did, for the one who is next

private:
    std::optional<PageId> Asked;
};

}  // namespace seabattle

#endif
