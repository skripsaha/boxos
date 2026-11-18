/*
 * easing.cpp — how a change goes over the time it takes. See
 * painter/core/easing.h.
 */

#include "painter/core/easing.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace painter::core {

namespace {

/* Within this share of its way a spring has settled. */
constexpr float Settled = 1e-3f;

/* (1 + x) e^-x = Settled: where a spring that never passes its rest has
 * settled, in its frequency's radians. */
constexpr float SettledCritical = 9.2335f;

/* A cubic Bézier from (0, 0) to (1, 1) through (X1, Y1) and (X2, Y2), along
 * one axis at T. */
float Along(float P1, float P2, float T) noexcept
{
    const float U = 1 - T;
    return 3 * U * U * T * P1 + 3 * U * T * T * P2 + T * T * T;
}

float Slope(float P1, float P2, float T) noexcept
{
    const float U = 1 - T;
    return 3 * U * U * P1 + 6 * U * T * (P2 - P1) + 3 * T * T * (1 - P2);
}

/* The curve's height where it is Share across: its T found by Newton's steps,
 * by halving where they stall. */
float Bezier(float X1, float Y1, float X2, float Y2, float Share) noexcept
{
    X1 = std::clamp(X1, 0.0f, 1.0f);
    X2 = std::clamp(X2, 0.0f, 1.0f);
    float T = Share;
    for (int I = 0; I < 8; I++) {
        const float Off = Along(X1, X2, T) - Share, Steep = Slope(X1, X2, T);
        if (std::fabs(Off) < 1e-6f) return Along(Y1, Y2, T);
        if (std::fabs(Steep) < 1e-6f) break;
        T = std::clamp(T - Off / Steep, 0.0f, 1.0f);
    }
    float Low = 0, High = 1;
    T = Share;
    for (int I = 0; I < 32; I++) {
        const float Off = Along(X1, X2, T) - Share;
        if (std::fabs(Off) < 1e-6f) break;
        (Off < 0 ? Low : High) = T;
        T = (Low + High) / 2;
    }
    return Along(Y1, Y2, T);
}

}  // namespace

Spring Spring::For(float Settle, float Bounce) noexcept
{
    Settle = std::max(Settle, 1e-3f);
    Bounce = std::clamp(Bounce, 0.0f, 0.9f);
    if (Bounce <= 0) return Spring{1, SettledCritical / Settle};
    /* Passing its rest by Bounce of the way is a damping of this ratio; its
     * swings shrink within Settled of the way by Settle. */
    const float Log     = std::log(Bounce);
    const float Damping = -Log / std::sqrt(std::numbers::pi_v<float> * std::numbers::pi_v<float> + Log * Log);
    return Spring{Damping, -std::log(Settled * std::sqrt(1 - Damping * Damping)) / (Damping * Settle)};
}

Spring::State Spring::At(float Offset, float Speed, float T) const noexcept
{
    if (Frequency <= 0 || !(T > 0)) return State{Offset, Speed};
    const float W = Frequency;
    if (Damping >= 1) {
        const float Fade = std::exp(-W * T), Push = Speed + W * Offset;
        return State{Fade * (Offset + Push * T), Fade * (Push - W * (Offset + Push * T))};
    }
    const float Swing = W * std::sqrt(1 - Damping * Damping);
    const float Fade  = std::exp(-Damping * W * T);
    const float Lift  = (Speed + Damping * W * Offset) / Swing;
    const float Cos = std::cos(Swing * T), Sin = std::sin(Swing * T);
    const float Where = Offset * Cos + Lift * Sin;
    return State{Fade * Where, Fade * (-Damping * W * Where - Offset * Swing * Sin + Lift * Swing * Cos)};
}

float Easing::At(float Share) const noexcept
{
    if (!(Share > 0)) return 0;
    if (Share >= 1) return 1;
    switch (What) {
    case Kind::Linear:
        return Share;
    case Kind::In:
        return Bezier(0.4f, 0, 1, 1, Share);
    case Kind::Out:
        return Bezier(0, 0, 0.2f, 1, Share);
    case Kind::InOut:
        return Bezier(0.4f, 0, 0.2f, 1, Share);
    case Kind::Steps: {
        const int Count = std::max(1, Steps);
        return std::floor(Share * static_cast<float>(Count)) / static_cast<float>(Count);
    }
    case Kind::Spring:
        return 1 + Spring::For(1, Bounce).At(-1, 0, Share).Offset;
    case Kind::Own:
        return Bezier(X1, Y1, X2, Y2, Share);
    }
    return Share;
}

}  // namespace painter::core
