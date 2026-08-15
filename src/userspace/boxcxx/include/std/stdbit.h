// boxcxx — <stdbit.h>  ([stdbit.h.syn], C++26 adopting C23 7.18)
//
// The C spelling of what <bit> already does. Every function here is one
// line over a std:: counterpart -- the header exists so C code moved into
// BoxOS's C++ userspace keeps compiling, and so C23's four-way naming
// (generic template, _uc/_us/_ui/_ul/_ull suffixes) resolves to the same
// implementation instead of a second one.
//
// Two things are NOT copied from the obvious model:
//
//   * stdc_bit_ceil is NOT "msb set => 0". libstdc++ 16.1 writes it that
//     way, and it is wrong on exactly the powers of two that have the top
//     bit set: stdc_bit_ceil_uc(0x80) returns 0 there, but 0x80 IS the
//     minimal power of two not less than 0x80 and IS representable in
//     unsigned char, so C23 7.18.15.3 asks for 0x80. Measured, not read:
//     g++-16 prints 0 for both 0x80 (uc) and 0x80000000 (ui). The
//     representability test is value > msb, not value & msb.
//
//   * Nothing here is constexpr. [constexpr.functions] forbids declaring a
//     standard library signature constexpr unless the standard says so,
//     and [stdbit.h.syn] does not -- even though every std:: function
//     underneath is constexpr. A caller who wants the constant-expression
//     form has <bit>.
#ifndef BOXCXX_STDBIT_H
#define BOXCXX_STDBIT_H

#include <bit>
#include <limits>

// [version.syn] names this header as an owner of the macro below;
// nothing else here is declared, so nothing else is answered for.
#define BOXCXX_OWNS_stdbit_h
#include <__bits/version_stdbit>

#define __STDC_VERSION_STDBIT_H__ 202311L

#define __STDC_ENDIAN_BIG__    __ORDER_BIG_ENDIAN__
#define __STDC_ENDIAN_LITTLE__ __ORDER_LITTLE_ENDIAN__
#define __STDC_ENDIAN_NATIVE__ __BYTE_ORDER__

// C23 names these in the global namespace, which is where they are
// defined; the generic form is a constrained template so that passing a
// signed type is a substitution failure with a readable message rather
// than a silent conversion to unsigned.

template <std::__bits::BitOperand T>
inline unsigned int stdc_leading_zeros(T value)
{
    return static_cast<unsigned int>(std::countl_zero(value));
}
inline unsigned int stdc_leading_zeros_uc(unsigned char value)
{
    return stdc_leading_zeros(value);
}
inline unsigned int stdc_leading_zeros_us(unsigned short value)
{
    return stdc_leading_zeros(value);
}
inline unsigned int stdc_leading_zeros_ui(unsigned int value)
{
    return stdc_leading_zeros(value);
}
inline unsigned int stdc_leading_zeros_ul(unsigned long value)
{
    return stdc_leading_zeros(value);
}
inline unsigned int stdc_leading_zeros_ull(unsigned long long value)
{
    return stdc_leading_zeros(value);
}

template <std::__bits::BitOperand T>
inline unsigned int stdc_leading_ones(T value)
{
    return static_cast<unsigned int>(std::countl_one(value));
}
inline unsigned int stdc_leading_ones_uc(unsigned char value)
{
    return stdc_leading_ones(value);
}
inline unsigned int stdc_leading_ones_us(unsigned short value)
{
    return stdc_leading_ones(value);
}
inline unsigned int stdc_leading_ones_ui(unsigned int value)
{
    return stdc_leading_ones(value);
}
inline unsigned int stdc_leading_ones_ul(unsigned long value)
{
    return stdc_leading_ones(value);
}
inline unsigned int stdc_leading_ones_ull(unsigned long long value)
{
    return stdc_leading_ones(value);
}

template <std::__bits::BitOperand T>
inline unsigned int stdc_trailing_zeros(T value)
{
    return static_cast<unsigned int>(std::countr_zero(value));
}
inline unsigned int stdc_trailing_zeros_uc(unsigned char value)
{
    return stdc_trailing_zeros(value);
}
inline unsigned int stdc_trailing_zeros_us(unsigned short value)
{
    return stdc_trailing_zeros(value);
}
inline unsigned int stdc_trailing_zeros_ui(unsigned int value)
{
    return stdc_trailing_zeros(value);
}
inline unsigned int stdc_trailing_zeros_ul(unsigned long value)
{
    return stdc_trailing_zeros(value);
}
inline unsigned int stdc_trailing_zeros_ull(unsigned long long value)
{
    return stdc_trailing_zeros(value);
}

