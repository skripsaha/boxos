#!/bin/sh
# cxx_freestanding_audit.sh — check boxcxx's __cpp_lib_freestanding_* claims
# against the freestanding subsets the working draft actually defines.
#
# [compliance] does not write the subsets out as lists. Each one is spelled
# inside its header's synopsis, as a `// freestanding` comment on the
# declarations that belong to it. tools/gen_freestanding_manifest.py fetches
# those marks and writes tools/freestanding_manifest.txt; this script is the
# half that runs offline, compiles a probe per macro against the real
# cross-compiler in freestanding mode, and answers one question per macro:
#
#     is every entity the subset requires actually here?
#
# Then it compares the answer against what <version> claims, in both
# directions:
#   OVERCLAIM   a macro is defined and the probe does not compile.  Fatal —
#               that is a promise the library cannot keep, and the whole point
#               of the epic's macro rule is that a defined macro can be trusted.
#   UNDERCLAIM  the probe compiles and the macro is not defined.  Reported, not
#               fatal: a macro may be held back on purpose (the reason belongs
#               in CONFORMANCE.md), but it must never be held back by accident.
#
# Usage:  tools/cxx_freestanding_audit.sh [-v]
#   -v   list every entity of every stem as it is probed
#
# Ф43-f built it. Before that the twenty-six freestanding markers were the
# largest block of undefined macros in the library, and CONFORMANCE.md called
# them "an unrun [compliance] audit" — which was honest and is the kind of
# sentence a tool is supposed to retire.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT" || exit 2

BX=src/userspace/boxcxx
MANIFEST=tools/freestanding_manifest.txt
CXX=${CXX:-x86_64-elf-g++}
FLAGS="-std=gnu++23 -nostdinc++ -ffreestanding -fno-builtin -fexceptions -frtti
       -I include/std -I include -I ../boxlib/include -I ../../include -fsyntax-only"
VERBOSE=0
[ $# -gt 0 ] && [ "$1" = "-v" ] && VERBOSE=1

[ -f "$MANIFEST" ] || { echo "no $MANIFEST — run tools/gen_freestanding_manifest.py"; exit 2; }
command -v "$CXX" >/dev/null || { echo "$CXX not found"; exit 2; }

TMP=$(mktemp -d) || exit 2
trap 'rm -rf "$TMP"' EXIT

# Does one translation unit compile?
try() { ( cd "$BX" && $CXX $FLAGS "$1" ) >"$TMP/err" 2>&1; }

# A name may live in std, in std::ranges (the range algorithms and the
# [range.iter.ops] family) or in std::placeholders. A using-declaration is the
# probe that works for all of overload sets, class templates and typedefs
# alike, which is why the probe is written this way and not with decltype.
one_name() {  # stem_header name -> 0 if the name exists somewhere legal
    for ns in std std::ranges std::placeholders; do
        printf '#include %s\nnamespace probe { using %s::%s; }\n' "$1" "$ns" "$2" \
            > "$TMP/one.cpp"
        try "$TMP/one.cpp" && return 0
    done
    return 1
}

# The macro a stem belongs to. errc is spelled across two headers and
# feature_test_macros has no header of its own.
macro_of() {
    case "$1" in
        errc.*) echo "__cpp_lib_freestanding_errc" ;;
        *)      echo "__cpp_lib_freestanding_$1" ;;
    esac
}

