#!/bin/sh
# cxx_ftm_audit.sh — check boxcxx's feature-test macros against [version.syn].
#
# [support.limits.general] says each __cpp_lib_* macro is defined by <version>
# AND by the header that provides the feature. CONFORMANCE.md used to state
# that this had been checked across the whole cross-product; it had been, once,
# by hand, and by Ф32-i it was no longer true — __cpp_lib_nonmember_container_
# access had gone missing from seven of the twelve headers that own it. A claim
# nothing re-checks is a claim with a shelf life, so this is the re-check.
#
# Three properties, all over the FULL cross-product of macros and headers:
#   OWNED   every macro boxcxx defines is visible from every header
#           [version.syn] says owns it (and that boxcxx provides)
#   EXACT   how many macros are visible from a header that does NOT own one.
#           This is a RATCHET, not a pass/fail: the standard sets a floor, not
#           a ceiling, and full exactness is not reachable by any header-only
#           library. A header that includes another inherits its macros --
#           <vector> includes <memory>, so <vector> sees allocate_at_least --
#           and no amount of gating undoes that. libstdc++ leaks for the same
#           reason and is only lower because its headers include less of each
#           other. So the number is pinned: growing it fails, shrinking it
#           means re-pin lower. Measured with the BOXCXX_OWNS_ gating in place.
#
#           One legitimate way to grow it: ADDING a macro whose owning header
#           is itself widely included. Ф33 raised the pin from 147 to 153 for
#           exactly six of those -- constant_wrapper and function_ref
#           (<utility> / <functional>, which nearly everything pulls in),
#           is_virtual_base_of (<type_traits>), optional_range_support
#           (<optional>), ranges_indices (<ranges>) and to_string (<string>).
#           Four other new macros -- bitset, debugging, inplace_vector,
#           philox_engine -- do not leak at all, because nothing else includes
#           their headers. When re-pinning, account for the delta macro by
#           macro: it must be new macros, not a header that started including
#           more than it used to.
#
#           Ф34 raised it 153 -> 158, again macro by macro: constrained_
#           equality (<utility> and four more), algorithm_default_value_type
#           (<algorithm>/<memory>/five containers), copyable_function
#           (<functional>), smart_ptr_owner_equality (<memory>) and
#           format_uchar (<format>) each own a widely-included header. The
#           other three macros Ф34 added -- stdatomic_h, stdbit_h and
#           stdckdint_h -- leak nowhere at all, because no header in the
#           tree includes a C-compatibility header.
#
#           Ф43-f raised it 172 -> 191, the largest jump the pin has ever
#           taken, and the delta is EXACTLY the freestanding family: 26 macros
#           arrived and 19 of them leak, because their owners are <utility>,
#           <memory>, <iterator>, <algorithm>, <tuple>, <ranges>, <optional>,
#           <variant>, <array>, <string_view>, <charconv>, <cstdlib>, <new>,
#           <functional>, <numeric>, <string>, <cerrno> and <ratio> — which is
#           most of what every other header includes. Measured as a set
#           difference: the 172 that leaked before still leak, none stopped,
#           and nothing outside the family started. The seven that do NOT leak
#           are cstring, cwchar, execution, expected, mdspan, random and
#           feature_test_macros, whose owners nothing else includes.
#
#           Ф43-e-2 raised it 171 -> 172, and the delta is ONE macro:
#           __cpp_lib_format, whose owner <format> is included by <chrono>,
#           <ostream>, <iostream>, <bitset>, <complex> and eight more, so
#           claiming it at last necessarily made it visible from all of them.
#           The count is of MACROS, and exactly one macro was added to the
#           library in that commit; nothing started leaking that had not been.
#
#           Ф42-g raised it 170 -> 171, and the delta is ONE macro reaching
#           ONE header: __cpp_lib_formatters, whose owners are <stacktrace>
#           and <thread>, becomes visible from <future> because <future>
#           includes <thread>. Measured as a set difference against the
#           previous run: exactly that pair, and nothing stopped leaking.
#
#           Ф40's fourth commit raised it 169 -> 170, and the delta is ONE
#           macro: __cpp_lib_parallel_algorithm, whose owners are <algorithm>
#           and <numeric> and which therefore reaches everything <algorithm>
#           reaches, which is most of the library. Measured as a set
#           difference against the previous run: exactly that macro, nothing
#           else, and nothing stopped leaking either.
#
#           Ф40's third commit raised it 165 -> 169, and the delta is FOUR
#           macros, all of them <numeric>'s: constexpr_numeric, gcd_lcm,
#           ranges_iota and saturation_arithmetic. None of them had ever
#           leaked ANYWHERE, because nothing in the tree included <numeric> --
#           and <execution> does, since [numeric.ops.overview] is where half
#           the policy overloads are declared. Measured as a set difference
#           with the header moved aside: without <execution> the count is
#           exactly 165 again, so nothing else in that commit widened
#           anything. The new leaves (execution_policy, par_engine, par_algo,
#           par_numeric) define no macros at all.
#
#           Ф40's first commit raised it 164 -> 165, and the delta is ONE
#           macro: __cpp_lib_syncbuf. Its owners are <syncstream> and
#           <iosfwd>, and <iosfwd> is the most widely included header in the
#           stream family -- every stream header takes it, and through them
#           twenty-odd others -- so the macro reaches all of them the moment
#           it exists. The phase's new header, <syncstream>, leaks nothing
#           itself: nothing in the tree includes it, and every macro it sees
#           through <ostream>/<sstream>/<memory> was already leaking
#           elsewhere before this header existed.
#
#           Ф39's second commit raised it 163 -> 164, and the delta is again
#           ONE macro: __cpp_lib_constexpr_vector, owned by <vector>, which
#           <ios> includes -- so it reaches every stream header, and through
#           them <bitset>, <chrono> and the rest of the twenty. Same shape as
#           the first commit: a widely-included owning header, no header
#           answering for anything new.
#
#           Ф39 raised it 162 -> 163, and the delta is ONE macro:
#           __cpp_lib_constexpr_string, owned by <string>, which almost
#           everything includes -- it leaks into 20-odd headers and they are
#           all headers that were already taking <string>'s other macros.
#           Measured as a set difference: with the phase's <string> changes
#           in place but the macro still undefined the count was 162.
#           __cpp_lib_constexpr_bitset, defined in the same commit, leaks
#           nowhere at all: nothing in the tree includes <bitset>.
#
#           Ф38 raised it 161 -> 162, and the delta is ONE macro:
#           __cpp_lib_make_obj_using_allocator, owned by <memory>, which
#           nearly every container pulls in. Measured as a set difference
#           rather than asserted -- with the phase's new headers present but
#           the macro still undefined the count was 161, and defining it made
#           it 162 with nothing else moving. The phase's other two new
#           headers, <spanstream> and <scoped_allocator>, leak nothing at
#           all, because nothing in the tree includes either.
#
#           Ф35 raised it 158 -> 161, and the three are: indirect and
#           polymorphic (owned by <memory>, which nearly every container
#           pulls in), plus __cpp_lib_span, which had never leaked ANYWHERE
#           because nothing included <span> -- and now <mdspan> does, since
#           extents and mdspan both take one. That third is the whole delta
#           from adding a header, not from a header answering for more than
#           it provides. mdspan, aligned_accessor and submdspan leak
#           nowhere: nothing includes <mdspan>.
#   SYNOPSIS every macro is visible from <version> itself
#
# Usage:  tools/cxx_ftm_audit.sh [-v]      exit 0 iff OWNED and SYNOPSIS hold
#                                          and the leak count has not grown
set -u
LEAK_BUDGET=191
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT" || exit 2
VERBOSE=${1:-}