template <std::__bits::BitOperand T>
inline unsigned int stdc_trailing_ones(T value)
{
    return static_cast<unsigned int>(std::countr_one(value));
}
inline unsigned int stdc_trailing_ones_uc(unsigned char value)
{
    return stdc_trailing_ones(value);
}
inline unsigned int stdc_trailing_ones_us(unsigned short value)
{
    return stdc_trailing_ones(value);
}
inline unsigned int stdc_trailing_ones_ui(unsigned int value)
{
    return stdc_trailing_ones(value);
}
inline unsigned int stdc_trailing_ones_ul(unsigned long value)
{
    return stdc_trailing_ones(value);
}
inline unsigned int stdc_trailing_ones_ull(unsigned long long value)
{
    return stdc_trailing_ones(value);
}

// The four "first_*" functions are 1-based indices from the named end, and
// return 0 when there is no such bit -- C23's way of folding "not found"
// into the same unsigned result.

template <std::__bits::BitOperand T>
inline unsigned int stdc_first_leading_zero(T value)
{
    return value == static_cast<T>(-1)
               ? 0u
               : 1u + static_cast<unsigned int>(std::countl_one(value));
}
inline unsigned int stdc_first_leading_zero_uc(unsigned char value)
{
    return stdc_first_leading_zero(value);
}
inline unsigned int stdc_first_leading_zero_us(unsigned short value)
{
    return stdc_first_leading_zero(value);
}
inline unsigned int stdc_first_leading_zero_ui(unsigned int value)
{
    return stdc_first_leading_zero(value);
}
inline unsigned int stdc_first_leading_zero_ul(unsigned long value)
{
    return stdc_first_leading_zero(value);
}
inline unsigned int stdc_first_leading_zero_ull(unsigned long long value)
{
    return stdc_first_leading_zero(value);
}

template <std::__bits::BitOperand T>
inline unsigned int stdc_first_leading_one(T value)
{
    return value == 0 ? 0u
                      : 1u + static_cast<unsigned int>(std::countl_zero(value));
}
inline unsigned int stdc_first_leading_one_uc(unsigned char value)
{
    return stdc_first_leading_one(value);
}
inline unsigned int stdc_first_leading_one_us(unsigned short value)
{
    return stdc_first_leading_one(value);
}
inline unsigned int stdc_first_leading_one_ui(unsigned int value)
{
    return stdc_first_leading_one(value);
}
inline unsigned int stdc_first_leading_one_ul(unsigned long value)
{
    return stdc_first_leading_one(value);
}
inline unsigned int stdc_first_leading_one_ull(unsigned long long value)
{
    return stdc_first_leading_one(value);
}

template <std::__bits::BitOperand T>
inline unsigned int stdc_first_trailing_zero(T value)
{
    return value == static_cast<T>(-1)
               ? 0u
               : 1u + static_cast<unsigned int>(std::countr_one(value));
}
inline unsigned int stdc_first_trailing_zero_uc(unsigned char value)
{
    return stdc_first_trailing_zero(value);
}
inline unsigned int stdc_first_trailing_zero_us(unsigned short value)
{
    return stdc_first_trailing_zero(value);
}
inline unsigned int stdc_first_trailing_zero_ui(unsigned int value)
{
    return stdc_first_trailing_zero(value);
}
inline unsigned int stdc_first_trailing_zero_ul(unsigned long value)
{
    return stdc_first_trailing_zero(value);
}
inline unsigned int stdc_first_trailing_zero_ull(unsigned long long value)
{
    return stdc_first_trailing_zero(value);
}

template <std::__bits::BitOperand T>
inline unsigned int stdc_first_trailing_one(T value)
{
    return value == 0 ? 0u
                      : 1u + static_cast<unsigned int>(std::countr_zero(value));
}
inline unsigned int stdc_first_trailing_one_uc(unsigned char value)
{
    return stdc_first_trailing_one(value);
}
inline unsigned int stdc_first_trailing_one_us(unsigned short value)
{
    return stdc_first_trailing_one(value);
}
inline unsigned int stdc_first_trailing_one_ui(unsigned int value)
{
    return stdc_first_trailing_one(value);
}
inline unsigned int stdc_first_trailing_one_ul(unsigned long value)
{
    return stdc_first_trailing_one(value);
}
inline unsigned int stdc_first_trailing_one_ull(unsigned long long value)
{
    return stdc_first_trailing_one(value);
}