STEMS=$(awk '$1=="header"{print $2}' "$MANIFEST")
PASSED=""; FAILED=""
for stem in $STEMS; do
    hdr=$(awk -v s="$stem" '$1=="header"&&$2==s{print $3}' "$MANIFEST")
    kind=$(awk -v s="$stem" '$1=="header"&&$2==s{print $4}' "$MANIFEST")
    names=$(awk -v s="$stem" '$1=="name"&&$2==s{print $3}' "$MANIFEST")
    macros=$(awk -v s="$stem" '$1=="macro"&&$2==s{print $3}' "$MANIFEST")

    # The bulk probe: the header, plus every required name at once. Only when
    # it fails is anything probed one at a time, so the slow path costs
    # nothing on a green tree.
    {
        printf '#include %s\nnamespace probe {\n' "$hdr"
        for n in $names; do printf 'using std::%s;\n' "$n"; done
        printf '}\n'
        for m in $macros; do printf '#ifndef %s\n#error MISSING\n#endif\n' "$m"; done
    } > "$TMP/bulk.cpp"

    miss=""
    if ! try "$TMP/bulk.cpp"; then
        for n in $names; do
            one_name "$hdr" "$n" || miss="$miss $n"
        done
        for m in $macros; do
            printf '#include %s\n#ifndef %s\n#error MISSING\n#endif\n' "$hdr" "$m" \
                > "$TMP/one.cpp"
            try "$TMP/one.cpp" || miss="$miss $m"
        done
        # A header that will not compile at all reports no missing entity, and
        # that must not read as success.
        if [ -z "$miss" ]; then
            printf '#include %s\n' "$hdr" > "$TMP/one.cpp"
            try "$TMP/one.cpp" || miss=" <$hdr does not compile freestanding>"
        fi
    fi

    n_names=$(echo $names | wc -w | tr -d ' ')
    n_macros=$(echo $macros | wc -w | tr -d ' ')
    if [ -z "$miss" ]; then
        [ "$VERBOSE" = 1 ] && printf '  %-14s %-8s %s names, %s macros — all present\n' \
            "$stem" "$kind" "$n_names" "$n_macros"
        PASSED="$PASSED $(macro_of "$stem")"
    else
        printf 'MISSING  %-14s %s\n' "$stem" "$miss"
        FAILED="$FAILED $(macro_of "$stem")"
    fi
done

# __cpp_lib_freestanding_feature_test_macros has no entity list: it says the
# implementation defines the feature-test macros in a freestanding environment.
# boxcxx has no hosted mode at all, so the probe is that <version> works and
# reports macros with nothing but the freestanding flags in force.
printf '#include <version>\n#ifndef __cpp_lib_freestanding_feature_test_macros\n#error MISSING\n#endif\n' \
    > "$TMP/ftm.cpp"
if try "$TMP/ftm.cpp"; then
    PASSED="$PASSED __cpp_lib_freestanding_feature_test_macros"
else
    printf '#include <version>\nstatic_assert(__cpp_lib_optional > 0);\n' > "$TMP/ftm.cpp"
    if try "$TMP/ftm.cpp"; then
        printf 'UNDEFINED __cpp_lib_freestanding_feature_test_macros (but <version> works freestanding)\n'
        FAILED="$FAILED __cpp_lib_freestanding_feature_test_macros"
    else
        printf 'MISSING  feature_test_macros  <version> does not work freestanding\n'
        FAILED="$FAILED __cpp_lib_freestanding_feature_test_macros"
    fi
fi

# What does the library actually claim? Ask a translation unit, not a grep.
printf '#include <version>\n' > "$TMP/v.cpp"
( cd "$BX" && $CXX -std=gnu++23 -nostdinc++ -ffreestanding -fno-builtin \
    -I include/std -I include -I ../boxlib/include -I ../../include \
    -dM -E "$TMP/v.cpp" ) 2>/dev/null \
    | sed -n 's/^#define \(__cpp_lib_freestanding_[a-z_]*\) .*/\1/p' | sort > "$TMP/claimed"
echo "$PASSED" | tr ' ' '\n' | grep . | sort -u > "$TMP/earned"

echo
echo "boxcxx freestanding audit"
echo "  stems probed:  $(echo "$STEMS" | wc -w | tr -d ' ') (+ feature_test_macros)"
echo "  earned:        $(wc -l < "$TMP/earned" | tr -d ' ')"
echo "  claimed:       $(wc -l < "$TMP/claimed" | tr -d ' ')"

RC=0
OVER=$(comm -13 "$TMP/earned" "$TMP/claimed")
UNDER=$(comm -23 "$TMP/earned" "$TMP/claimed")
if [ -n "$OVER" ]; then
    echo
    echo "OVERCLAIM — defined, but the subset is not all here:"
    echo "$OVER" | sed 's/^/     /'
    RC=1
fi
if [ -n "$UNDER" ]; then
    echo
    echo "UNDERCLAIM — the subset is all here and the macro is not defined:"
    echo "$UNDER" | sed 's/^/     /'
    echo "     (deliberate holds belong in CONFORMANCE.md; accidents do not)"
    [ "$RC" = 0 ] && RC=3
fi
[ "$RC" = 0 ] && echo "
OK — every freestanding macro boxcxx defines is one it can keep, and every
     subset it provides in full is one it claims."
exit $RC
