/* emblem.cpp — the two faces' reliefs, each a handful of shapes. */

#include "emblem.h"

#include <cmath>

namespace coinflip {

namespace {

bool InCircle(double A, double B, double CentreA, double CentreB, double Radius)
{
    const double DA = A - CentreA;
    const double DB = B - CentreB;
    return DA * DA + DB * DB <= Radius * Radius;
}

bool InEllipse(double A, double B, double CentreA, double CentreB, double Wide, double High)
{
    const double DA = (A - CentreA) / Wide;
    const double DB = (B - CentreB) / High;
    return DA * DA + DB * DB <= 1.0;
}

bool InBox(double A, double B, double From, double To, double Below, double Above)
{
    return A >= From && A <= To && B >= Below && B <= Above;
}

// A point of the crown: a triangle that narrows from its foot at Base to its tip.
bool InSpike(double A, double B, double CentreA, double Base, double Tip, double HalfFoot)
{
    if (B < Base || B > Tip) return false;
    return std::fabs(A - CentreA) <= HalfFoot * (Tip - B) / (Tip - Base);
}

}  // namespace

bool HeadsRaised(double A, double B)
{
    // The eye is cut out of the head.
    if (InCircle(A, B, 0.19, 0.08, 0.04)) return false;
    if (InCircle(A, B, -0.02, 0.10, 0.30)) return true;        // the skull
    if (InEllipse(A, B, 0.10, -0.10, 0.20, 0.24)) return true; // the face and the jaw
    if (InCircle(A, B, 0.31, -0.02, 0.07)) return true;        // the nose
    if (InBox(A, B, -0.22, 0.08, -0.45, -0.20)) return true;   // the neck
    if (InEllipse(A, B, -0.05, -0.72, 0.52, 0.28)) return true; // the shoulders
    if (InBox(A, B, -0.30, 0.28, 0.36, 0.45)) return true;     // the crown's band
    return InSpike(A, B, -0.24, 0.45, 0.65, 0.09) || InSpike(A, B, 0.0, 0.45, 0.68, 0.09) ||
           InSpike(A, B, 0.24, 0.45, 0.65, 0.09);
}

bool TailsRaised(double A, double B)
{
    // The door and the window are cut out of the tower.
    if (InBox(A, B, -0.08, 0.08, -0.46, -0.16) || InCircle(A, B, 0.0, -0.16, 0.08)) return false;
    if (InBox(A, B, -0.04, 0.04, 0.02, 0.14)) return false;
    if (InBox(A, B, -0.27, 0.27, -0.46, 0.30)) return true;    // the wall
    if (InBox(A, B, -0.35, 0.35, 0.30, 0.38)) return true;     // the walk along its top
    if (InBox(A, B, -0.33, -0.19, 0.38, 0.50) || InBox(A, B, -0.07, 0.07, 0.38, 0.50) ||
        InBox(A, B, 0.19, 0.33, 0.38, 0.50))
        return true;                                           // the battlements
    if (InBox(A, B, -0.52, 0.52, -0.56, -0.46)) return true;   // the ground
    return InCircle(A, B, -0.55, 0.40, 0.045) || InCircle(A, B, 0.55, 0.40, 0.045);   // two stars
}

}  // namespace coinflip
