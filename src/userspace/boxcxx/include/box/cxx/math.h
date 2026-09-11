#ifndef BOXCXX_BOX_CXX_MATH_H
#define BOXCXX_BOX_CXX_MATH_H
#include <charconv>
#include <cmath>

namespace box {

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

inline double log(double x) { return std::log(x); }
inline double log(double x, double base) {
    if (base == 2.0)  return std::log2(x);
    if (base == 10.0) return std::log10(x);
    return std::log(x) / std::log(base);
}

inline double root(double x, double n) {
    if (n == 2.0) return std::sqrt(x);
    if (n == 3.0) return std::cbrt(x);
    if (x == 0.0) return (n > 0.0) ? 0.0 : std::kInf;
    if (x > 0.0) return std::pow(x, 1.0 / n);
    double rn = std::rint(n);
    if (rn == n) { long ni = (long)rn; if (ni % 2 != 0) return -std::pow(-x, 1.0 / n); }
    return std::kQNaN;
}

namespace __math {
inline constexpr int kMaxFracDigits = 341;
inline constexpr double kTwo52 = 4503599627370496.0;
}
inline double round(double x, int digits = 0) {
    if (!std::isfinite(x)) return x;
    if (digits >= 0) {
        if (digits >= __math::kMaxFracDigits || std::fabs(x) >= __math::kTwo52)
            return x;
        const double a = std::ldexp(x, digits + 1), b = std::ldexp(x, digits);
        if (std::floor(a) == a && std::floor(b) != b)
            x = std::nextafter(x, std::copysign(std::kInf, x));
        char buf[1 + 16 + 1 + __math::kMaxFracDigits + 1];
        auto r = std::to_chars(buf, buf + sizeof buf, x, std::chars_format::fixed, digits);
        if (r.ec != std::errc{}) return x;
        double out = x;
        std::from_chars(buf, r.ptr, out);
        return out;
    }
    double p = std::pow(10.0, (double)digits);
    if (p == 0.0) return std::copysign(0.0, x);
    double r = std::round(x * p) / p;
    return std::isfinite(r) ? r : x;
}

namespace __math {
inline double SinSmallDeg(double f) { return std::sin(f * (std::kPi / 180.0)); }
inline double CosSmallDeg(double f) { return std::cos(f * (std::kPi / 180.0)); }

inline void SplitDeg(double d, int &q, double &f) {
    const double r = std::fmod(d, 360.0);
    f = std::fmod(r, 90.0);
    q = (int)((r - f) / 90.0);
}

inline double SinDeg(double d) {
    if (!std::isfinite(d)) return std::kQNaN;
    const double sgn = std::signbit(d) ? -1.0 : 1.0;
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
    SplitDeg(std::fabs(d), q, f);
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

inline double TanDeg(double d) {
    if (!std::isfinite(d)) return std::kQNaN;
    const double sgn = std::signbit(d) ? -1.0 : 1.0;
    int q; double f;
    SplitDeg(std::fabs(d), q, f);
    if (f == 0.0)
        return (q % 2 == 0) ? sgn * 0.0
                            : sgn * ((q == 1) ? std::kInf : -std::kInf);
    if (f == 45.0)
        return sgn * ((q % 2 == 0) ? 1.0 : -1.0);
    const double s = SinSmallDeg(f), c = CosSmallDeg(f);
    return sgn * ((q % 2 == 0) ? (s / c) : -(c / s));
}
}

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

}
#endif