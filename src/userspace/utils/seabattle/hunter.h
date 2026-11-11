/* hunter.h — the computer's aim. For every ship still afloat it counts the
 * ways that ship could lie among the cells nothing rules out, and fires where
 * the most ways cross; a way that runs through a hit not yet sunk counts many
 * times over, so a wounded ship is finished along its line before the hunt
 * goes on. It knows only what a player knows: the shots and the ships' sizes. */
#ifndef SEABATTLE_HUNTER_H
#define SEABATTLE_HUNTER_H

#include "board.h"

namespace seabattle {

struct Aim {
    int Row, Col;
};

// The cell to fire at on Waters, which has one left that nothing is known of.
Aim Hunt(const Board &Waters, Luck &Chance);

}  // namespace seabattle

#endif
