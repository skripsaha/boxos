#!/bin/bash
# format stand — BoxLib's printf conversions (src/userspace/boxlib/src/format.c),
# built for the host and held to independent witnesses:
#
#     tools/format_stand.sh
#
# the host's C library on random conversions of every kind, with random
# flags, widths and precisions (tools/format_stand.c, which says where the
# host itself departs from C and what stands in for it there), and exact
# arithmetic on x87 long doubles the host has no type for
# (tools/format_oracle.py). The last line counts the witnesses that agreed.

set -u

TREE=$(cd "$(dirname "$0")/.." && pwd)
cd "$TREE" || exit 2
CC_HOST=${CC_HOST:-cc}

WORK=$(mktemp -d "${TMPDIR:-/tmp}/format_stand.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT

$CC_HOST -std=c11 -O1 -Wall -Wextra -Werror -c -Isrc/userspace/boxlib/include -Isrc/include \
    tools/format_bridge.c -o "$WORK/bridge.o" > "$WORK/cc.log" 2>&1 &&
    $CC_HOST -std=c11 -O1 -Wall -Wextra -Werror -Wno-format-nonliteral tools/format_stand.c \
        "$WORK/bridge.o" -o "$WORK/format_stand" -lm >> "$WORK/cc.log" 2>&1 ||
    { cat "$WORK/cc.log"; echo "format: harness failed — the stand did not build"; exit 2; }
python3 tools/format_oracle.py extended 3000 > "$WORK/extended.txt" ||
    { echo "format: harness failed — the oracle wrote no vectors"; exit 2; }

passed=0
failed=0
if "$WORK/format_stand" random 300000; then passed=$((passed + 1)); else failed=$((failed + 1)); fi
if "$WORK/format_stand" extended "$WORK/extended.txt"; then passed=$((passed + 1)); else failed=$((failed + 1)); fi
echo "format: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
