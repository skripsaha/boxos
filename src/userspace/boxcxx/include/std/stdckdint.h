#ifndef BOXCXX_STDCKDINT_H
#define BOXCXX_STDCKDINT_H

#include <type_traits>

#define BOXCXX_OWNS_stdckdint_h
#include <__bits/version_stdckdint>

#define __STDC_VERSION_STDCKDINT_H__ 202311L

namespace std {
namespace __ckd {

template <class T>
concept CheckedInteger =
    is_integral_v<T> &&
    !__bits::IsAnyOf<remove_cv_t<T>, bool, char, wchar_t, char8_t, char16_t,
                     char32_t>;

}
}

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

#endif