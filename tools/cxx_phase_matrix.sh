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
#   BUDGET=7200 tools/cxx_phase_matrix.sh cxxtest '\[CXX\] ALL PASS' uefi16   # two hours for the slow one
#
# Env:
#   BUDGET=<seconds>   per-config post-shell ceiling (default 3600). It is read
#                      from the clock, so it means what it says whatever a poll
#                      costs -- see the note above the wait loop.
#
# Per-config PASS = marker appears AND no PANIC / [EXCEPTION] / TOTAL FAILURES.
# Exit 0 iff every requested config passes.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT" || exit 2

APPS=$1
MARKER=$2
CONFIGS=${3:-"bios1 bios16 uefi1 uefi16"}
BUDGET=${BUDGET:-3600}

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
    # Post-shell budget (BUDGET, default 3600s): the correctly-rounded cmath suite (Phase66+) runs
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

    # ‼ The ceiling is WALL CLOCK, and it was not. This loop used to be
    # `for i in $(seq 1 7200)` with a 0.5 s sleep, described three paragraphs up
    # as a 3600 s budget -- which it is only while a poll is free. A poll is
    # three greps over build/serial.log, and its cost is set by the SIZE of that
    # log: measured, 8.4 ms on the 72 KB a healthy run produces (1.7% of the
    # hour, i.e. nothing), and 2.42 s on a 72 MB one. The run that recorded
    # "227 of 245 phases, 12594 s, marker_hit=0 bad_lines=0" is that arithmetic
    # and not a mystery: 12594 / 7200 = 1.749 s per iteration, so the guest was
    # spewing into the log, each poll had grown to a second and a quarter, and
    # the ceiling this runner advertises as one hour silently became three and a
    # half -- with a stuck config reading exactly like a slow one for all of it.
    # A deadline taken from the clock cannot drift that way whatever a poll
    # costs, and the poll stays a plain full-log grep, because on a healthy log
    # that is 1.7% and buying it back is not worth an offset to keep straight.
    t_start=$(date +%s)
    deadline=$((t_start + BUDGET))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        [ "$(grep -cE "$MARKER" build/serial.log 2>/dev/null)" -ge 1 ] && { hit=1; break; }
        [ "$(count_bad)" -gt 0 ] && break
        sleep 0.5
    done
    elapsed=$(($(date +%s) - t_start))
    bad=$(count_bad)

    # ‼ Keep this config's log before the next one overwrites it.
    # Every config writes build/serial.log, so a failure used to destroy its own
    # evidence the moment the run moved on: by the time anyone read "[bios1]
    # FAIL", build/serial.log held bios16's output and the lines that explained
    # the failure were gone. Measured 2026-08-22, chasing exactly that.
    cp build/serial.log "build/serial-$cfg.log" 2>/dev/null || true

    if [ "$hit" = 1 ] && [ "$bad" = 0 ]; then
        echo "[$cfg] PASS in ${elapsed}s (marker matched; no PANIC/EXCEPTION/FATAL/FAILURES)"
    else
        if [ "$hit" != 1 ] && [ "$bad" = 0 ]; then
            # ‼ Its own outcome, not a shrug. "marker_hit=0 bad_lines=0" is
            # what this runner printed for BOTH "nothing was ever going to
            # finish in the time given" and "something died in a way the
            # pattern above does not name", and the comment over count_bad
            # records what reading those as the same thing cost. The clock
            # tells them apart, so it says which one this is.
            echo "[$cfg] FAIL: budget exhausted (${elapsed}s of ${BUDGET}s; no marker, nothing died)  log: build/serial-$cfg.log"
            echo "[$cfg]   raise it with BUDGET=<seconds>, or split the suite across boots (\"0-80\" then \"81-250\")"
        else
            echo "[$cfg] FAIL (marker_hit=$hit  bad_lines=$bad  after ${elapsed}s)  log: build/serial-$cfg.log"
        fi
        # ‼ `phase9a4` is a LANDMARK in this pattern, not a symptom. It prints
        # on every failure because the suite always reaches it, and reading its
        # presence as "the run stopped there" has sent at least one
        # investigation down the wrong road.
        grep -nE 'PANIC|\[EXCEPTION\]|\[boxcxx\] FATAL|\[CXX\] (TOTAL FAILURES|FAIL)|phase9a4' build/serial.log 2>/dev/null | tail -20
        echo "[$cfg] last line seen: $(tail -1 build/serial.log 2>/dev/null | cut -c1-100)"
        FAILED=1
    fi
    make run-stop >/dev/null 2>&1 || true
done

echo "================ MATRIX RESULT: $([ $FAILED = 0 ] && echo ALL-PASS || echo FAILED) ================"
exit $FAILED
