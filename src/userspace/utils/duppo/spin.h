/* spin.h — how Duppo is turned in space. It is a ball: its eye is on one side
 * of it, and it turns about all three axes — a yaw and a pitch that carry the
 * eye round the ball and let it go over the edge and come back, and a roll in
 * the plane of the glass. It leans into the way it goes (faces it, tilts to
 * it), drifts about of itself, and now and then turns to look at something.
 * The angles are sprung, so a turn eases in and overshoots a little. */
#ifndef DUPPO_SPIN_H
#define DUPPO_SPIN_H

#include "flight.h"
#include "vec.h"

namespace duppo {

class Spin {
public:
    /* How fast it lives, as a share of the usual. */
    void Tune(float Speed) { Pace = Speed; }
    void Step(float Seconds, Random &Dice, const Flight &Body);

    float    Yaw = 0.0f, Pitch = 0.0f, Roll = 0.0f;   /* radians */
    Rotation Pose;                                    /* the three together, body into view */

private:
    void Look(Random &Dice);

    float Rate[3]  = {0.0f, 0.0f, 0.0f};
    float Pace     = 1.0f;
    float Clock    = 0.0f;
    float ToLook   = 2.0f;
    float Holding  = 0.0f;
    float LookYaw  = 0.0f, LookPitch = 0.0f;
};

}  // namespace duppo

#endif
