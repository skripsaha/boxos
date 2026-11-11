/* toss.h — one toss of the coin, worked out whole before it starts: the face
 * it will lie on is drawn first, and the turns it makes are the ones that
 * bring that face up, so what is shown is what was drawn. */
#ifndef COINFLIP_TOSS_H
#define COINFLIP_TOSS_H

#include "look.h"

namespace coinflip {

enum class Face { Heads, Tails };

// Where the coin is at one moment of its toss.
struct Pose {
    double Lift;     // from 0, on the floor, to 1, the top of the first arc
    double Turn;     // radians about the line from left to right; 0 shows heads, pi tails
    bool   Landed;   // it lies, and moves no more
};

// Up in one arc, down, and bounces each lower and shorter than the last, while
// the turn slows to nothing.
class Toss {
public:
    Toss(Face Result, const Look &Shown, bool Quick);

    Pose   At(double Seconds) const;
    double Length() const { return Total; }

private:
    double ArcSeconds;   // the first arc, from the hand to the floor
    int    Arcs;         // the first and its bounces
    double Total;        // all of them
    double Turns;        // radians the coin turns in all
};

}  // namespace coinflip

#endif