template <std::__bits::BitOperand T>
inline unsigned int stdc_count_zeros(T value)
{
    return static_cast<unsigned int>(std::popcount(static_cast<T>(~value)));
}
inline unsigned int stdc_count_zeros_uc(unsigned char value)
{
    return stdc_count_zeros(value);
}
inline unsigned int stdc_count_zeros_us(unsigned short value)
{
    return stdc_count_zeros(value);
}
inline unsigned int stdc_count_zeros_ui(unsigned int value)
{
    return stdc_count_zeros(value);
}
inline unsigned int stdc_count_zeros_ul(unsigned long value)
{
    return stdc_count_zeros(value);
}
inline unsigned int stdc_count_zeros_ull(unsigned long long value)
{
    return stdc_count_zeros(value);
}

template <std::__bits::BitOperand T>
inline unsigned int stdc_count_ones(T value)
{
    return static_cast<unsigned int>(std::popcount(value));
}
inline unsigned int stdc_count_ones_uc(unsigned char value)
{
    return stdc_count_ones(value);
}
inline unsigned int stdc_count_ones_us(unsigned short value)
{
    return stdc_count_ones(value);
}
inline unsigned int stdc_count_ones_ui(unsigned int value)
{
    return stdc_count_ones(value);
}
inline unsigned int stdc_count_ones_ul(unsigned long value)
{
    return stdc_count_ones(value);
}
inline unsigned int stdc_count_ones_ull(unsigned long long value)
{
    return stdc_count_ones(value);
}

template <std::__bits::BitOperand T>
inline bool stdc_has_single_bit(T value)
{
    return std::has_single_bit(value);
}
inline bool stdc_has_single_bit_uc(unsigned char value)
{
    return stdc_has_single_bit(value);
}
inline bool stdc_has_single_bit_us(unsigned short value)
{
    return stdc_has_single_bit(value);
}
inline bool stdc_has_single_bit_ui(unsigned int value)
{
    return stdc_has_single_bit(value);
}
inline bool stdc_has_single_bit_ul(unsigned long value)
{
    return stdc_has_single_bit(value);
}
inline bool stdc_has_single_bit_ull(unsigned long long value)
{
    return stdc_has_single_bit(value);
}

template <std::__bits::BitOperand T>
inline unsigned int stdc_bit_width(T value)
{
    return static_cast<unsigned int>(std::bit_width(value));
}
inline unsigned int stdc_bit_width_uc(unsigned char value)
{
    return stdc_bit_width(value);
}
inline unsigned int stdc_bit_width_us(unsigned short value)
{
    return stdc_bit_width(value);
}
inline unsigned int stdc_bit_width_ui(unsigned int value)
{
    return stdc_bit_width(value);
}
inline unsigned int stdc_bit_width_ul(unsigned long value)
{
    return stdc_bit_width(value);
}
inline unsigned int stdc_bit_width_ull(unsigned long long value)
{
    return stdc_bit_width(value);
}

template <std::__bits::BitOperand T>
inline T stdc_bit_floor(T value)
{
    return std::bit_floor(value);
}
inline unsigned char stdc_bit_floor_uc(unsigned char value)
{
    return stdc_bit_floor(value);
}
inline unsigned short stdc_bit_floor_us(unsigned short value)
{
    return stdc_bit_floor(value);
}
inline unsigned int stdc_bit_floor_ui(unsigned int value)
{
    return stdc_bit_floor(value);
}
inline unsigned long stdc_bit_floor_ul(unsigned long value)
{
    return stdc_bit_floor(value);
}
inline unsigned long long stdc_bit_floor_ull(unsigned long long value)
{
    return stdc_bit_floor(value);
}

// Unlike std::bit_ceil, an unrepresentable result is 0 rather than
// undefined -- and "unrepresentable" means strictly above the top bit, so
// the top bit itself still ceils to itself (see the file header).
template <std::__bits::BitOperand T>
inline T stdc_bit_ceil(T value)
{
    constexpr T kMsb = static_cast<T>(T(1) << (std::numeric_limits<T>::digits - 1));
    return value > kMsb ? T(0) : std::bit_ceil(value);
}
inline unsigned char stdc_bit_ceil_uc(unsigned char value)
{
    return stdc_bit_ceil(value);
}
inline unsigned short stdc_bit_ceil_us(unsigned short value)
{
    return stdc_bit_ceil(value);
}
inline unsigned int stdc_bit_ceil_ui(unsigned int value)
{
    return stdc_bit_ceil(value);
}
inline unsigned long stdc_bit_ceil_ul(unsigned long value)
{
    return stdc_bit_ceil(value);
}
inline unsigned long long stdc_bit_ceil_ull(unsigned long long value)
{
    return stdc_bit_ceil(value);
}

#endif // BOXCXX_STDBIT_H