STD=src/userspace/boxcxx/include/std
MAP=tools/version_syn_owners.txt
CXX=${CXX:-x86_64-elf-g++}
WORK=$(mktemp -d) || exit 2
trap 'rm -rf "$WORK"' EXIT

INC="-I$STD -I src/userspace/boxcxx/include -I src/userspace/boxlib/include -I src/include"
FLAGS="-std=gnu++23 -nostdinc++ $INC"

visible() {  # $1 = header name -> stdout: sorted macro names
    printf '#include <%s>\n' "$1" > "$WORK/tu.cpp"
    $CXX $FLAGS -dM -E "$WORK/tu.cpp" 2>/dev/null |
        sed -n 's/^#define \(__cpp_lib_[a-z_0-9]*\) .*/\1/p' | sort -u
}

printf 'boxcxx feature-test macro audit\n'
visible version > "$WORK/version.txt"
DEFINED=$(wc -l < "$WORK/version.txt" | tr -d ' ')
printf '  macros defined by <version>: %s\n' "$DEFINED"

for h in "$STD"/*; do
    [ -f "$h" ] || continue
    n=$(basename "$h")
    visible "$n" > "$WORK/vis.$n"
done
HDRS=$(ls "$WORK"/vis.* | wc -l | tr -d ' ')
printf '  headers probed: %s\n\n' "$HDRS"

FAIL=0
while IFS='|' read -r m owners; do
    case "$m" in \#*|'') continue ;; esac
    grep -qx "$m" "$WORK/version.txt" || continue   # not defined here: not our problem
    # OWNED
    for o in $(printf '%s' "$owners" | tr ',' ' '); do
        [ -f "$WORK/vis.$o" ] || continue           # header not provided by boxcxx
        grep -qx "$m" "$WORK/vis.$o" || {
            printf 'OWNED  %s is not visible from <%s>\n' "$m" "$o"; FAIL=1; }
    done
    # EXACT
    for f in "$WORK"/vis.*; do
        n=${f##*/vis.}
        [ "$n" = version ] && continue
        grep -qx "$m" "$f" || continue
        printf '%s\n' "$owners" | tr ',' '\n' | grep -qx "$n" || {
            [ -n "$VERBOSE" ] && printf 'EXACT  %s leaks into <%s>\n' "$m" "$n"
            echo "$m" >> "$WORK/leaks"; }
    done
