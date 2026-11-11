/* trig.h — angles for the picture: sine, cosine and the angle of a point, to a
 * thousandth of a radian, from a few multiplications. The wheel asks for one
 * angle per pixel, so it does not ask the libm. */
#ifndef CASINO_TRIG_H
#define CASINO_TRIG_H

#include <cmath>

namespace casino {

inline constexpr float Pi       = 3.14159265f;
inline constexpr float TwoPi    = 6.28318531f;
inline constexpr float HalfPi   = 1.57079633f;

// Any angle brought into -Pi..Pi.
inline float Wrapped(float Angle)
{
    return Angle - TwoPi * std::floor((Angle + Pi) / TwoPi);
}

inline float Sine(float Angle)
{
    const float Turned = Wrapped(Angle);
    const float Rough  = 1.27323954f * Turned - 0.405284735f * Turned * std::fabs(Turned);
    return 0.225f * (Rough * std::fabs(Rough) - Rough) + Rough;
}

inline float Cosine(float Angle)
{
    return Sine(Angle + HalfPi);
}

// The angle of the point (Across, Down) seen from the origin, from straight up
// and turning clockwise on the glass: 0 is up, HalfPi is right.
inline float Bearing(float Across, float Down)
{
    const float AbsAcross = std::fabs(Across);
    const float AbsDown   = std::fabs(Down);
    const float Big       = AbsAcross > AbsDown ? AbsAcross : AbsDown;
    if (Big == 0.0f) return 0.0f;
    const float Ratio   = (AbsAcross > AbsDown ? AbsDown : AbsAcross) / Big;
    const float Squared = Ratio * Ratio;
    float       Arc     = Ratio * (0.9998660f + Squared * (-0.3302995f + Squared * (0.1801410f + Squared * (-0.0851330f + Squared * 0.0208351f))));
    if (AbsDown > AbsAcross) Arc = HalfPi - Arc;
    // Arc is the point's angle above or below the horizontal; the angle
    // counterclockwise from the right is +Arc above it, -Arc below.
    float Counter = Down < 0 ? Arc : -Arc;
    if (Across < 0) Counter = Pi - Counter;
    return Wrapped(HalfPi - Counter);
}

}  // namespace casino

#endif
