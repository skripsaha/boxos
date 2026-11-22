/* world.h — the game itself, with no screen in it: the bird, the pipes, the
 * score and what stage the round is at. Everything is measured in units of a
 * field 512 tall, the height the game was first drawn at, with the ground's
 * top at 400; the picture (scene.h) lays a unit as many points as the screen
 * is tall over 512, and as wide as the screen is the field goes on. */
#ifndef FLAPPYBIRD_WORLD_H
#define FLAPPYBIRD_WORLD_H

#include <cstdint>
#include <vector>

namespace flappybird {

namespace field {
inline constexpr float Tall       = 512;
inline constexpr float Ground     = 400;   // the ground's top
inline constexpr float PipeWide   = 52;
inline constexpr float CapWide    = 58;
inline constexpr float CapTall    = 24;
inline constexpr float Gap        = 104;   // between a pair's two pipes
inline constexpr float Spacing    = 184;   // from one pair to the next
inline constexpr float Speed      = 128;   // how fast the pipes and the ground come, a second
inline constexpr float BirdWide   = 34;
inline constexpr float BirdTall   = 24;
inline constexpr float BirdReach  = 11.5f; // the round of the bird that a pipe must not touch
}  // namespace field

enum class Stage : std::uint8_t {
    Ready,     // the bird hovers, waiting for the first flap
    Flying,
    Falling,   // struck: everything stops but the bird, which drops to the ground
    Over,
};

struct Pipe {
    float X      = 0;      // its left side
    float Middle = 0;      // the middle of its gap
    bool  Passed = false;  // the bird is past it, and it is counted
};

struct Bird {
    float X     = 0;
    float Y     = 0;
    float Speed = 0;       // down, a second
    float Tilt  = 0;       // degrees, nose down
    float Wing  = 0;       // how far through its beat the wing is, 0 to 1
};

/* What a step brought about, for the picture and the sounds of it. */
struct Happened {
    bool Scored = false;
    bool Struck = false;   // the bird hit a pipe or the ground
    bool Landed = false;   // it came down on the ground: the round is over
};

class World {
public:
    explicit World(float Wide);

    /* A new round, ready for the first flap. */
    void Begin();
    /* The bird beats its wings once; the first one starts the round. */
    void Flap();
    /* Seconds gone by. */
    Happened Advance(float Seconds);

    Stage                    Now() const noexcept { return At; }
    const Bird              &Flier() const noexcept { return Hero; }
    const std::vector<Pipe> &Pipes() const noexcept { return Row; }
    unsigned                 Score() const noexcept { return Points; }
    float                    Wide() const noexcept { return Width; }
    /* How far the ground has come, in units: its stripes go by with it. */
    float                    Travelled() const noexcept { return Distance; }
    /* Seconds the round has lasted, the hovering too: what bobs and beats by it. */
    float                    Clock() const noexcept { return Time; }

private:
    Happened Tick(float Seconds);
    void     MoveBird(float Seconds);
    void     MovePipes(float Seconds, Happened &What);
    bool     Strikes() const;
    float    NewMiddle();

    float             Width;
    Stage             At = Stage::Ready;
    Bird              Hero;
    std::vector<Pipe> Row;
    unsigned          Points   = 0;
    float             Distance = 0;
    float             Time     = 0;
    float             Rest     = 0;   // of the last advance, short of a whole tick
    float             Raised   = 0;   // seconds the nose has stayed up since the last flap
    float             Previous = 0;   // the middle of the gap laid last, 0 before the first
    std::uint64_t     Seed;
};

}  // namespace flappybird

#endif
