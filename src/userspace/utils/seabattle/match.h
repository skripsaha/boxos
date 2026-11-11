/* match.h — one game between two players: whose turn it is, how many shots
 * are left in it, and what each player has done. Nothing of the picture. */
#ifndef SEABATTLE_MATCH_H
#define SEABATTLE_MATCH_H

#include <string>

#include "board.h"
#include "rules.h"

namespace seabattle {

struct Stats {
    int Shots = 0;
    int Hits = 0;
    int Sunk = 0;
    int Turns = 0;
    int Streak = 0;         // hits in a row, now
    int BestStreak = 0;

    int AccuracyPercent() const { return Shots == 0 ? 0 : Hits * 100 / Shots; }
};

struct Player {
    std::string Name;
    Board       Waters;     // this player's own fleet and the shots fired at it
    Stats       Record;
    bool        Computer = false;
};

// What a shot did, and what comes of it.
struct Report {
    int        Row = 0, Col = 0;
    ShotResult Shot{};
    bool       Again = false;   // the shooter has another shot
    bool       Won = false;
};

class Match {
public:
    Match(const Rules &Chosen, std::string One, std::string Two, bool AgainstComputer);

    const Rules &Chosen() const { return Played; }
    Player      &At(int Which) { return Players[Which]; }
    const Player &At(int Which) const { return Players[Which]; }
    int          Current() const { return Shooter; }
    Player      &Shooting() { return Players[Shooter]; }
    Player      &Aimed() { return Players[1 - Shooter]; }
    const Player &Aimed() const { return Players[1 - Shooter]; }
    int          ShotsLeft() const { return Left; }
    int          TurnNumber() const { return Turn; }
    bool         Over() const { return Finished; }
    int          Winner() const { return Shooter; }
    bool         VsComputer() const { return Players[1].Computer; }

    // Whether the shooter may fire at the cell: it is in the grid and nothing is known of it.
    bool CanFire(int Row, int Col) const;
    // The first player begins, once both fleets are laid.
    void Start() { BeginTurn(); }
    // The shooter fires. The turn is not passed: see PassTurn.
    Report Fire(int Row, int Col);
    // The other player is to shoot, as many times as the rules give.
    void PassTurn();

private:
    void BeginTurn();

    Rules  Played;
    Player Players[2];
    int    Shooter = 0;
    int    Left = 1;
    int    Turn = 0;
    bool   Finished = false;
};

}  // namespace seabattle

#endif
