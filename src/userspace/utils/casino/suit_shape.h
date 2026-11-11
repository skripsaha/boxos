/* suit_shape.h — the four suits as shapes, not pixels: whether a point of a
 * square from -1 to 1 (V up) is inside the mark. The card is drawn from these
 * at whatever size the glass has room for. */
#ifndef CASINO_SUIT_SHAPE_H
#define CASINO_SUIT_SHAPE_H

#include <cmath>

#include "cards.h"

namespace casino {

namespace suit_shape {

inline bool Heart(float U, float V)
{
    const float X = U * 1.18f, Y = V * 1.18f + 0.12f;
    const float Base = X * X + Y * Y - 1.0f;
    return Base * Base * Base - X * X * Y * Y * Y <= 0.0f;
}

inline bool Diamond(float U, float V)
{
    return std::fabs(U) / 0.74f + std::fabs(V) <= 1.0f;
}

inline bool Stem(float U, float V, float Top, float Bottom)
{
    if (V > Top || V < Bottom) return false;
    return std::fabs(U) <= 0.14f + 0.3f * (Top - V) / (Top - Bottom);
}

inline bool Spade(float U, float V)
{
    return Heart(U, -(V - 0.28f) * 1.05f) || Stem(U, V, -0.1f, -1.0f);
}

inline bool Club(float U, float V)
{
    const auto Disc = [&](float Across, float Down, float Radius) {
        return (U - Across) * (U - Across) + (V - Down) * (V - Down) <= Radius * Radius;
    };
    return Disc(0.0f, 0.46f, 0.37f) || Disc(-0.45f, -0.14f, 0.37f) || Disc(0.45f, -0.14f, 0.37f) || Stem(U, V, -0.05f, -1.0f) ||
           Disc(0.0f, 0.1f, 0.3f);
}

}  // namespace suit_shape

// Whether (U, V) is inside the mark of Of.
inline bool InSuit(Suit Of, float U, float V)
{
    switch (Of) {
    case Suit::Spades:   return suit_shape::Spade(U, V);
    case Suit::Hearts:   return suit_shape::Heart(U, V);
    case Suit::Diamonds: return suit_shape::Diamond(U, V);
    case Suit::Clubs:    return suit_shape::Club(U, V);
    }
    return false;
}

}  // namespace casino

#endif
