#ifndef PAINTER_CORE_EASING_H
#define PAINTER_CORE_EASING_H

// How a change goes over the time it takes: its progress, 0 at its start and
// 1 at its end, at each share of that time — straight, slowly at first,
// slowing at the end, both, in jumps, as a spring, or along a curve of one's
// own (box/items.h ITEM_CURVE_*). The display plays animations by it and a
// program may ask it where one of its own stands.
//
// A spring moves as a weight on a spring does: from where it is, at the speed
// it has, toward its rest, settling in about the time it is given — bouncing
// past its rest by as much of the way as it is told, or not at all. A change
// cut on its way hands its speed on to the spring that takes it from there.

#include <cstdint>

namespace painter::core {

struct Spring {
    float Damping   = 1;   // 1 settles without passing its rest; less bounces
    float Frequency = 0;   // how fast it swings, radians a second

    // A spring that settles within a thousandth of its way in about Settle
    // seconds, passing its rest by Bounce of the way (0 to 0.9; 0 not at all).
    static Spring For(float Settle, float Bounce) noexcept;

    // Where it is and how fast it moves, T seconds after it was Offset from
    // its rest, moving at Speed a second.
    struct State {
        float Offset;
        float Speed;
    };
    State At(float Offset, float Speed, float T) const noexcept;
};

struct Easing {
    enum class Kind : std::uint8_t { Linear, In, Out, InOut, Steps, Spring, Own };
    Kind  What   = Kind::Linear;
    int   Steps  = 1;                   // Steps: how many jumps
    float Bounce = 0;                   // Spring: how far past its end, a share of the way
    float X1 = 0, Y1 = 0, X2 = 1, Y2 = 1;   // Own: a cubic Bézier's inner points; X within 0 to 1

    // The progress at Share of the time, 0 to 1 — past them only where a
    // spring bounces or an own curve goes beyond. 1 from the end on.
    float At(float Share) const noexcept;
};

}  // namespace painter::core

#endif  // PAINTER_CORE_EASING_H
