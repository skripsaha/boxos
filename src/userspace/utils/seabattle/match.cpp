/* match.cpp — the turns of a game and what is counted in them. */

#include "match.h"

#include <algorithm>
#include <utility>

namespace seabattle {

Match::Match(const Rules &Chosen, std::string One, std::string Two, bool AgainstComputer)
    : Played(Chosen), Players{Player{std::move(One), Board(Chosen.Grid), {}, false},
                              Player{std::move(Two), Board(Chosen.Grid), {}, AgainstComputer}}
{
}

bool Match::CanFire(int Row, int Col) const
{
    const Board &Waters = Aimed().Waters;
    return Waters.Inside(Row, Col) && Waters.MarkAt(Row, Col) == Mark::None;
}

void Match::BeginTurn()
{
    Turn++;
    Players[Shooter].Record.Turns++;
    Left = Played.Salvo ? std::max(1, Players[Shooter].Waters.Afloat()) : 1;
}

void Match::PassTurn()
{
    Shooter = 1 - Shooter;
    BeginTurn();
}

Report Match::Fire(int Row, int Col)
{
    Report Result{.Row = Row, .Col = Col};
    Result.Shot = Aimed().Waters.Fire(Row, Col, Played.Touching);
    Stats &Mine = Shooting().Record;
    Mine.Shots++;
    if (Result.Shot.Result == Outcome::Miss) {
        Mine.Streak = 0;
    } else {
        Mine.Hits++;
        Mine.BestStreak = std::max(Mine.BestStreak, ++Mine.Streak);
        if (Result.Shot.Result == Outcome::Sunk) Mine.Sunk++;
    }
    if (Aimed().Waters.AllSunk()) {
        Finished = true;
        Result.Won = true;
        return Result;
    }
    Left--;
    Result.Again = Played.Salvo ? Left > 0 : (Played.ExtraShot && Result.Shot.Result != Outcome::Miss);
    return Result;
}

}  // namespace seabattle
