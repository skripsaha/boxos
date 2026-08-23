#!/bin/sh
# cxx_valarray_oracle.sh — run the differential <valarray> stand.
#
# Two reference implementations and, once the leaf exists, boxcxx's own —
# one case sequence, one diff. Unlike the regex sweep, the two references agree
# here on every case, so there is no pin of cells where neither may be quoted:
# the pin IS their agreed output, and a row where our column differs from it is
# a defect until argued otherwise from the standard.
#
# Usage:
#   tools/cxx_valarray_oracle.sh              semantic sweep, all built columns
#   tools/cxx_valarray_oracle.sh --big        the long-array battery (the only
#                                             mode in which the brigade splits)
#   tools/cxx_valarray_oracle.sh --san        our column under ASan+UBSan
#   tools/cxx_valarray_oracle.sh --tsan       our column under ThreadSanitizer,
#                                             on the long arrays, where two
#                                             chunks really do run at once
#   tools/cxx_valarray_oracle.sh --repin      refresh the agreed pin
#
# Ф44-f built it before the first line of the header, and its first run found
# that `v[mask] += scalar` compiles in NEITHER reference -- see the comment at
# that case in tools/cxx_valarray_oracle.cpp.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TOOLS="$ROOT/tools"
OUT="$ROOT/build/valarray_oracle"
SRC="$TOOLS/cxx_valarray_oracle.cpp"
HOSTPAR="$TOOLS/cxx_par_host.cpp"
LEAVES="$ROOT/src/userspace/boxcxx/include/std/__bits"
PIN="$TOOLS/valarray_oracle_agreed.txt"
PIN_BIG="$TOOLS/valarray_oracle_agreed_big.txt"

GNU=${GNU_CXX:-g++-16}
LLVM=${LLVM_CXX:-/opt/homebrew/opt/llvm/bin/clang++}
for c in "$GNU" "$LLVM"; do
    command -v "$c" >/dev/null 2>&1 || { echo "host oracle missing: $c" >&2; exit 2; }
done

mkdir -p "$OUT"
MODE=${1:-sweep}

build_ref() {
    _bin=$1; shift
    [ -x "$OUT/$_bin" ] && [ "$OUT/$_bin" -nt "$SRC" ] && return 0
    "$@" -O2 -std=c++20 -Wall -Wextra "$SRC" -o "$OUT/$_bin" || exit 2
}

# The third column: our leaf, built by the host compiler, reached through
# symlinks so that boxcxx's <cmath> and <cstddef> cannot shadow the host's --
# only the files named here are visible to it.
build_ours() {
    [ -f "$LEAVES/valarray_core" ] || return 1
    mkdir -p "$OUT/inc/__bits"
    for leaf in valarray_core par_engine; do
        ln -sf "$LEAVES/$leaf" "$OUT/inc/__bits/$leaf"
    done
    _newest=$(ls -t "$LEAVES"/valarray_core "$LEAVES"/par_engine "$SRC" "$HOSTPAR" 2>/dev/null | head -1)
    [ -x "$OUT/$1" ] && [ "$OUT/$1" -nt "$_newest" ] && return 0
    _bin=$1; shift
    "$GNU" -std=c++20 -Wall -Wextra -DBOXCXX_COLUMN -I"$OUT/inc" \
           "$@" "$SRC" "$HOSTPAR" -o "$OUT/$_bin" || return 2
    return 0
}

run_and_report() {
    _arg=$1 _pin=$2
    build_ref va_gnu  "$GNU"
    build_ref va_llvm "$LLVM" -stdlib=libc++
    "$OUT/va_gnu"  $_arg > "$OUT/gnu.txt"  || { echo "gnu column died"; exit 1; }
    "$OUT/va_llvm" $_arg > "$OUT/llvm.txt" || { echo "llvm column died"; exit 1; }

    if ! diff -q "$OUT/gnu.txt" "$OUT/llvm.txt" >/dev/null; then
        echo "‼ THE REFERENCES DISAGREE -- neither may be quoted on these rows:"
        diff "$OUT/gnu.txt" "$OUT/llvm.txt"
        echo
    fi
    if [ -f "$_pin" ] && ! diff -q "$OUT/gnu.txt" "$_pin" >/dev/null; then
        echo "‼ the references moved since the pin was taken:"
        diff "$_pin" "$OUT/gnu.txt"
        echo
    fi

    if build_ours va_box -O2; then
        "$OUT/va_box" $_arg > "$OUT/box.txt" || { echo "our column died"; exit 1; }
        if diff -q "$OUT/box.txt" "$OUT/gnu.txt" >/dev/null; then
            echo "ALL AGREE — $(wc -l < "$OUT/gnu.txt" | tr -d ' ') cases, three columns"
        else
            echo "boxcxx stands alone on:"
            diff "$OUT/gnu.txt" "$OUT/box.txt" | sed 's/^</  refs :/; s/^>/  ours :/'
            exit 1
        fi
    else
        echo "two columns only — $(wc -l < "$OUT/gnu.txt" | tr -d ' ') cases agree; \
__bits/valarray_core is not in the tree yet"
    fi
}

case "$MODE" in
  --big)  run_and_report --big "$PIN_BIG" ;;
  --repin)
        build_ref va_gnu "$GNU"
        "$OUT/va_gnu"       > "$PIN"
        "$OUT/va_gnu" --big > "$PIN_BIG"
        echo "pinned $(wc -l < "$PIN" | tr -d ' ') + $(wc -l < "$PIN_BIG" | tr -d ' ') cases"
        ;;
  --san)
        build_ours va_box_san -O1 -g -fsanitize=address,undefined \
            -fno-omit-frame-pointer || { echo "our column is not in the tree yet"; exit 2; }
        "$OUT/va_box_san" && "$OUT/va_box_san" --big && echo "ASan+UBSan: clean"
        ;;
  --tsan)
        build_ours va_box_tsan -O1 -g -fsanitize=thread \
            || { echo "our column is not in the tree yet"; exit 2; }
        "$OUT/va_box_tsan" --big && echo "TSan: clean on the long arrays"
        ;;
  *)      run_and_report "" "$PIN" ;;
esac
