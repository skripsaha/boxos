// boxcxx — <stdckdint.h>  ([stdckdint.h.syn], C++26 adopting C23 7.20)
//
// Checked integer arithmetic: compute in infinite signed precision, store
// the wrapped result, and say whether the true result fit. The three
// __builtin_*_overflow calls do exactly that and are the only sane
// implementation -- a hand-written check would have to detect signed
// overflow without committing it, which is the very thing that is
// undefined.
//
// In C these are type-generic macros; C++26 makes them function templates,
// which is why the result pointer's type is deduced separately from the two
// operands: ckd_add(&r, a, b) is allowed to mix three different integer
// types and is defined in terms of the mathematical result, not of any
// promotion between them.
//
// Not constexpr, for the same [constexpr.functions] reason as <stdbit.h>:
// the standard does not say so. (The builtins themselves would work in a
// constant expression.)
#ifndef BOXCXX_STDCKDINT_H
#define BOXCXX_STDCKDINT_H

#include <type_traits>

// [version.syn] names this header as an owner of the macro below;
// nothing else here is declared, so nothing else is answered for.
#define BOXCXX_OWNS_stdckdint_h
#include <__bits/version_stdckdint>

#define __STDC_VERSION_STDCKDINT_H__ 202311L

namespace std {
namespace __ckd {

// C23 7.20.1: the operands and the result must be signed or unsigned
// integer types -- NOT bool, NOT a character type, NOT an enumeration.
template <class T>
concept CheckedInteger =
    is_integral_v<T> &&
    !__bits::IsAnyOf<remove_cv_t<T>, bool, char, wchar_t, char8_t, char16_t,
                     char32_t>;

} // namespace __ckd
} // namespace std

template <std::__ckd::CheckedInteger R, std::__ckd::CheckedInteger A,
          std::__ckd::CheckedInteger B>
inline bool ckd_add(R *result, A a, B b)
{
    return __builtin_add_overflow(a, b, result);
}

template <std::__ckd::CheckedInteger R, std::__ckd::CheckedInteger A,
          std::__ckd::CheckedInteger B>
inline bool ckd_sub(R *result, A a, B b)
{
    return __builtin_sub_overflow(a, b, result);
}

template <std::__ckd::CheckedInteger R, std::__ckd::CheckedInteger A,
          std::__ckd::CheckedInteger B>
inline bool ckd_mul(R *result, A a, B b)
{
    return __builtin_mul_overflow(a, b, result);
}

#endif // BOXCXX_STDCKDINT_H
