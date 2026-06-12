/*
 * cxxtest_traits.cpp — Phase 2 compile-time torture suite for the boxcxx
 * freestanding header core. Everything here is static_assert'ed: if this
 * TU compiles, phase 2 passes; the runtime hook only reports that fact.
 */

#include <type_traits>
#include <concepts>
#include <utility>
#include <tuple>
#include <functional>
#include <compare>
#include <limits>
#include <bit>
#include <numbers>
#include <source_location>

namespace {

using namespace std;

// ── type_traits ─────────────────────────────────────────────────────────

static_assert(is_void_v<const void> && !is_void_v<int>);
static_assert(is_integral_v<unsigned long long> && !is_integral_v<float>);
static_assert(is_floating_point_v<double>);
static_assert(is_array_v<int[3]> && is_bounded_array_v<int[3]> &&
              is_unbounded_array_v<int[]>);
static_assert(is_pointer_v<int *const>);
static_assert(is_function_v<int(int)> && is_function_v<void() const noexcept> &&
              !is_function_v<int (*)(int)>);

struct Probe {
    int m;
    int F(int);
};
static_assert(is_member_object_pointer_v<int Probe::*>);
static_assert(is_member_function_pointer_v<int (Probe::*)(int)>);

enum PlainEnum {};
enum class ScopedEnum : short {};
static_assert(is_enum_v<PlainEnum> && is_scoped_enum_v<ScopedEnum> &&
              !is_scoped_enum_v<PlainEnum>);
static_assert(is_same_v<underlying_type_t<ScopedEnum>, short>);
static_assert(is_same_v<make_unsigned_t<const int>, const unsigned int>);
static_assert(is_same_v<make_signed_t<ScopedEnum>, short>);
static_assert(is_same_v<remove_cvref_t<const int &&>, int>);
static_assert(is_same_v<decay_t<int[4]>, int *> &&
              is_same_v<decay_t<int(int)>, int (*)(int)>);
static_assert(is_convertible_v<int, double> && !is_convertible_v<int *, double *>);

struct NoexceptCtor {
    NoexceptCtor(int) noexcept;
    NoexceptCtor(double);
};
static_assert(is_nothrow_constructible_v<NoexceptCtor, int> &&
              !is_nothrow_constructible_v<NoexceptCtor, double>);
static_assert(is_destructible_v<int> && !is_destructible_v<void>);
static_assert(is_trivially_destructible_v<int>);
static_assert(is_same_v<common_type_t<int, long>, long>);
static_assert(is_same_v<common_reference_t<int &, const int &>, const int &>);
static_assert(is_same_v<common_reference_t<int &&, int &>, const int &>);

using TraitsFn = int(int);
static_assert(is_same_v<invoke_result_t<TraitsFn *, int>, int>);
static_assert(is_same_v<invoke_result_t<int Probe::*, Probe &>, int &>);
static_assert(is_same_v<invoke_result_t<int (Probe::*)(int), Probe *, int>, int>);
static_assert(is_invocable_v<TraitsFn, short> && !is_invocable_v<TraitsFn, int *>);
static_assert(is_invocable_r_v<long, TraitsFn, int>);
static_assert(is_swappable_v<int> && !is_swappable_v<void>);
static_assert(extent_v<int[2][3], 1> == 3 && rank_v<int[2][3]> == 2);

// ── compare ─────────────────────────────────────────────────────────────

struct OrderedPair {
    int x;
    int y;
    friend constexpr auto operator<=>(const OrderedPair &,
                                      const OrderedPair &) = default;
};
static_assert(OrderedPair{1, 2} < OrderedPair{1, 3});
static_assert((OrderedPair{2, 0} <=> OrderedPair{1, 9}) > 0);
static_assert(is_same_v<decltype(1.0 <=> 2.0), partial_ordering>);
static_assert(is_same_v<decltype(1 <=> 2), strong_ordering>);
static_assert(is_same_v<
              common_comparison_category_t<strong_ordering, partial_ordering>,
              partial_ordering>);
static_assert(three_way_comparable<int> && three_way_comparable_with<int, long>);
static_assert(compare_three_way{}(1, 2) < 0);

// ── concepts / functional ───────────────────────────────────────────────

static_assert(same_as<int, int> && !same_as<int, const int>);
static_assert(integral<bool> && signed_integral<int> && floating_point<float>);
static_assert(assignable_from<int &, int> && swappable<int>);
static_assert(movable<int> && copyable<int> && regular<int>);
static_assert(equality_comparable_with<int, long> && totally_ordered<int>);
static_assert(predicate<bool (*)(int), int>);

struct InvokeProbe {
    int v;
    constexpr int Add(int k) const { return v + k; }
};
static_assert(invoke(&InvokeProbe::Add, InvokeProbe{40}, 2) == 42);
static_assert(invoke(&InvokeProbe::v, InvokeProbe{40}) == 40);
static_assert(invoke([](int a, int b) { return a * b; }, 6, 7) == 42);
static_assert(invoke_r<long>([] { return 42; }) == 42L);
static_assert(identity{}(42) == 42 && plus<>{}(40, 2) == 42);

// ── utility / pair ──────────────────────────────────────────────────────

constexpr int UtilExchange()
{
    int a = 1;
    int old = exchange(a, 7);
    return a * 10 + old;
}
static_assert(UtilExchange() == 71);
static_assert(cmp_less(-1, 1u) && cmp_greater(2u, -5));
static_assert(in_range<signed char>(-128) && !in_range<signed char>(128));
static_assert(to_underlying(ScopedEnum{}) == 0);

template <size_t... I>
constexpr size_t SumIdx(index_sequence<I...>)
{
    return (0 + ... + I);
}
static_assert(SumIdx(make_index_sequence<5>{}) == 10);

constexpr pair<int, long> kPair{1, 2L};
static_assert(kPair.first == 1 && get<1>(kPair) == 2 && get<int>(kPair) == 1);
static_assert(pair<int, int>{1, 2} < pair<int, int>{1, 3});

// ── tuple ───────────────────────────────────────────────────────────────

constexpr tuple<int, char, long> kTuple{1, 'x', 3L};
static_assert(get<0>(kTuple) == 1 && get<char>(kTuple) == 'x');
static_assert(tuple_size_v<decltype(kTuple)> == 3);
static_assert(apply([](int a, char, long c) { return a + int(c); }, kTuple) == 4);
static_assert(get<2>(tuple_cat(make_tuple(1, 2), make_tuple('a'))) == 'a');
static_assert(tuple<int, int>{1, 2} < tuple<int, int>{1, 3});

struct EmptyTag {};
static_assert(sizeof(tuple<EmptyTag, int>) == sizeof(int));   // EBO via leaf

// ── limits / bit / numbers / source_location ────────────────────────────

static_assert(numeric_limits<int>::min() == -2147483647 - 1);
static_assert(numeric_limits<double>::is_iec559 &&
              numeric_limits<double>::digits == 53);
static_assert(numeric_limits<double>::infinity() >
              numeric_limits<double>::max());
static_assert(endian::native == endian::little);
static_assert(bit_cast<unsigned>(1.0f) == 0x3f800000u);
static_assert(byteswap<unsigned>(0x11223344u) == 0x44332211u);
static_assert(popcount(0xFFu) == 8 && countl_zero(uint8_t(1)) == 7);
static_assert(bit_ceil(100u) == 128u && bit_floor(100u) == 64u);
static_assert(rotl(uint8_t(0x81), 1) == 0x03);
static_assert(numbers::pi > 3.14159 && numbers::pi < 3.1416);

constexpr auto kHereLoc = source_location::current();
static_assert(kHereLoc.line() == __LINE__ - 1);

} // namespace

// Runtime hook for cxxtest.cpp: compiling this TU IS the test.
int CxxTraitsTortureCompiled()
{
    return 1;
}
