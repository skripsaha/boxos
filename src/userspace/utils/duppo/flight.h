/* flight.h — where Duppo floats and how its body gives: a drift between
 * places on the glass, on a spring, so it sets off and comes to rest softly;
 * a bob that never stops; and a jelly that stretches along the way it goes
 * and wobbles when the way changes. Nothing of the glass but its size. */
#ifndef DUPPO_FLIGHT_H
#define DUPPO_FLIGHT_H

#include <array>

#include "painter/core/geometry.h"

#include "vec.h"

namespace duppo {

/* The map from an offset on the glass to the same point of the body at rest,
 * the inverse of the body's squash: a 2x2 matrix. */
struct Skew {
    float Xx = 1.0f, Xy = 0.0f, Yx = 0.0f, Yy = 1.0f;

    Vec Apply(Vec At) const { return {Xx * At.X + Xy * At.Y, Yx * At.X + Yy * At.Y}; }
};

class Flight {
public:
    static constexpr float Fastest = 15.0f;   /* units a second, at the usual pace */

    /* The glass in cells, and Duppo's size as the glass units one unit of
     * the logo takes. It starts in a place of its own choosing. */
    void Begin(painter::core::Size Cells, float Size, Random &Dice);
    /* A new size while it floats. */
    void Fit(float Size);
    /* How fast it lives, as a share of the usual. */
    void Tune(float Speed) { Pace = Speed; }
    void Step(float Seconds, Random &Dice);

    /* The largest size at which the whole logo fits on a glass. */
    static float Largest(painter::core::Size Cells);

    bool Travelling() const;

    Vec   Center;        /* where it is drawn: the place it holds, bobbing */
    Vec   Velocity;
    Vec   Goal;          /* the place it is bound for */
    float Scale = 1.0f;
    Skew  Warp;

private:
    void Choose(Random &Dice);
    void Settle(Random &Dice);
    void Give(float Seconds);
    void Shape();

    Vec   Glass;         /* the glass in units */
    Vec   Low, High;     /* the corners of the places it may hold */
    Vec   Place;
    float Pace      = 1.0f;
    float Clock     = 0.0f;
    float Leg       = 0.0f;
    float Dwell     = 0.0f;
    bool  Resting   = false;

    std::array<float, 3> Squash{};   /* xx, xy, yy of what it is stretched by */
    std::array<float, 3> Quiver{};   /* and how fast that moves */
};

}  // namespace duppo

#endif
