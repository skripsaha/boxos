#!/bin/sh
# check_error_parity.sh — fail the build if the boxlib userspace error table
# drifts from the kernel error_t enum.
#
# Ф23 made boxlib box/error.h the single USERSPACE source of truth: one
# BOX_ERROR_LIST(X) row per code expands into the boxlib ERR_* constants, the
# C++ box::errc enumerators, and the box_error.cpp message/category tables — so
# those three can never disagree with each other. This guard extends the
# guarantee ACROSS THE KERNEL BOUNDARY (the one seam the X-macro cannot reach,
# since boxlib is userspace-only and does not include kernel headers): every
# kernel ERR_* must have a boxlib row with the identical value, and vice versa.
# Without it, a future kernel code silently re-opens the subset-drift Ф23 closed
# (that drift is exactly how box::error{941} once printed "box:941").
#
# Pure source diff — no build artifact needed. Wired into `make all`.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
KERN="$ROOT/src/kernel/core/error/error.h"
BOX="$ROOT/src/userspace/boxlib/include/box/error.h"

[ -r "$KERN" ] || { echo "[error-parity] cannot read $KERN"; exit 2; }
[ -r "$BOX" ]  || { echo "[error-parity] cannot read $BOX"; exit 2; }

tmpk=$(mktemp) || exit 2
tmpb=$(mktemp) || { rm -f "$tmpk"; exit 2; }
trap 'rm -f "$tmpk" "$tmpb"' EXIT

# Kernel: enum lines `ERR_NAME = 123,`. ERR_MAX is a sentinel that aliases the
# last real code (1100), not a distinct error — skip it.
awk -F'=' '/ERR_[A-Z0-9_]+[[:space:]]*=[[:space:]]*[0-9]+/ {
    name=$1; gsub(/[^A-Z0-9_]/,"",name);
    val=$2;  gsub(/[^0-9]/,"",val);
    if (name!="ERR_MAX") print name, val
}' "$KERN" | sort -u > "$tmpk"

# boxlib: BOX_ERROR_LIST rows `X(SUFFIX, name, value, ...)` -> ERR_SUFFIX value.
grep -oE 'X\([A-Z0-9_]+, *[a-z0-9_]+, *[0-9]+' "$BOX" \
    | sed -E 's/X\(([A-Z0-9_]+), *[a-z0-9_]+, *([0-9]+)/ERR_\1 \2/' \
    | sort -u > "$tmpb"

if cmp -s "$tmpk" "$tmpb"; then
    echo "[error-parity] OK — kernel and boxlib agree on all $(grep -c . "$tmpk") error codes"
    exit 0
fi

echo "[error-parity] FAIL — kernel error.h and boxlib BOX_ERROR_LIST disagree:"
echo "                (< kernel-only   > boxlib-only)"
diff "$tmpk" "$tmpb"
echo "[error-parity] fix: every kernel ERR_* needs a matching BOX_ERROR_LIST row"
echo "                (same name + value) in src/userspace/boxlib/include/box/error.h"
exit 1
