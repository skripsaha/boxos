/* context.cpp — the way a game goes from page to page (context.h). */

#include "context.h"

#include <utility>

namespace seabattle {

Context::Context(painter::tui::Stage Opened, const std::string &First, const std::string &Second)
    : Screen(std::move(Opened)), Chosen(Adapted(ReadLook(), Screen.Glass() == painter::tui::GlassKind::Text)), Picture(Screen.Size().Width, Screen.Size().Height * 2)
{
    Plan.NameOne = (First.empty() ? Chosen.NameOne : First).substr(0, MostName);
    Plan.NameTwo = (Second.empty() ? Chosen.NameTwo : Second).substr(0, MostName);
    Plan.Chosen = Chosen.Start;
    Plan.Speed = Chosen.Speed;
}

std::optional<PageId> Context::TakeAsked()
{
    std::optional<PageId> Page = Asked;
    Asked.reset();
    return Page;
}

int Context::HandTo() const
{
    return Purpose == Errand::PlaceFleet ? Placing : Game->Current();
}

int Context::Viewer() const
{
    return Game->VsComputer() ? 0 : Game->Current();
}

void Context::StartGame()
{
    Game = std::make_unique<Match>(Plan.Chosen, Plan.NameOne, Plan.VsComputer ? std::string(ComputerName) : Plan.NameTwo, Plan.VsComputer);
    Placing = 0;
    Recap.clear();
    if (Plan.VsComputer) {
        Go(PageId::Place);
        return;
    }
    Purpose = Errand::PlaceFleet;
    Go(PageId::Handover);
}

void Context::FleetLaid()
{
    if (Placing == 0 && !Game->VsComputer()) {
        Placing = 1;
        Purpose = Errand::PlaceFleet;
        Go(PageId::Handover);
        return;
    }
    if (Game->VsComputer() && !Game->At(1).Waters.Fill(Chance, FleetLengths(Plan.Chosen.Fleet), Plan.Chosen.Touching)) {
        Go(PageId::Setup);
        return;
    }
    Game->Start();
    Purpose = Errand::TakeTurn;
    Go(Game->VsComputer() ? PageId::Battle : PageId::Handover);
}

void Context::TurnPassed()
{
    Game->PassTurn();
    Purpose = Errand::TakeTurn;
    Go(Game->VsComputer() ? PageId::Battle : PageId::Handover);
}

void Context::GameEnded()
{
    const int Winner = Game->Winner();
    for (int Which = 0; Which < 2; Which++) {
        const Player &Each = Game->At(Which);
        if (!Each.Computer) Hall.Add(Each.Name, Which == Winner, Each.Record.Shots, Each.Record.Hits);
    }
    Go(PageId::Victory);
}

}  // namespace seabattle
