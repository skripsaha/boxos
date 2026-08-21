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
# Usage:  tools/cxx_phase_matrix.sh "<cmdline[;cmdline2...]>" "<PASS-marker-regex>" [configs]
#   configs (optional): subset of  bios1 bios16 uefi1 uefi16  (default: all four)
# The first argument is a SEMICOLON-separated list of shell command lines, so a
# command may carry arguments of its own -- "cxxtest 220-224" is one command,
# not two. (It used to split on whitespace, which made an argument impossible.)
# Examples:
#   tools/cxx_phase_matrix.sh cxxtest '\[CXX\] ALL PASS'                    # full matrix
#   tools/cxx_phase_matrix.sh cxxtest '\[CXX\] ALL PASS' bios1              # quick smoke
#   tools/cxx_phase_matrix.sh "cxxtest 220-224" '\[CXX\] SUBSET PASS'       # four configs, five phases
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
    # `make run-stop` sends SIGTERM, waits 0.3 s, then SIGKILL — and returns
    # without waiting for the process to actually be gone. Starting the next
    # config on top of a dying QEMU means two processes holding the same
    # serial.log at two different offsets, which is exactly what a line cut off
    # mid-printf looks like. Measured: the uefi1 config, third in the sequence,
    # stalled twice this way while the SAME tree passed a standalone run four
    # times. So wait for it, and say so if it will not go.
    make run-stop >/dev/null 2>&1 || true
    gone=0
    for i in $(seq 1 60); do
        pgrep -f "qemu-system-x86_64 -drive .*boxos.img" >/dev/null 2>&1 || { gone=1; break; }
        sleep 0.25
    done
    if [ "$gone" != 1 ]; then
        echo "[$cfg] previous QEMU would not die — refusing to boot on top of it"
        FAILED=1
        continue
    fi
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

    (
        IFS=';'
        for c in $APPS; do
            [ -z "$c" ] && continue
            tools/qemu-input.sh type "$c" >/dev/null 2>&1
            tools/qemu-input.sh key  ret >/dev/null 2>&1
        done
    )

    hit=0
    # 3600s post-shell budget: the correctly-rounded cmath suite (Phase66+) runs
    # tens of thousands of software-dd evals per function, and each ranges phase
    # (Phase87..100: views/algos/ranges::to/container range-members) plus the
    # newer value-type phases (Phase114 <any>, Phase115-118 iostreams, Phase119
    # <complex> Annex-G special-value grids) add more; on 16c-TCG the full run
    # legitimately takes many minutes and grows every phase. The slowest config
    # is UEFI+16c-TCG (UEFI firmware overhead × 16-vCPU TCG serialization ×
    # the pre-existing AMP-slowness [boxos-smp-scaling-epic]); as the suite grew
    # past Phase119 it stopped finishing within the old 1200s ceiling mid-suite
    # (marker=0 bad=0, a runner-budget timeout, not a code flake). Still breaks
    # out immediately on marker-hit OR PANIC/EXCEPTION/FAILURES, so fast configs
    # and genuine faults both finish/fail fast; only a slow-but-healthy config
    # uses the full ceiling.
    # '[boxcxx] FATAL' is in the pattern because it was NOT, and that cost a
    # whole diagnosis: an uncaught exception ends the app through
    # std::terminate, which prints "[boxcxx] FATAL: std::terminate() called" and
    # nothing else — no PANIC, no [EXCEPTION], no TOTAL FAILURES. The runner
    # therefore reported marker_hit=0 bad_lines=0, which is EXACTLY the
    # signature of hitting the budget below, and a real crash read as a slow
    # config. Any death the guest can print has to be in this alternation, or
    # "budget" becomes a place for failures to hide.
    # One death IS a test: Ф41's phase203 spawns a child that must die of an
    # uncaught exception, because nothing inside a living process can witness
    # whether teardown ran on the way out. That child prints a fence line
    # BEFORE it throws, and exactly one announced FATAL is subtracted per
    # fence. The criterion stays strict in both directions — an unannounced
    # death still fails, and a SECOND death after one fence still fails,
    # because the subtraction is capped by the number of fences.
    count_bad() {
        total=$(grep -cE 'PANIC|\[EXCEPTION\]|\[boxcxx\] FATAL|\[CXX\] TOTAL FAILURES' build/serial.log 2>/dev/null)
        announced=$(grep -c 'expected-fatal' build/serial.log 2>/dev/null)
        [ "$announced" -gt "$total" ] && announced=$total
        echo $((total - announced))
    }

    for i in $(seq 1 7200); do
        [ "$(grep -cE "$MARKER" build/serial.log 2>/dev/null)" -ge 1 ] && { hit=1; break; }
        [ "$(count_bad)" -gt 0 ] && break
        sleep 0.5
    done
    bad=$(count_bad)

    if [ "$hit" = 1 ] && [ "$bad" = 0 ]; then
        echo "[$cfg] PASS (marker matched; no PANIC/EXCEPTION/FATAL/FAILURES)"
    else
        echo "[$cfg] FAIL (marker_hit=$hit  bad_lines=$bad)"
        grep -nE 'PANIC|\[EXCEPTION\]|\[boxcxx\] FATAL|\[CXX\] (TOTAL FAILURES|FAIL)|phase9a4' build/serial.log 2>/dev/null | tail -20
        FAILED=1
    fi
    make run-stop >/dev/null 2>&1 || true
done

echo "================ MATRIX RESULT: $([ $FAILED = 0 ] && echo ALL-PASS || echo FAILED) ================"
exit $FAILED
