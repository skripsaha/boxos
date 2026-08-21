#!/bin/sh
# cxx_regex_oracle.sh — run the differential <regex> stand and report only what
# is NEW.
#
# Two reference implementations, one generated case sequence, one diff. Cells
# where the two disagree are cells where neither may be quoted as ground truth
# for boxcxx, so the set of them is PINNED in tools/regex_oracle_known.txt and
# this script reports additions and removals rather than the whole pile. A
# sweep that prints nothing has told you something.
#
# Usage:
#   tools/cxx_regex_oracle.sh                 sweep the pinned range, diff vs pin
#   tools/cxx_regex_oracle.sh 1 20000 2 3     sweep FROM TO DEPTH FAN
#   tools/cxx_regex_oracle.sh --cases         replay tools/regex_oracle_cases.txt
#   tools/cxx_regex_oracle.sh --adversarial   time the blowup shapes, one
#                                             process per case under a clock
#   tools/cxx_regex_oracle.sh --repin         overwrite the pin with today's set
#
# Ф44-0 built it, before the first line of the engine. Its first run found that
# libstdc++ 16.1 loses the meaning of `^` inside a lookahead — see
# tools/regex_oracle_cases.txt.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TOOLS="$ROOT/tools"
OUT="$ROOT/build/regex_oracle"
SRC="$TOOLS/cxx_regex_oracle.cpp"
PIN="$TOOLS/regex_oracle_known.txt"
CASES="$TOOLS/regex_oracle_cases.txt"

GNU=${GNU_CXX:-g++-16}
LLVM=${LLVM_CXX:-/opt/homebrew/opt/llvm/bin/clang++}

for c in "$GNU" "$LLVM"; do
    command -v "$c" >/dev/null 2>&1 || { echo "host oracle missing: $c" >&2; exit 2; }
done

mkdir -p "$OUT"
build() {
    _bin="$1"; shift
    [ -x "$OUT/$_bin" ] && [ "$OUT/$_bin" -nt "$SRC" ] && return 0
    "$@" -O2 -std=c++20 "$SRC" -o "$OUT/$_bin" || exit 2
}
build or_gnu  "$GNU"
build or_llvm "$LLVM" -stdlib=libc++

MODE=gen
case "${1:-}" in
    --cases) MODE=cases ;;
    --adversarial) MODE=adv ;;
    --repin) MODE=repin ;;
esac

ADV="$TOOLS/regex_oracle_adversarial.txt"
BUDGET=${BUDGET:-15}

if [ "$MODE" = adv ]; then
    [ -f "$ADV" ] || { echo "no $ADV" >&2; exit 2; }
    echo "--- blowup shapes, ${BUDGET}s budget per case. Correct answer is always no-match."
    while IFS="$(printf '\t')" read -r pat n; do
        case "$pat" in ''|\#*) continue ;; esac
        for bin in or_gnu or_llvm; do
            if ! timeout "$BUDGET" "$OUT/$bin" adv "$pat" "$n"; then
                printf '%-10s n=%-4s %-9s HUNG  >%ss (killed)\n' "$pat" "$n" "$bin" "$BUDGET"
            fi
        done
    done < "$ADV"
    exit 0
fi

if [ "$MODE" = cases ]; then
    [ -f "$CASES" ] || { echo "no $CASES" >&2; exit 2; }
    "$OUT/or_gnu"  file "$CASES" > "$OUT/c_gnu.txt"  2>/dev/null
    "$OUT/or_llvm" file "$CASES" > "$OUT/c_llvm.txt" 2>/dev/null
    echo "--- reduced cases: libstdc++ vs libc++"
    paste -d'\n' "$OUT/c_gnu.txt" "$OUT/c_llvm.txt" |
        awk 'NR%2{g=$0; next} {if (g==$0) next
             split(g,A," -> "); split($0,B," -> ")
             printf "  %s\n     libstdc++: %s\n     libc++   : %s\n", A[1], A[2], B[2]}'
    exit 0
