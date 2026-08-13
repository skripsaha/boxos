// boxcxx — box::cxx math: BoxOS-native universal layer over std <cmath>.
// box::log(x[,base]), box::root(x,n), box::round(x,digits), angle-unit trig.
#ifndef BOXCXX_BOX_CXX_MATH_H
#define BOXCXX_BOX_CXX_MATH_H
#include <charconv>   // box::round rounds through the exact decimal conversion
#include <cmath>

namespace box {

// ── angle units ────────────────────────────────────────────────────────────
enum class angle { rad, deg, turn };
inline constexpr angle rad  = angle::rad;
inline constexpr angle deg  = angle::deg;
inline constexpr angle turn = angle::turn;

inline double to_radians(double x, angle u) {
    switch (u) {
        case angle::deg:  return x * (std::kPi / 180.0);
        case angle::turn: return x * (2.0 * std::kPi);
        default:          return x;
    }
}
inline double from_radians(double x, angle u) {
    switch (u) {
        case angle::deg:  return x * (180.0 / std::kPi);
        case angle::turn: return x * (1.0 / (2.0 * std::kPi));
        default:          return x;
    }
}

// ── generalized logarithm: box::log(x) = ln; box::log(x, base) = log_base ───
// Bases 2 and 10 go through their own <cmath> entry rather than a ratio of two
// logarithms. Measured against THIS cmath, not a host one: of the 87 powers of
// two and ten whose logarithm is an exact integer, the ratio returns that
// integer for 70, the dedicated entries for 86. This mirrors what root() below
// already does for its exact cases.
inline double log(double x) { return std::log(x); }
inline double log(double x, double base) {
    if (base == 2.0)  return std::log2(x);
    if (base == 10.0) return std::log10(x);
    return std::log(x) / std::log(base);
}

// ── generalized root: box::root(x, n) = n-th root (sqrt/cbrt fast paths) ────
inline double root(double x, double n) {
    if (n == 2.0) return std::sqrt(x);
    if (n == 3.0) return std::cbrt(x);
    if (x == 0.0) return (n > 0.0) ? 0.0 : std::kInf;
    if (x > 0.0) return std::pow(x, 1.0 / n);
    // x < 0: real root only for odd-integer n
    double rn = std::rint(n);
    if (rn == n) { long ni = (long)rn; if (ni % 2 != 0) return -std::pow(-x, 1.0 / n); }
    return std::kQNaN;
}

// ── decimal rounding: box::round(x, digits) ────────────────────────────────
// Rounds the value the double ACTUALLY holds, by going through the exact
// decimal conversion (Ф27b's to_chars/from_chars are correctly rounded) instead
// of scaling by a power of ten and back. Scaling was both unpredictable —
// round(2.675, 2) gave 2.68 while round(1.005, 2) gave 1.00, because the stored
// values sit either side of the midpoint — and unsafe at the ends of the range,
// where 10^digits overflowed and turned a finite input into inf or NaN.
namespace __math {
// Past this every finite double is already exact: the smallest subnormal has
// its first significant digit at place 324, so 341 places keep 17 significant
// digits and the value round-trips unchanged.
inline constexpr int kMaxFracDigits = 341;
inline constexpr double kTwo52 = 4503599627370496.0;   // 2^52: no fraction above
}
inline double round(double x, int digits = 0) {
    if (!std::isfinite(x)) return x;
    if (digits >= 0) {
        if (digits >= __math::kMaxFracDigits || std::fabs(x) >= __math::kTwo52)
            return x;   // nothing behind the point left to round
        // The decimal conversion breaks ties to even; this function has always
        // broken them away from zero, because std::round does. x is an exact tie
        // at `digits` places iff it has exactly digits+1 fractional BITS — then
        // x * 10^digits is an odd multiple of one half. One ulp outward puts it
        // past the midpoint, so a correctly rounded conversion lands away.
        const double a = std::ldexp(x, digits + 1), b = std::ldexp(x, digits);
        if (std::floor(a) == a && std::floor(b) != b)
            x = std::nextafter(x, std::copysign(std::kInf, x));
        char buf[1 + 16 + 1 + __math::kMaxFracDigits + 1];   // sign, int, point, frac
        auto r = std::to_chars(buf, buf + sizeof buf, x, std::chars_format::fixed, digits);
        if (r.ec != std::errc{}) return x;
        double out = x;
        std::from_chars(buf, r.ptr, out);
        return out;
    }
    // digits < 0: round to a multiple of 10^-digits. A step wider than any
    // double leaves nothing but the zero, and it keeps x's sign.
    double p = std::pow(10.0, (double)digits);
    if (p == 0.0) return std::copysign(0.0, x);
    double r = std::round(x * p) / p;
    return std::isfinite(r) ? r : x;
}

// ── angle-unit trigonometry: box::sin(90, box::deg), etc. ──────────────────
// Degrees and turns are reduced onto the quadrant BEFORE any conversion to
// radians, which buys two things the plain conversion cannot. The compass
// points come out exact, where the plain conversion gave sin(180, deg) as a
// value near 1.2e-16 — the double nearest pi is not pi. And the error stops
// growing with the angle: multiplying the whole angle by a rounded kPi/180
// loses accuracy in proportion to the angle before <cmath> is ever called, so a
// million turns past a compass point the old path no longer landed on 0 or +-1
// at all. fmod is exact for doubles, so the reduction itself costs nothing.
// Measured on BoxOS across 3600 ordinary angles against an 80-bit reference,
// the reduced path totals 1.19e-13 of error against 3.14e-13 for the plain
// conversion; a host libm ranked them the other way round, which is why that
// comparison lives in phase143 and not in a scratch program. The quadrant is as
// far as the folding goes: folding again at 45 degrees costs an extra rounding
// just above 45 and buys nothing, since a quadrant already caps the argument at
// 1.57 rad.
namespace __math {
inline double SinSmallDeg(double f) { return std::sin(f * (std::kPi / 180.0)); }
inline double CosSmallDeg(double f) { return std::cos(f * (std::kPi / 180.0)); }

// Split |d| degrees into quadrant q and remainder f, both exactly.
inline void SplitDeg(double d, int &q, double &f) {
    const double r = std::fmod(d, 360.0);
    f = std::fmod(r, 90.0);
    q = (int)((r - f) / 90.0);          // r - f is an exact multiple of 90
}

inline double SinDeg(double d) {
    if (!std::isfinite(d)) return std::kQNaN;
    const double sgn = std::signbit(d) ? -1.0 : 1.0;   // sin is odd
    int q; double f;
    SplitDeg(std::fabs(d), q, f);
    if (f == 0.0) {
        constexpr double kT[4] = {0.0, 1.0, 0.0, -1.0};
        return sgn * kT[q];
    }
    switch (q) {
        case 0:  return sgn * SinSmallDeg(f);
        case 1:  return sgn * CosSmallDeg(f);
        case 2:  return sgn * -SinSmallDeg(f);
        default: return sgn * -CosSmallDeg(f);
    }
}

inline double CosDeg(double d) {
    if (!std::isfinite(d)) return std::kQNaN;
    int q; double f;
    SplitDeg(std::fabs(d), q, f);                      // cos is even
    if (f == 0.0) {
        constexpr double kT[4] = {1.0, 0.0, -1.0, 0.0};
        return kT[q];
    }
    switch (q) {
        case 0:  return CosSmallDeg(f);
        case 1:  return -SinSmallDeg(f);
        case 2:  return -CosSmallDeg(f);
        default: return SinSmallDeg(f);
    }
}

// tan is odd and exact on every multiple of 45 degrees. The poles follow from
// sin/cos above (cos is +0 there), so tan(x,u) still equals sin(x,u)/cos(x,u).
inline double TanDeg(double d) {
    if (!std::isfinite(d)) return std::kQNaN;
    const double sgn = std::signbit(d) ? -1.0 : 1.0;
    int q; double f;
    SplitDeg(std::fabs(d), q, f);
    if (f == 0.0)
        return (q % 2 == 0) ? sgn * 0.0
                            : sgn * ((q == 1) ? std::kInf : -std::kInf);
    if (f == 45.0)                      // sin and cos are not bit-equal here
        return sgn * ((q % 2 == 0) ? 1.0 : -1.0);
    // Period 180, so an odd quadrant is the co-tangent branch: tan(90+f) is
    // -cot(f), not -tan(f).
    const double s = SinSmallDeg(f), c = CosSmallDeg(f);
    return sgn * ((q % 2 == 0) ? (s / c) : -(c / s));
}
}  // namespace __math

inline double sin(double x, angle u = angle::rad) {
    switch (u) {
        case angle::deg:  return __math::SinDeg(x);
        case angle::turn: return __math::SinDeg(x * 360.0);
        default:          return std::sin(x);
    }
}
inline double cos(double x, angle u = angle::rad) {
    switch (u) {
        case angle::deg:  return __math::CosDeg(x);
        case angle::turn: return __math::CosDeg(x * 360.0);
        default:          return std::cos(x);
    }
}
inline double tan(double x, angle u = angle::rad) {
    switch (u) {
        case angle::deg:  return __math::TanDeg(x);
        case angle::turn: return __math::TanDeg(x * 360.0);
        default:          return std::tan(x);
    }
}
inline double asin(double x, angle u = angle::rad) { return from_radians(std::asin(x), u); }
inline double acos(double x, angle u = angle::rad) { return from_radians(std::acos(x), u); }
inline double atan(double x, angle u = angle::rad) { return from_radians(std::atan(x), u); }
inline double atan2(double y, double x, angle u = angle::rad) { return from_radians(std::atan2(y, x), u); }

} // namespace box
#endif
