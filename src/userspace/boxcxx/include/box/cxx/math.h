// boxcxx — box::cxx math: BoxOS-native universal layer over std <cmath>.
// box::log(x[,base]), box::root(x,n), box::round(x,digits), angle-unit trig.
#ifndef BOXCXX_BOX_CXX_MATH_H
#define BOXCXX_BOX_CXX_MATH_H
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
inline double log(double x) { return std::log(x); }
inline double log(double x, double base) { return std::log(x) / std::log(base); }

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
inline double round(double x, int digits = 0) {
    if (!std::isfinite(x)) return x;
    double p = std::pow(10.0, (double)digits);
    return std::round(x * p) / p;
}

// ── angle-unit trigonometry: box::sin(90, box::deg), etc. ──────────────────
inline double sin(double x, angle u = angle::rad)  { return std::sin(to_radians(x, u)); }
inline double cos(double x, angle u = angle::rad)  { return std::cos(to_radians(x, u)); }
inline double tan(double x, angle u = angle::rad)  { return std::tan(to_radians(x, u)); }
inline double asin(double x, angle u = angle::rad) { return from_radians(std::asin(x), u); }
inline double acos(double x, angle u = angle::rad) { return from_radians(std::acos(x), u); }
inline double atan(double x, angle u = angle::rad) { return from_radians(std::atan(x), u); }
inline double atan2(double y, double x, angle u = angle::rad) { return from_radians(std::atan2(y, x), u); }

} // namespace box
#endif
