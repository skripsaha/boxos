/* orbit.h — the little bubbles that keep close to Duppo. Each has the place it
 * has in the logo as its home, in space: it swings about Duppo of itself (round
 * the upright, round the glass's across, a little in the plane), goes with
 * Duppo's own turn in part, so it passes behind the ball and comes in front of
 * it, and breathes in and out. A spring draws it to its home across the glass,
 * so it trails when Duppo goes and swings past when Duppo stops; the heavier
 * the bubble, the slower the spring. */
#ifndef DUPPO_ORBIT_H
#define DUPPO_ORBIT_H

#include "logo.h"
#include "spin.h"
#include "vec.h"

namespace duppo {

struct Moon {
    Vec   Center;
    Vec   Velocity;
    float Depth = 0.0f;   /* its middle's Z, in glass units: behind the ball when below 0 */
    float Grow  = 1.0f;   /* its size as a share of the logo's, nearer being larger */
};

class Orbit {
public:
    /* The bubbles at home about a Duppo at Center of the given size. */
    void Begin(Vec Center, float Size);
    /* How fast it lives, as a share of the usual, and how many bubbles there are, 0 to 3. */
    void Tune(float Speed, int Count);
    void Step(float Seconds, Vec Center, float Size, const Spin &Turning);

    int  Count = Logo::MoonCount;
    Moon Moons[Logo::MoonCount];

private:
    struct Home {
        Vec   Center;
        float Depth;
        float Grow;
    };

    Home Of(int Which, Vec Center, float Size, const Spin &Turning) const;

    float Pace  = 1.0f;
    float Clock = 0.0f;
};

}  // namespace duppo

#endif
