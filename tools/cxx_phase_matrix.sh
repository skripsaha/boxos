#!/bin/sh
# cxx_phase_matrix.sh — focused per-phase matrix runner for the boxcxx
# "Ф22–Ф31 Completion & Hardening" epic.
#
# Boots ONLY the four owner-mandated configs (UEFI/BIOS × STRICT × {1c,16c})
# and runs ONLY the test app(s) relevant to the phase under test — never the
# unrelated heavy suites (write_stress / cow_test / touch_stress), which are
# slow and carry the pre-existing AMP-storage UEFI-16c flake. This keeps the
# signal about the phase clean and the run fast ("только нужные тесты").
#
# Usage:  tools/cxx_phase_matrix.sh "<app [app2 ...]>" "<PASS-marker-regex>" [configs]
#   configs (optional): subset of  bios1 bios16 uefi1 uefi16  (default: all four)
# Examples:
#   tools/cxx_phase_matrix.sh cxxtest '\[CXX\] ALL PASS'            # full matrix
#   tools/cxx_phase_matrix.sh cxxtest '\[CXX\] ALL PASS' bios1      # quick smoke
#
# Per-config PASS = marker appears AND no PANIC / [EXCEPTION] / TOTAL FAILURES.
# Exit 0 iff every requested config passes.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT" || exit 2

APPS=$1
MARKER=$2
CONFIGS=${3:-"bios1 bios16 uefi1 uefi16"}

cfg_env() {
    case "$1" in
        bios1)  echo "STRICT=on CORES=1  MEM=2G" ;;
        bios16) echo "STRICT=on CORES=16 MEM=8G" ;;
        uefi1)  echo "UEFI=on STRICT=on CORES=1  MEM=2G" ;;
        uefi16) echo "UEFI=on STRICT=on CORES=16 MEM=8G" ;;
        *)      echo "" ;;
    esac
}

FAILED=0
for cfg in $CONFIGS; do
    extra=$(cfg_env "$cfg")
    [ -z "$extra" ] && { echo "[$cfg] unknown config name"; FAILED=1; continue; }
    echo "================ CONFIG: $cfg  ($extra) ================"
    make run-stop >/dev/null 2>&1 || true
    : > build/serial.log 2>/dev/null || true
    eval "$extra make run-bg" >/dev/null 2>&1

    up=0
    for i in $(seq 1 160); do
        grep -q "BoxOS Shell" build/serial.log 2>/dev/null && { up=1; break; }
        sleep 0.5
    done
    if [ "$up" != 1 ]; then
        echo "[$cfg] FAIL: shell prompt never appeared"
        make run-stop >/dev/null 2>&1 || true; FAILED=1; continue
    fi
    sleep 2

    for c in $APPS; do
        tools/qemu-input.sh type "$c" >/dev/null 2>&1
        tools/qemu-input.sh key  ret >/dev/null 2>&1
    done

    hit=0
    # 450s post-shell budget: the correctly-rounded cmath suite (Phase66+) runs
    # tens of thousands of software-dd evals per function; on 16c-TCG the full
    # run legitimately takes minutes and grows with each CR phase. Still breaks
    # out immediately on PANIC/EXCEPTION/FAILURES, so genuine faults fail fast.
    for i in $(seq 1 900); do
        [ "$(grep -cE "$MARKER" build/serial.log 2>/dev/null)" -ge 1 ] && { hit=1; break; }
        grep -qE 'PANIC|\[EXCEPTION\]|\[CXX\] TOTAL FAILURES' build/serial.log 2>/dev/null && break
        sleep 0.5
    done
    bad=$(grep -cE 'PANIC|\[EXCEPTION\]|\[CXX\] TOTAL FAILURES' build/serial.log 2>/dev/null)

    if [ "$hit" = 1 ] && [ "$bad" = 0 ]; then
        echo "[$cfg] PASS (marker matched; no PANIC/EXCEPTION/FAILURES)"
    else
        echo "[$cfg] FAIL (marker_hit=$hit  bad_lines=$bad)"
        grep -nE 'PANIC|\[EXCEPTION\]|\[CXX\] (TOTAL FAILURES|FAIL)|phase9a4' build/serial.log 2>/dev/null | tail -20
        FAILED=1
    fi
    make run-stop >/dev/null 2>&1 || true
done

echo "================ MATRIX RESULT: $([ $FAILED = 0 ] && echo ALL-PASS || echo FAILED) ================"
exit $FAILED
