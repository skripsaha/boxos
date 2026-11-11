/* air.h — what falls and what flashes in the forest's air: rain and its
 * splashes, snow, leaves and petals on the wind, lightning, and the small
 * things the animals leave in the air — a puff of powder, a note of song. */
#ifndef FOREST_AIR_H
#define FOREST_AIR_H

#include <vector>

#include "nature.h"
#include "numbers.h"
#include "scenery.h"

namespace forest {

// How long a bolt of lightning stays drawn, in seconds.
inline constexpr float BoltLasts = 0.28f;

struct Drop {
    float X, Y;
    float Speed;
    float Land;          // the row it falls to
};

struct Splash {
    float X, Y;
    float Age;
};

struct Flake {
    float X, Y;
    float Speed;
    float Phase;
    bool  Big;
};

struct Leaf {
    float X, Y;
    float Land;          // the row it comes to rest on
    float Phase;
    float Lying;         // seconds it has lain, once down
    int   Tone;          // which colour of its tree
    bool  Petal;
};

struct Bolt {
    std::vector<Spot> Points;       // down from the cloud, in pixels
    float              Age = 0;
};

// A speck in the air for a moment: powder thrown up from the snow, a note.
struct Speck {
    float X, Y;
    float Vx, Vy;
    float Age;
    float Lasts;
    bool  Note;
};

class Falling {
public:
    void Step(const Nature &Weather, const Scenery &Place, Dice &Chance, float Seconds);

    // The animals' share of the air.
    void Powder(float X, float Y, int Count, Dice &Chance);
    void Note(float X, float Y);

    std::vector<Drop>   Drops;
    std::vector<Splash> Splashes;
    std::vector<Flake>  Flakes;
    std::vector<Leaf>   Leaves;
    std::vector<Speck>  Specks;
    Bolt                Lightning;
    float               Flash   = 0;     // 0..1 how bright the sky is from the last strike
    float               Strikes = 0;     // so far: the animals tell a new one by it
    float               Passed  = 0;     // the clouds' way, in seconds of wind

private:
    void Rain(const Nature &Weather, const Layout &Size, Dice &Chance, float Seconds);
    void Snow(const Nature &Weather, const Layout &Size, Dice &Chance, float Seconds);
    void Foliage(const Nature &Weather, const Scenery &Place, Dice &Chance, float Seconds);
    void Strike(const Nature &Weather, const Layout &Size, Dice &Chance, float Seconds);

    float EchoLeft = 0;
    float LeafOwed  = 0;
    float PetalOwed = 0;
};

}  // namespace forest

#endif
