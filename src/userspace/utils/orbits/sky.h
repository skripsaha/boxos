// sky.h — the bodies and what they do to each other: pulled by gravity,
// merged when they touch, lost when they are far gone. Nothing of the glass
// or of BoxOS is in it; the picture is paint.cpp's, the keys are show's.
//
// The unit of the sky is the one the presets are made in: a star of mass 1000
// pulls with G = 0.001, so a body in a circular orbit 1.0 from it goes round at
// speed 1.0 and takes 2 pi seconds. Time moves by a fixed step, kick-drift-kick
// leapfrog, which is symplectic: the energy of a calm system wanders about its
// value and does not run away, however long it is left.
#ifndef ORBITS_SKY_H
#define ORBITS_SKY_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace orbits {

struct Vec {
    double X = 0.0;
    double Y = 0.0;
};

inline Vec operator+(Vec A, Vec B) { return {A.X + B.X, A.Y + B.Y}; }
inline Vec operator-(Vec A, Vec B) { return {A.X - B.X, A.Y - B.Y}; }
inline Vec operator*(Vec A, double By) { return {A.X * By, A.Y * By}; }

inline constexpr double StepSeconds   = 1.0 / 720.0;
inline constexpr double BaseGravity   = 0.001;
inline constexpr double Softening     = 0.01;    // keeps a near pass finite
inline constexpr double StarMass      = 300.0;   // from here a body shines
inline constexpr double EscapeRadius  = 14.0;    // farther than this a body is gone
inline constexpr std::size_t MostBodies = 160;

struct Body {
    std::uint32_t Id     = 0;
    Vec           At;
    Vec           Speed;
    double        Mass   = 1.0;
    double        Radius = 0.01;
    double        Tone   = 0.0;   // 0..1: its place in the palette
    Vec           Drawn;          // where its trail was last drawn to

    bool IsStar() const { return Mass >= StarMass; }
};

// What a touch did, for the flare: where the lighter body was and its mass,
// and the tone of the heavier, whose place the merged body takes.
struct Collision {
    Vec    At;
    double Mass = 0.0;
    double Tone = 0.0;
    bool   Star = false;
};

// Where a body thrown now would go: its places a fixed time apart, and
// whether it ends in a touch before the forecast does.
struct Forecast {
    std::vector<Vec> Places;
    bool             Ends = false;
};

double RadiusOf(double Mass);

class Sky {
public:
    // The body is added; false when the sky is full.
    bool Add(Vec At, Vec Speed, double Mass, double Tone);
    void Clear();
    void SetGravity(double Strength);
    double Strength() const { return Gravity; }
    // One step of Seconds.
    void Advance(double Seconds);
    // The centre of mass brought to rest at the middle.
    void Settle();
    // The speed of a circular orbit Distance from Mass.
    double CircularSpeed(double Distance, double Mass) const;
    // The total mass of the stars: what a far body orbits.
    double StarsMass() const;
    // The `Places` places a body of Mass thrown from At at Speed will be at,
    // `Steps` steps of Seconds apart.
    Forecast Foretell(Vec At, Vec Speed, double Mass, std::size_t Places, int Steps, double Seconds) const;

    std::vector<Body>      Bodies;
    std::vector<Collision> Collisions;   // taken by whoever draws them
    std::size_t            Merged  = 0;
    std::size_t            Escaped = 0;

private:
    void Pull();
    void Kick(double Seconds);
    void Join(std::size_t Kept, std::size_t Taken);
    void Merge();
    void Leave();

    double           Gravity   = BaseGravity;
    std::vector<Vec> Pulled;
    bool             PullKnown = false;
    bool             Touching  = false;   // two bodies touch at the places Pulled was worked out for
    std::uint32_t    NextId    = 1;
};

}  // namespace orbits

#endif  // ORBITS_SKY_H
