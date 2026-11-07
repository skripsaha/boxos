#!/bin/bash
# recipe stand — BoxLib's Recipe (src/userspace/boxlib/src/recipe*.c) and its
# SHA-256 (sha256.c), built for the host and held to exact outcomes:
#
#     tools/recipe_stand.sh
#
# SHA-256 to the vectors of FIPS 180-4 and, for every length up to 1100 bytes,
# to the host's own implementation (Python's hashlib); the Recipe's text
# written and read back byte for byte, every refusal to its error, the image
# to what the reader finds in it, and the check of its files to the state of
# each (tools/recipe_stand.c). Built under the address and undefined-behaviour
# sanitizers. The last line counts the cases that held.

set -u

TREE=$(cd "$(dirname "$0")/.." && pwd)
cd "$TREE" || exit 2
CC_HOST=${CC_HOST:-cc}

LIB=src/userspace/boxlib
WORK=$(mktemp -d "${TMPDIR:-/tmp}/recipe_stand.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT

FLAGS="-std=gnu11 -g -O1 -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-builtin -I$LIB/include -Isrc/include -Dfread=StandRead -DFileMetadataRead=StandMetadata -DFileMetadataRelease=StandMetadataRelease"

SOURCES="sha256 image_section recipe recipe_check"
objects=""
for name in $SOURCES; do
    $CC_HOST $FLAGS -c "$LIB/src/$name.c" -o "$WORK/$name.o" > "$WORK/cc.log" 2>&1 ||
        { cat "$WORK/cc.log"; echo "recipe: harness failed — $name.c did not build"; exit 2; }
    objects="$objects $WORK/$name.o"
done
$CC_HOST $FLAGS tools/recipe_stand.c $objects -o "$WORK/recipe_stand" > "$WORK/cc.log" 2>&1 ||
    { cat "$WORK/cc.log"; echo "recipe: harness failed — the stand did not build"; exit 2; }

"$WORK/recipe_stand" > "$WORK/out" 2>&1
stand=$?

# The stand's own count, and every `digest LEN HEX` line held to hashlib over
# the same pattern of bytes.
python3 -I - "$WORK/out" "$stand" <<'PY'
import hashlib, re, sys
lines = open(sys.argv[1], encoding="utf-8", errors="replace").read().splitlines()
stand = int(sys.argv[2])
pattern = bytes((i * 131 + 7) & 0xFF for i in range(1100))
passed = failed = digests = 0
for line in lines:
    m = re.fullmatch(r"digest (\d+) ([0-9a-f]{64})", line)
    if m:
        digests += 1
        n = int(m.group(1))
        want = hashlib.sha256(pattern[:n]).hexdigest()
        if m.group(2) == want:
            passed += 1
        else:
            failed += 1
            print(f"FAIL sha256 of {n} bytes\n  got:  {m.group(2)}\n  want: {want}")
        continue
    m = re.fullmatch(r"recipe: (\d+) passed, (\d+) failed", line)
    if m:
        passed += int(m.group(1))
        failed += int(m.group(2))
        continue
    print(line)
if digests != 1101:
    failed += 1
    print(f"FAIL the stand gave {digests} digests for the host to check, not 1101")
if stand not in (0, 1):
    print(f"recipe: harness failed — the stand ended with {stand}")
    sys.exit(2)
print(f"recipe: {passed} passed, {failed} failed")
sys.exit(0 if failed == 0 else 1)
PY