done < "$MAP"

# MAPPED — every macro boxcxx defines must be IN the map, or the OWNED check
# above skipped it in silence. Ф41 found seven [version.syn] macros missing
# from the transcription and one that does not exist in [version.syn] at all
# (__cpp_lib_spans, a phantom). Neither could produce a false green that day,
# because boxcxx defines none of them — but "the day boxcxx defines one" is
# exactly when a missing line stops being harmless, and nothing was watching
# for it. Now something is: this check makes the map's completeness a property
# of the gate rather than of whoever last edited the file.
while read -r m; do
    grep -q "^$m|" "$MAP" || {
        printf 'MAPPED %s is defined but has no [version.syn] ownership line in %s\n' \
            "$m" "$MAP"; FAIL=1; }
done < "$WORK/version.txt"

# SYNOPSIS — a macro an owning header defines must reach <version> as well.
for f in "$WORK"/vis.*; do
    n=${f##*/vis.}
    [ "$n" = version ] && continue
    while read -r m; do
        grep -qx "$m" "$WORK/version.txt" ||
            { printf 'SYNOPSIS %s is visible from <%s> but not from <version>\n' "$m" "$n"; FAIL=1; }
    done < "$f"
done

LEAKS=0
[ -f "$WORK/leaks" ] && LEAKS=$(sort -u "$WORK/leaks" | wc -l | tr -d ' ')
printf 'EXACT  %s macros reachable from a non-owning header (pinned at %s)' \
    "$LEAKS" "$LEAK_BUDGET"
[ -n "$VERBOSE" ] || printf ' — -v lists them'
printf '\n'
if [ "$LEAKS" -gt "$LEAK_BUDGET" ]; then
    printf 'EXACT  ...which is MORE than the pin. A header started answering for\n'
    printf '       a feature it does not provide; find it with -v.\n'
    FAIL=1
elif [ "$LEAKS" -lt "$LEAK_BUDGET" ]; then
    printf 'EXACT  ...which is FEWER than the pin. Lower LEAK_BUDGET to %s so the\n' "$LEAKS"
    printf '       ratchet keeps its grip.\n'
    FAIL=1
fi

if [ "$FAIL" = 0 ]; then
    printf '\nOK — all %s macros are visible from <version> and from every header\n' "$DEFINED"
    printf '     [version.syn] names as an owner, across all %s x %s pairs, and\n' "$DEFINED" "$HDRS"
    printf '     the reach into non-owning headers has not grown.\n'
else
    printf '\nFAILED\n'
fi
exit $FAIL
