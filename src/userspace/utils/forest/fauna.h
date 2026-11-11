/* fauna.h — the animals: who lives in the forest, what each is doing and how
 * it comes to do the next. Each kind has a small life of its own — the fox
 * trots, sniffs, sits, sleeps and, in snow, pounces; the hedgehog shuffles and
 * curls up, and sleeps the winter; the hare sits, washes, hops and bolts; the
 * deer grazes and lifts its head; the birds fly, perch, sing and peck; the owl
 * keeps the night; the bats, butterflies and fireflies drift in their hours.
 * What an animal does depends on the hour, the season and the weather, and on
 * who is near: a fox sends the hare off and the hedgehog into a ball, and the
 * thunder sends all of them.
 *
 * fauna.cpp keeps the herd and what the kinds have in common; fauna_ground.cpp
 * and fauna_wing.cpp are the kinds that walk and the kinds that fly. */
#ifndef FOREST_FAUNA_H
#define FOREST_FAUNA_H

#include <cstdint>
#include <vector>

#include "air.h"
#include "kinds.h"
#include "look.h"
#include "nature.h"
#include "numbers.h"
#include "scenery.h"
#include "sky.h"

namespace forest {

enum class Act : std::uint8_t {
    Away,       // out of the picture: gone to its cover, or not yet come
    Walk,
    Trot,
    Run,
    Sniff,
    Sit,
    Wash,
    Alert,
    Graze,
    Sleep,
    Curl,
    Hop,
    Pounce,
    Lie,
    Fly,
    Perch,
    Sing,
    Peck,
    Hoot
};

struct Animal {
    Kind  What;
    Act   Doing  = Act::Away;
    float X = 0, Y = 0;       // the feet on the ground, or the middle in the air
    float Vx = 0, Vy = 0;
    float GoalX = 0, GoalY = 0;
    float Left   = 0;         // seconds left in this act
    float Time   = 0;         // seconds since it began, for its frames
    float Stride = 0;         // how far it has walked, in turns of its legs
    float Phase  = 0;         // its own place in every slow wave
    float Rise   = 0;         // above its feet: a hop
    float Fear   = 0;         // seconds of fright left
    float Seen   = 0;         // the strikes of lightning it has seen
    float Presence = 0;       // what only drifts fades in and out by it
    int   Variant = 0;
    int   Tree   = -1;        // where a bird sits
    int   Perch  = -1;
    bool  FacesLeft = false;
    bool  Leaving = false;    // on its way out of the picture
    bool  Retire = false;     // told to go for good: it leaves, and is gone
};

// What the animals live among, and the part of it they change.
struct Habitat {
    const Nature  &Weather;
    const SkyNow  &Sky;
    const Scenery &Place;
    Falling       &Air;
    Dice          &Chance;
};

class Fauna {
public:
    // The animals as the person wants them: the first time they stand about
    // the glass, after that more come in at its edge and fewer are told to go.
    void Fit(const Look &Shown, const Habitat &Where, bool Settled);
    void Step(const Habitat &Where, float Seconds);

    std::size_t Living() const;

    std::vector<Animal> Animals;
};

// What the kinds' files share (fauna.cpp).

// Whether this is the hour, the season and the weather for the kind.
bool Wants(const Animal &Beast, const Habitat &Where);

// The ground an animal walks, from the first lane to the last.
float LaneAt(const Layout &Size, float Depth);

// Toward the goal on the ground at `Speed`, its legs turning with the steps:
// true when it has come.
bool Walks(Animal &Beast, const Habitat &Where, float Speed, float Seconds);

// The same for the hare: in hops, each one a short arc.
bool Hops(Animal &Beast, const Habitat &Where, float Speed, float Seconds);

// Somewhere to go on the ground within `Reach` pixels, on the glass.
void Wanders(Animal &Beast, const Habitat &Where, float Reach);

void Begin(Animal &Beast, Act Doing, float Lasts);

// The nearest animal of a kind within `Reach` of a point, or nullptr.
const Animal *Near(const Fauna &Herd, Kind Which, float X, float Y, float Reach);

// Lightning it has not yet seen: true once for each strike.
bool Startled(Animal &Beast, const Habitat &Where);

// The fox, the hedgehog, the hare, the deer (fauna_ground.cpp).
void StepFox(Animal &Beast, const Fauna &Herd, const Habitat &Where, float Seconds);
void StepHedgehog(Animal &Beast, const Fauna &Herd, const Habitat &Where, float Seconds);
void StepHare(Animal &Beast, const Fauna &Herd, const Habitat &Where, float Seconds);
void StepDeer(Animal &Beast, const Fauna &Herd, const Habitat &Where, float Seconds);

// The bird, the owl, the bat, the butterfly, the firefly (fauna_wing.cpp).
void StepBird(Animal &Beast, const Habitat &Where, float Seconds);
void StepOwl(Animal &Beast, const Habitat &Where, float Seconds);
void StepBat(Animal &Beast, const Habitat &Where, float Seconds);
void StepButterfly(Animal &Beast, const Habitat &Where, float Seconds);
void StepFirefly(Animal &Beast, const Habitat &Where, float Seconds);

// Where on the glass a bird sits: the perch's place after the tree sways.
bool PerchPlace(const Habitat &Where, const Animal &Beast, float &X, float &Y);

}  // namespace forest

#endif
