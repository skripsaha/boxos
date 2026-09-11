#ifndef BOXCXX_STDBIT_H
#define BOXCXX_STDBIT_H

#include <bit>
#include <limits>

#define BOXCXX_OWNS_stdbit_h
#include <__bits/version_stdbit>

#define __STDC_VERSION_STDBIT_H__ 202311L

#define __STDC_ENDIAN_BIG__    __ORDER_BIG_ENDIAN__
#define __STDC_ENDIAN_LITTLE__ __ORDER_LITTLE_ENDIAN__
#define __STDC_ENDIAN_NATIVE__ __BYTE_ORDER__


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

#endif