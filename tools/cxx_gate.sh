#!/bin/sh
# cxx_gate.sh — the fast gate: rebuild, boot ONE config, run ONLY the named
# phases of cxxtest, report, and leave no QEMU behind.
#
# Why it exists (measured 2026-08-20, Ф43-d-1): the full gate is
#   tools/cxx_phase_matrix.sh cxxtest '\[CXX\] ALL PASS' bios1   ->  7 min 05 s
# of which the phases under test account for ~10 s of reported CPU. The rest is
# TCG emulating 239 phases that nobody touched — Unicode sweeps and
# correctly-rounded cmath among them. cxxtest now takes a phase selector on its
# launch args, so this runs the five phases a subphase actually changes.
#
# Usage:  tools/cxx_gate.sh [selector ...]
#   selector: phase number (9 -> 9a,9a2,9a3,9a4,9b,9c), name (9a4, current),
#             inclusive range (220-224), or "all". No selector = the whole suite.
# Env:
#   CFG=bios1|bios16|uefi1|uefi16   which config to boot   (default bios1)
#   NOBUILD=1                       skip make (tree already built)
#   BUDGET=<seconds>                post-shell ceiling, passed through to
#                                   cxx_phase_matrix.sh (default 3600). The
#                                   two slow configs no longer fit the default
#                                   with the whole suite -- raise it, or run
#                                   "0-80" and "81-250" as two gates.
#
# Examples:
#   tools/cxx_gate.sh 220-224              # the locale/facet phases, BIOS 1c
#   CFG=uefi16 tools/cxx_gate.sh 225       # one new phase on the slow config
#   NOBUILD=1 tools/cxx_gate.sh 9a4 current
#
# The full four-config matrix stays the thing that gates a COMMIT; this gates
# an edit. A subset run ends in "[CXX] SUBSET PASS", never "ALL PASS" — a
# partial run may not claim the word ALL.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT" || exit 2

CFG=${CFG:-bios1}
SEL=$*

if [ -n "$SEL" ]; then
    MARKER='\[CXX\] SUBSET PASS'
else
    MARKER='\[CXX\] ALL PASS'
fi

# A gate that races a QEMU left over from the previous run reads a stale log.
make run-stop >/dev/null 2>&1 || true
pkill -f 'qemu-system-x86_64.*boxos.img' >/dev/null 2>&1 || true

t0=$(date +%s)
if [ "${NOBUILD:-0}" != "1" ]; then
    if ! make >build/cxx_gate_build.log 2>&1; then
        echo "[gate] BUILD FAILED — build/cxx_gate_build.log:"
        grep -nE 'error:|Error|undefined reference' build/cxx_gate_build.log | head -30
        exit 2
    fi
fi
t1=$(date +%s)

tools/cxx_phase_matrix.sh "cxxtest $SEL" "$MARKER" "$CFG"
rc=$?
t2=$(date +%s)

passed=$(grep -c '^\[CXX\] PASS phase' build/serial.log 2>/dev/null)
echo "[gate] cfg=$CFG  selector='${SEL:-<all>}'  phases-passed=$passed  build=$((t1-t0))s  run=$((t2-t1))s  total=$((t2-t0))s"
[ "$rc" != 0 ] && grep -nE '\[CXX\] FAIL|\[CXX\] TOTAL FAILURES|PANIC|\[EXCEPTION\]|\[boxcxx\] FATAL' build/serial.log 2>/dev/null | tail -25
exit $rc
