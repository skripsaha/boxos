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
#   SYNOPSIS every macro is visible from <version> itself
#
# Usage:  tools/cxx_ftm_audit.sh [-v]      exit 0 iff OWNED and SYNOPSIS hold
#                                          and the leak count has not grown
set -u
LEAK_BUDGET=153
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
