/* shower.h — the coins that fall over the glass for a big win: they drop from
 * the top at different places and speeds, turn as they fall, and are gone at
 * the bottom. */
#ifndef CASINO_SHOWER_H
#define CASINO_SHOWER_H

#include <vector>

#include "canvas.h"
#include "luck.h"

namespace casino {

class Shower {
public:
    // Coins to fall over Seconds from now.
    void Begin(int Coins, float Seconds);
    // Time passes over a glass Wide pixels across and High down.
    void Step(float Seconds, std::int32_t Wide, std::int32_t High, Luck &Chance);
    void Paint(Canvas &Draw, Color Gold) const;
    // Coins still to come or still falling.
    bool Falling() const { return Left > 0 || !Coins.empty(); }

private:
    struct Coin {
        float Across;
        float Down;
        float Drift;     // pixels a second sideways
        float Fall;      // and down: grows as it falls
        float Turn;      // where it is in its turn
        float Size;
    };

    std::vector<Coin> Coins;
    float             Left = 0;      // coins not yet dropped
    float             Rate = 0;      // dropped a second
    float             Owed = 0;      // the part of a coin the time passed has not yet made a whole one
};

}  // namespace casino

#endif
