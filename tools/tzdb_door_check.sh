#!/bin/sh
# tzdb_door_check.sh — run the `clock:tzdb` gate stand on the build host.
#
# The blob behind that door is the only untrusted input boxcxx has, and
# tzdata::Validate is what stands between it and a reader that follows offsets
# without looking. This compiles the REAL reader — <__bits/tzdb_read> speaks
# `long long` and `const char*` and no std type, so it builds here exactly as
# it builds for the guest — under AddressSanitizer and UndefinedBehaviorSanitizer,
# and attacks the gate with the real table corrupted 300 000 ways.
#
# It is a host check on purpose. The same evidence gathered by booting the
# emulator would cost minutes per attempt and would not notice a read one byte
# past the blob at all; here it costs about forty seconds and the sanitiser
# notices. It found three unbounded accumulations in the reader the first time
# it was run — none reachable from a table IANA ships, all reachable from a
# file anybody can write.
#
#   tools/tzdb_door_check.sh          # exit 0 iff the stand passes
#   CXX=g++-16 tools/tzdb_door_check.sh
#
# Re-run it after ANY edit to <__bits/tzdb_read>, and after regenerating the
# table with tools/gen_tzdb.py or tools/gen_tzdb_mini.py.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)

# A host compiler with C++23 and both sanitisers. The cross-compiler is not
# usable here: it targets the guest and has no sanitiser runtime.
if [ -n "${CXX:-}" ]; then
    CC_HOST=$CXX
else
    CC_HOST=""
    for candidate in g++-16 g++-15 g++-14 g++ clang++; do
        if command -v "$candidate" >/dev/null 2>&1; then CC_HOST=$candidate; break; fi
    done
fi
[ -z "$CC_HOST" ] && { echo "[tzdb-door] no host C++ compiler found"; exit 2; }

WORK=${TMPDIR:-/tmp}/tzdb_door.$$
OUT=$WORK/stand
trap 'rm -rf "$WORK"' EXIT INT TERM

# ‼ A shim of SYMLINKS to the three leaves this needs, not -I on the whole of
# include/std. Putting boxcxx's standard headers on a host include path shadows
# the host's own — its <cstdio> is written against boxlib and does not compile
# here — so the stand reaches the real files by name and the host library stays
# the host library. Symlinks, not copies: a stale copy would test a reader that
# no longer exists, which is the one failure mode a stand must not have.
mkdir -p "$WORK/__bits"
for leaf in civil_days tzdb_read tzdb_table; do
    ln -sf "$ROOT/src/userspace/boxcxx/include/std/__bits/$leaf" "$WORK/__bits/$leaf"
done

echo "[tzdb-door] compiling with $CC_HOST"
"$CC_HOST" -std=c++23 -g -O1 -fsanitize=address,undefined \
    -I "$WORK" \
    -I "$ROOT/src/userspace/apps" \
    "$ROOT/tools/tzdb_door_stand.cpp" -o "$OUT" || {
        echo "[tzdb-door] BUILD FAILED"
        exit 2
    }

"$OUT"
rc=$?
[ "$rc" = 0 ] && echo "[tzdb-door] OK" || echo "[tzdb-door] FAILED (rc=$rc)"
exit $rc
