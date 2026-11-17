/*
 * transform.cpp — where a place of a shape's own space lands. See
 * painter/core/transform.h.
 */

#include "painter/core/transform.h"

#include <cmath>

namespace painter::core {

namespace {

constexpr float Pi = 3.14159265358979f;

/* The cosine and sine of Degrees: a whole number of quarter turns exactly. */
void TurnOf(float Degrees, float &Cos, float &Sin) noexcept
{
    const float Quarters = Degrees / 90.0f;
    if (Quarters == std::floor(Quarters) && std::fabs(Quarters) < 1.0e6f) {
        const int Q = static_cast<int>(std::fmod(Quarters, 4.0f) + 4.0f) % 4;
        Cos = Q == 0 ? 1.0f : Q == 2 ? -1.0f : 0.0f;
        Sin = Q == 1 ? 1.0f : Q == 3 ? -1.0f : 0.0f;
        return;
    }
    Cos = std::cos(Degrees * Pi / 180.0f);
    Sin = std::sin(Degrees * Pi / 180.0f);
}

}  // namespace

Transform Transform::Rotate(float Degrees) noexcept
{
    float Cos, Sin;
    TurnOf(Degrees, Cos, Sin);
    return {Cos, Sin, -Sin, Cos, 0, 0};
}

Transform Transform::Skew(float XDegrees, float YDegrees) noexcept
{
    return {1, std::tan(YDegrees * Pi / 180.0f), std::tan(XDegrees * Pi / 180.0f), 1, 0, 0};
}

Transform Transform::Then(const Transform &N) const noexcept
{
    return {N.A * A + N.C * B, N.B * A + N.D * B, N.A * C + N.C * D,
            N.B * C + N.D * D, N.A * E + N.C * F + N.E, N.B * E + N.D * F + N.F};
}

PointF Transform::Apply(PointF P) const noexcept
{
    return {A * P.X + C * P.Y + E, B * P.X + D * P.Y + F};
}

std::optional<Transform> Transform::Inverse() const noexcept
{
    const float Folds = Determinant();
    if (Folds == 0.0f || !std::isfinite(Folds)) return std::nullopt;
    const float I = 1.0f / Folds;
    return Transform{D * I, -B * I, -C * I, A * I, (C * F - D * E) * I, (B * E - A * F) * I};
}

}  // namespace painter::core
