/* face.h — what Duppo's eye does: where it looks and how shut the lids are.
 * It looks along the way it goes, and while it rests it glances about the
 * glass and now and then at the place it will go next; the gaze is sprung, so
 * it overshoots a little as a quick look does. It blinks now and then, and
 * sometimes twice. */
#ifndef DUPPO_FACE_H
#define DUPPO_FACE_H

#include "painter/core/geometry.h"

#include "flight.h"
#include "vec.h"

namespace duppo {

class Face {
public:
    void Begin(painter::core::Size Cells);
    /* How fast it lives, as a share of the usual, and how often it blinks,
     * as a share of the usual; none keeps the eye open. */
    void Tune(float Speed, float Blinks);
    void Step(float Seconds, Random &Dice, const Flight &Body);

    Vec   Gaze;          /* within the unit circle, 1 being the way it can look at most */
    float Shut = 0.0f;   /* 0 open, 1 closed */

private:
    void Glance(Random &Dice, const Flight &Body);
    void Blink(float Seconds, Random &Dice);
    void Move(float Seconds, const Flight &Body);
    static float Lids(float Age);

    Vec   Glass;
    Vec   Looking;
    Vec   Quickness;
    float Pace      = 1.0f;
    float Blinks    = 1.0f;
    float ToGlance  = 1.0f;
    float ToBlink   = 2.0f;
    float Age       = -1.0f;   /* seconds into a blink, or -1 between them */
};

}  // namespace duppo

#endif