fi

if [ "$MODE" = repin ]; then shift; fi
FROM=${1:-1}; TO=${2:-4000}; DEPTH=${3:-1}; FAN=${4:-2}

# libstdc++ is exponential on some of the shapes this generator produces, so a
# sweep with no clock on it is a sweep that can fail to end. Measured: 40000
# cases at depth 2 took 592 s, nearly all of it inside one library.
SWEEP_BUDGET=${SWEEP_BUDGET:-1200}
for pair in "or_gnu o_gnu" "or_llvm o_llvm"; do
    set -- $pair
    if ! timeout "$SWEEP_BUDGET" "$OUT/$1" gen "$FROM" "$TO" "$DEPTH" "$FAN" > "$OUT/$2.txt" 2>/dev/null; then
        echo "$1 did not finish within ${SWEEP_BUDGET}s — narrow the range or lower depth/fan." >&2
        exit 2
    fi
done

# One line per disagreeing case: id, then each side's verdict reduced to its
# class. Captures differ far more often than verdicts do, and a class histogram
# is what tells you whether a sweep found something new in KIND.
paste -d'\t' "$OUT/o_gnu.txt" "$OUT/o_llvm.txt" |
    awk -F'\t' '{
        split($1, A, " -> "); split($2, B, " -> ")
        if (A[2] == B[2]) next
        ga = A[2]; gb = B[2]
        sub(/at=.*/, "MATCH", ga); sub(/at=.*/, "MATCH", gb)
        split(A[1], I, " ")
        printf "%s\t%s -> %s\n", I[1], ga, gb
    }' | sort > "$OUT/today.txt"

TOTAL=$((TO - FROM))
DIS=$(wc -l < "$OUT/today.txt" | tr -d ' ')
echo "cases=$TOTAL  disagree=$DIS  (depth=$DEPTH fan=$FAN)"
echo "--- classes:"
cut -f2 "$OUT/today.txt" | sort | uniq -c | sort -rn | sed 's/^/  /'

if [ "$MODE" = repin ]; then
    { echo "# Disagreements between libstdc++ and libc++ on the generated sweep,"
      echo "# as measured. A cell listed here is one where NEITHER reference may"
      echo "# be quoted as ground truth for boxcxx: the standard has to settle it."
      echo "# range=[$FROM,$TO) depth=$DEPTH fan=$FAN"
      cat "$OUT/today.txt"
    } > "$PIN"
    echo "pinned $DIS disagreements -> $PIN"
    exit 0
fi

[ -f "$PIN" ] || { echo "no pin yet; run --repin"; exit 0; }
PINNED_ARGS=$(sed -n 's/^# range=//p' "$PIN")
THIS_ARGS="[$FROM,$TO) depth=$DEPTH fan=$FAN"
if [ "$PINNED_ARGS" != "$THIS_ARGS" ]; then
    echo "--- pin was taken over $PINNED_ARGS, this sweep is $THIS_ARGS: not comparable."
    echo "    (histogram above is the whole result; --repin only over the pinned range.)"
    exit 0
fi
grep -v '^#' "$PIN" | sort > "$OUT/pinned.txt"
NEW=$(comm -13 "$OUT/pinned.txt" "$OUT/today.txt")
GONE=$(comm -23 "$OUT/pinned.txt" "$OUT/today.txt")
RC=0
if [ -n "$NEW" ]; then
    echo "--- NEW disagreements (not in the pin):"; echo "$NEW" | sed 's/^/  /'; RC=1
fi
if [ -n "$GONE" ]; then
    echo "--- disagreements that VANISHED (pin is stale, or a library changed):"
    echo "$GONE" | sed 's/^/  /'; RC=1
fi
[ "$RC" = 0 ] && echo "--- pin matches: no new cell of doubt."
exit $RC
