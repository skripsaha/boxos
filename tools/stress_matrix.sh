#!/bin/sh
# stress_matrix.sh — multi-config stress test for BoxOS stability.
#
# For each (FW × CORES × STRICT) tuple, boot the OS, run a mixed command
# burst (baseline shell utilities + every safe production-stable test
# app), then enforce zero-PANIC and a specific success marker per test.
#
# Tests in the burst (all required to print their PASS marker):
#   Baseline (run multiple times — historical regression guards):
#     - memtest (×3)  "All 15 tests passed"
#     - files   (×2)  "^Files:"
#     - bench   (×2)  "create+write64+delete (TagFS+disk)"
#   Additional production-stability suite (each ×1):
#     - mtest             "[mtest] PASS"             — Manifest fill+move
#     - chain             "[chain] PASS"             — multi-op Manifest
#     - cow_test          "[COW] PASS"               — CoW snapshot lifecycle
#     - lifecycle         "[LIFECYCLE] PASS"         — process:spawned/died Touches
#     - write_observer    "[WO] PASS"                — Touch WROTE delivery
#     - write_concurrent  "[WC] PASS"                — 4 children × cross-write
#     - write_stress      "[WS SUMMARY] all 3 PASS"  — disk-write stress
#     - touch_test        "[TT SUMMARY]"             — touch integration suite
#     - decks             ", 0 failed"               — Decks stress / OpRegistry
#     - touch_stress      "[STRESS S1]" + S2 + S3    — heavy tag churn (×3 markers)
#
# Global negative criteria (any one means FAIL):
#   - PANIC / [EXCEPTION] lines on serial
#   - "ATRC" canary (character-loss corruption)
#   - "Unknown command" (first-cmd-no-op race)
#   - "[*] FAIL" patterns from any test app
#   - TSC freq outside the 1 GHz band (lines matching "TSC freq: 100")
#   - a Nightwatch VERDICT — the kernel proving the machine stopped
#
# Exit non-zero if any config fails.

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2

FAILED=0

# MODE=fast (env or first arg) — quick smoke matrix: 4 representative
# configs × 9 essential tests, ~2 min total. Drops the historically
# flaky touch_stress + the slow write_stress + redundant repetitions.
# Use for quick regression checks; full matrix for releases.
MODE=${MODE:-${1:-full}}

# Per-command budget, in SECONDS OF WALL CLOCK. It used to be a count of
# iterations of {grep serial.log; sleep 0.5}, which is not a budget at all:
# each grep walks the whole serial.log, so the effective deadline grew as the
# log grew and shrank again on an idle host. cxxtest legitimately takes ~180 s
# at -O0 -- the correctly-rounded cmath phases stream 20 000 MPFR-verified
# values each -- and whether it fit inside a nominal "60 s" was decided by how
# slow grep happened to be on that run. That is what made cxxtest and, right
# behind it, strandtest fail intermittently in configs that had passed minutes
# earlier with the same binary. Measured, not guessed: cxxtest was timed at
# 180.6 s with Ф35's five new phases and 182.4 s without them, so the suite's
# size was never the variable.
#
# The loop still exits the instant the marker appears, so a fast test costs
# nothing and only a genuinely stuck one pays the ceiling.
# 300 s was too small, and the way it failed is worth recording: on UEFI 16c
# cxxtest overran it, the burst typed the next command into a shell still
# parked on cxxtest, and the config reported zeros for every remaining test.
# The suite was fine; the budget was not. 900 s was set above the slowest
# cxxtest observed then (450 s on that same configuration).
#
# Re-measured 2026-09-02, after the console became a guaranteed-delivery
# stream: a print now lands on screen before the printer moves on, so a
# suite's wall time includes its own rendering — and on 16 vCPUs emulated
# by ONE TCG host thread that is the dominant term. Full cxxtest on UEFI
# STRICT 16c: 1300 s, ALL PASS, zero exceptions. The ceiling is ~2x the
# slowest observed run — a real ceiling, not "wait forever", and the loop
# still exits the instant the marker appears.
POLL_SECONDS=2700

run_config() {
    cfg_name=$1
    extra=$2
    echo "================================================================"
    echo "  CONFIG: ${cfg_name}"
    echo "  ${extra}"
    echo "================================================================"

    make run-stop >/dev/null 2>&1 || true

    eval "${extra} make run-bg" >/dev/null 2>&1
    # Wait for shell prompt (UEFI GOP is slow). Poll instead of fixed sleep.
    for i in $(seq 1 120); do
        grep -q "BoxOS Shell" build/serial.log 2>/dev/null && break
        sleep 0.5
    done
    sleep 2

    # Per-test counters (cumulative pattern matches over serial.log).
    mt_seen=0; fl_seen=0; bn_seen=0
    TIMED_OUT_CMD=""
    TYPING_FAILED_CMD=""
    # Single-run tests reuse `want=1` against the first appearance of
    # their pattern. Fast mode drops historical-regression duplicates
    # and the long-tail tests (write_stress ~30 s, touch_stress ~30 s
    # plus historical flakiness — see memory:touch_stress_complete*).
    if [ "$MODE" = "fast" ]; then
        burst="memtest files bench mtest chain cow_test lifecycle usetest luggagetest write_observer decks htest cxxtest current_test strandtest"
    else
        burst="memtest files bench memtest files bench memtest \
               mtest chain htest cxxtest current_test strandtest cow_test lifecycle usetest luggagetest write_observer \
               write_concurrent write_stress touch_test decks touch_stress"
    fi
    for c in $burst
    do
        # qemu-input's `type` verifies its own work: it reads the guest's echo
        # back and retypes the whole line up to three times, then dies saying
        # so. That verdict used to be thrown away — stderr to /dev/null and the
        # exit status unread — after which this loop pressed Enter on a line
        # the guest had only half taken. The shell answered "Unknown command:
        # touch_ss", the test never ran, and the config then spent its entire
        # 2700 s budget waiting for a marker that could not appear. A harness
        # that cannot type must say THAT, not let the OS be blamed for it.
        if ! tools/qemu-input.sh type "$c" 2>build/.typeerr; then
            type_rc=$?
            echo "  KEYSTROKES DROPPED while typing '$c' (exit $type_rc):"
            # Print the code even when the message is empty. An empty complaint
            # once cost a whole configuration and an hour of reading: the typer
            # had died under `set -e` with nothing to say, and "keystrokes
            # dropped" was the harness blaming the OS for its own exit.
            if [ -s build/.typeerr ]; then
                sed 's/^/    /' build/.typeerr 2>/dev/null
            else
                echo "    (the typer exited without a message — a harness fault,"
                echo "     not a guest one; see tools/qemu-input.sh)"
            fi
            echo "  (burst stopped here — pressing Enter on a half-typed line"
            echo "   would run a DIFFERENT command and blame the OS for it)"
            TYPING_FAILED_CMD=$c
            rm -f build/.typeerr
            break
        fi
        rm -f build/.typeerr
        tools/qemu-input.sh key ret  >/dev/null 2>&1

        case "$c" in
            memtest)
                mt_seen=$((mt_seen+1))
                pat="All 15 tests passed"; want=$mt_seen ;;
            files)
                fl_seen=$((fl_seen+1))
                pat="^Files:"; want=$fl_seen ;;
            bench)
                bn_seen=$((bn_seen+1))
                # Escaped for grep -E: the '+' and the parens in bench's banner
                # are ERE metacharacters, so the unescaped form asked for
                # "TagFSdisk" and never matched ANYTHING. The poll therefore ran
                # to its deadline on every bench of every config, and nobody
                # noticed because a timeout used to be a no-op and the summary
                # line below counts with plain grep, where '+' is a literal.
                pat="create\+write64\+delete \(TagFS\+disk\)"; want=$bn_seen ;;
            mtest)            pat="\[mtest\] PASS";           want=1 ;;
            chain)            pat="\[chain\] PASS";           want=1 ;;
            htest)            pat="\[htest\] PASS";           want=1 ;;
            cxxtest)          pat="\[CXX\] ALL PASS";         want=1 ;;
            current_test)     pat="\[CURRENT\] ALL PASS";     want=1 ;;
            strandtest)       pat="\[STRAND\] PASS";          want=1 ;;
            cow_test)         pat="\[COW\] PASS";             want=1 ;;
            lifecycle)        pat="\[LIFECYCLE\] PASS";       want=1 ;;
            usetest)          pat="\[USE\] PASS";             want=1 ;;
            luggagetest)      pat="\[LUGGAGE\] PASS";         want=1 ;;
            write_observer)   pat="\[WO\] PASS";              want=1 ;;
            write_concurrent) pat="\[WC\] PASS";              want=1 ;;
            write_stress)     pat="\[WS SUMMARY\] all 3 PASS";want=1 ;;
            touch_test)       pat="\[TT SUMMARY\]";           want=1 ;;
            # NOT ", 0 failed" — the kernel prints that itself, twice, on
            # every boot ("[TME TEST] 8 passed, 0 failed" and "[TESTS] TagFS:
            # ... 0 failed ..."). The counter below therefore started at 2
            # before decks had run at all, so the gate `decks < 1` could never
            # go red and this test was effectively ungated: a config where
            # decks never ran still came out green. Match the app's own line.
            decks)            pat="\\[decks\\] [0-9]+ passed, 0 failed"; want=1 ;;
            touch_stress)
                # Wait for the FINAL "[STRESS] Done" marker —
                # touch_stress prints it after S1+S2+S3 regardless of
                # individual PASS/FAIL, so the poll exits as soon as
                # the whole suite finishes; per-substage PASS is checked
                # at grep-summary time.
                pat="\[STRESS\] Done";                        want=1 ;;
        esac

        # A command that never printed its marker leaves the shell PARKED on
        # its child. Typing the next command into a parked shell is not a
        # neutral act: the keystrokes land with no prompt to receive them, the
        # rest of the burst never runs, and the config reports zeros for every
        # later test — which reads as "twelve tests failed" when one timed out.
        # So a timeout ENDS this config's burst and says so by name.
        timed_out=""
        # A suite that has ALREADY announced failure is finished, and waiting
        # out the budget for a marker it will never print is 45 minutes spent
        # learning nothing. cxxtest prints "TOTAL FAILURES: n" and exits; the
        # old loop then sat until 2700 s and reported a TIMEOUT, which reads
        # as "the machine wedged" when the truth was "one phase failed and the
        # suite ended normally". Watch for the terminal failure line too, and
        # stop the moment either verdict lands.
        case "$c" in
            cxxtest) fail_pat="\[CXX\] TOTAL FAILURES: [1-9]" ;;
            *)       fail_pat="" ;;
        esac
        deadline=$(( $(date +%s) + POLL_SECONDS ))
        while :; do
            [ "$(grep -cE "$pat" build/serial.log)" -ge "$want" ] && break
            if [ -n "$fail_pat" ] && grep -qE "$fail_pat" build/serial.log; then
                echo "  '$c' ANNOUNCED FAILURE and ended — not a timeout:"
                grep -E "$fail_pat" build/serial.log | sed 's/^/    /' | tail -1
                break
            fi
            [ "$(date +%s)" -ge "$deadline" ] && { timed_out=1; break; }
            sleep 0.5
        done
        if [ -n "$timed_out" ]; then
            echo "  TIMEOUT: '$c' never printed its marker within ${POLL_SECONDS}s"
            echo "  (burst stopped here — the shell is parked on it; typing"
            echo "   past a parked shell only manufactures further failures)"
            TIMED_OUT_CMD=$c
            break
        fi
    done
    sleep 2

    make run-stop >/dev/null 2>&1 || true
    sleep 1

    # Archive per-config log so failures can be diagnosed after the
    # whole matrix completes. Filename sanitised for filesystem safety.
    mkdir -p build/matrix_logs
    safe_name=$(echo "${cfg_name}" | tr ' /' '__')
    # Per-run timestamped archive so duplicate-named configs do NOT overwrite
    # each other (matrix runs BIOS 1c / BIOS 4c / UEFI 1c / UEFI 4c twice).
    # Without the timestamp, a flaky first run is hidden by the duplicate's
    # successful overwrite, and post-mortem cannot locate the failure.
    ts=$(date +%H%M%S)
    cp build/serial.log "build/matrix_logs/${safe_name}_${ts}.log" 2>/dev/null || true
    # Also keep the legacy un-timestamped filename so existing tooling that
    # greps build/matrix_logs/<config>.log still finds the *latest* run.
    cp build/serial.log "build/matrix_logs/${safe_name}.log" 2>/dev/null || true

    if [ ! -f build/serial.log ]; then
        echo "  FAIL — serial.log missing"
        FAILED=$((FAILED + 1))
        return
    fi

    # Baseline counts.
    mt=$(grep -c "All 15 tests passed" build/serial.log)
    bn=$(grep -c "create+write64+delete (TagFS+disk)" build/serial.log)
    fl=$(grep -c "^Files:" build/serial.log)
    pn=$(grep -cE "PANIC|^\\[EXCEPTION\\]" build/serial.log)
    at=$(grep -c "ATRC" build/serial.log)
    un=$(grep -c "Unknown command" build/serial.log)
    # TSC band check — bench prints "TSC freq: <N> kHz" via boxlib's
    # cpu_get_tsc_freq_khz(). The kernel emulates a nominal 1 GHz TSC
    # under QEMU TCG, but the actual measured rate jitters under host
    # CPU contention because:
    #   - HPET / PMT busy-wait loops issue MMIO reads that each trap
    #     into the emulator; trap overhead scales with host load.
    #   - macOS host scheduler doesn't pin guest VCPUs; with -smp 16
    #     the per-core wallclock per emulated instruction drifts up
    #     to ~10 % between runs.
    # The kernel side already mitigates this (PMT-before-HPET source
    # priority under TCG + median-of-5 multi-sample PMT — see
    # cpu_calibrate.c). The matrix tolerance band 1.0–1.199 GHz (10 %)
    # accepts the residual TCG variance while still failing on a
    # truly broken calibration (zero, garbage, or out-of-range).
    # On real silicon both timers are sub-nanosecond-precise and the
    # measurement converges within <0.1 % — the same pattern matches
    # bare-metal trivially.
    tsc_good=$(grep -cE "TSC freq: 1[0-1][0-9]{5}|TSC source:.*— 1[0-1][0-9]{5} kHz" build/serial.log)

    # Nightwatch speaks only with proof, and until now nobody here listened:
    # the day matrix of 2026-09-06 was announced 6/6 over a verdict of eleven
    # lost wakes (a false one, as it turned out — the harness could not have
    # told either way). A verdict is the kernel saying that the machine has
    # stopped and on what; it fails the configuration by itself, whatever the
    # test markers say. The summary line is counted rather than the "‼" lines
    # under it: a stall is described once per look, and the count of looks is
    # not the count of stalls.
    nw=$(grep -c "\[NIGHTWATCH\] VERDICT: .*a defect, not a slow test" build/serial.log)

    # Extended-suite counts.
    mtest_p=$(grep -c     "\[mtest\] PASS"            build/serial.log)
    chain_p=$(grep -c     "\[chain\] PASS"            build/serial.log)
    cow_p=$(grep -c       "\[COW\] PASS"              build/serial.log)
    lc_p=$(grep -c        "\[LIFECYCLE\] PASS"        build/serial.log)
    wo_p=$(grep -c        "\[WO\] PASS"               build/serial.log)
    wc_p=$(grep -c        "\[WC\] PASS"               build/serial.log)
    ws_p=$(grep -c        "\[WS SUMMARY\] all 3 PASS" build/serial.log)
    tt_p=$(grep -c        "\[TT SUMMARY\]"            build/serial.log)
    decks_p=$(grep -cE    "\\[decks\\] [0-9]+ passed, 0 failed" build/serial.log)
    ts1_p=$(grep -cE      "\[STRESS S1\].*PASS"       build/serial.log)
    ts2_p=$(grep -cE      "\[STRESS S2\].*PASS"       build/serial.log)
    ts3_p=$(grep -cE      "\[STRESS S3\].*PASS"       build/serial.log)

    # C++ runtime / Current / Strands / Manifest-handle suites. These run in
    # BOTH the fast and full bursts but were previously NEITHER counted NOR
    # gated, so a [CXX]/[CURRENT]/[STRAND]/[htest] FAIL passed the matrix green.
    # strandtest legitimately prints "[STRAND] SKIP" when FSGSBASE is absent;
    # accept PASS or SKIP as "ran to completion" and let app_fail catch FAIL.
    cxx_p=$(grep -c       "\[CXX\] ALL PASS"          build/serial.log)
    current_p=$(grep -c   "\[CURRENT\] ALL PASS"      build/serial.log)
    strand_p=$(grep -cE   "\[STRAND\] (PASS|SKIP)"    build/serial.log)
    htest_p=$(grep -c     "\[htest\] PASS"            build/serial.log)
    # The Use Context end to end: use.set from a system utility, create stamped
    # inside it, query narrowed to it, _everywhere unaffected, clear restores.
    use_p=$(grep -c       "\[USE\] PASS"              build/serial.log)
    # The Luggage end to end: a child gets the typed line whole — short (in the
    # CabinInfo page) and thousands of bytes (in its buffer heap).
    luggage_p=$(grep -c   "\[LUGGAGE\] PASS"          build/serial.log)

    # Aggregate any negative-FAIL marker emitted by an app.
    # The prefix may have a sub-tag inside the brackets:
    #   [TT 6] FAIL: ...
    #   [STRESS S1] producer-consumer: ... FAIL
    #   [WS S3] PASS — ... (← no fail; line contains FAIL only via marker mismatch)
    # so the pattern accepts "[KEY any-non-]]\]" followed by "FAIL" or "fail"
    # anywhere on the same line. Tightened to FAIL as a word so substrings
    # inside legitimate identifiers don't trip it.
    app_fail=$(grep -cE  "\[(mtest|chain|COW|LIFECYCLE|WO|WC|WS|TT|decks|STRESS|CXX|CURRENT|STRAND|htest|USE|LUGGAGE)[^]]*\].*(\\bFAIL\\b|\\bfail\\b)" build/serial.log)

    # The kernel's own boot self-test (TagFS core, BCDC, journal, snapshot,
    # CoW, BoxHash, integrity, dedup). It runs on EVERY boot in EVERY config
    # and its verdict used to gate nothing at all — a config where the
    # filesystem's own suite failed still came out green here.
    selftest_ok=$(grep -c   "\[TESTS\] All tests PASSED"   build/serial.log)
    selftest_bad=$(grep -c  "\[TESTS\] Some tests FAILED"  build/serial.log)

    # The boot-time proof of a read nobody stands over. It speaks on every
    # boot: PASSED where the machine reads that way (several cores, a seat
    # with a completion — the AHCI seat every UEFI config here stands on),
    # "not asked" where it does not (one core; the legacy channel under BIOS).
    # A FAILED is the medium, the completion spine or the bytes being wrong on
    # the boot path, before userspace. It was red on every UEFI config for two
    # weeks and gated nothing.
    case "$cfg_name" in
        UEFI*4c|UEFI*16c) ur_want="PASSED" ;;
        *)                ur_want="not asked" ;;
    esac
    ur_ok=$(grep -c   "\[UNATTENDED READ\].*${ur_want}" build/serial.log)
    ur_fail=$(grep -c "\[UNATTENDED READ\].*FAILED"     build/serial.log)

    echo "  baseline: memtest=$mt/3 files=$fl/2 bench=$bn/2"
    echo "  apps:     mtest=$mtest_p chain=$chain_p cow=$cow_p lc=$lc_p wo=$wo_p wc=$wc_p ws=$ws_p tt=$tt_p decks=$decks_p ts=$ts1_p/$ts2_p/$ts3_p"
    echo "  suites:   cxx=$cxx_p current=$current_p strand=$strand_p htest=$htest_p use=$use_p luggage=$luggage_p"
    echo "  selftest: kernel=$selftest_ok bad=$selftest_bad unattended(${ur_want})=$ur_ok fail=$ur_fail"
    echo "  negatives: PANIC=$pn ATRC=$at Unknown=$un AppFAIL=$app_fail Nightwatch=$nw TSC=~1GHz:$tsc_good/$bn"

    ok=1
    if [ "$MODE" = "fast" ]; then
        # Fast mode requires only the trimmed burst (×1 each).
        [ "$mt" -lt 1 ] && ok=0
        [ "$fl" -lt 1 ] && ok=0
        [ "$bn" -lt 1 ] && ok=0
    else
        [ "$mt" -lt 3 ] && ok=0
        [ "$fl" -lt 2 ] && ok=0
        [ "$bn" -lt 2 ] && ok=0
    fi
    [ "$pn" -gt 0 ] && ok=0
    [ "$at" -gt 0 ] && ok=0
    [ "$un" -gt 0 ] && ok=0
    [ "$tsc_good" -lt "$bn" ] && ok=0

    [ "$mtest_p" -lt 1 ] && ok=0
    [ "$chain_p" -lt 1 ] && ok=0
    [ "$cow_p"   -lt 1 ] && ok=0
    [ "$lc_p"    -lt 1 ] && ok=0
    [ "$wo_p"    -lt 1 ] && ok=0
    [ "$decks_p" -lt 1 ] && ok=0
    # Unconditional — cxxtest/current_test/strandtest/htest run in fast AND full.
    [ "$cxx_p"     -lt 1 ] && ok=0
    [ "$current_p" -lt 1 ] && ok=0
    [ "$strand_p"  -lt 1 ] && ok=0
    [ "$htest_p"   -lt 1 ] && ok=0
    [ "$use_p"     -lt 1 ] && ok=0
    [ "$luggage_p" -lt 1 ] && ok=0
    if [ "$MODE" != "fast" ]; then
        [ "$wc_p"    -lt 1 ] && ok=0
        [ "$ws_p"    -lt 1 ] && ok=0
        [ "$tt_p"    -lt 1 ] && ok=0
        [ "$ts1_p"   -lt 1 ] && ok=0
        [ "$ts2_p"   -lt 1 ] && ok=0
        [ "$ts3_p"   -lt 1 ] && ok=0
    fi

    [ "$app_fail" -gt 0 ] && ok=0
    [ "$nw" -gt 0 ] && ok=0

    [ "$selftest_bad" -gt 0 ] && ok=0
    [ "$selftest_ok"  -lt 1 ] && ok=0
    [ "$ur_ok"   -lt 1 ] && ok=0
    [ "$ur_fail" -gt 0 ] && ok=0

    [ -n "$TIMED_OUT_CMD" ] && ok=0
    [ -n "$TYPING_FAILED_CMD" ] && ok=0

    if [ "$ok" = "1" ]; then
        echo "  RESULT: PASS"
    else
        echo "  RESULT: FAIL"
        FAILED=$((FAILED + 1))
    fi
    echo ""
}

# Build once.
#
# DEBUG=on environment variable (or first arg "debug") enables
# CONFIG_DEBUG_ENABLED/CONFIG_DEBUG_MODE so debug_printf surfaces on
# serial. Use when chasing a regression — without it debug_printf
# silently no-ops and the only kernel chatter on serial is kprintf
# (PANIC, perf_dump, deliberate diagnostic prints).
#
#   DEBUG=on tools/stress_matrix.sh
#   tools/stress_matrix.sh debug
#   tools/stress_matrix.sh fast debug
#
# Note: stress_matrix passes pattern checks regardless of DEBUG, but
# DEBUG-on logs are MUCH larger (~10× serial.log size) and slow each
# config by a few seconds. Don't enable for routine validation.
DEBUG_FLAG=${DEBUG:-off}
for arg in "$@"; do
    case "$arg" in
        debug|DEBUG=on|debug=on) DEBUG_FLAG=on ;;
    esac
done

echo "Building kernel + userspace... (mode=$MODE debug=$DEBUG_FLAG)"
make DEBUG=$DEBUG_FLAG >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 2; }

#
# STRICT=on is the DEFAULT for every config: production-grade BoxOS must
# work on real-HW with -cpu max (PKU, PKS, SMAP, SMEP, UMIP, 1 GB pages,
# WAITPKG, RDRAND/RDSEED, ...). Passing under qemu64 default is proof of
# nothing — qemu64 is a 2003-era K8 with none of those features. Per
# user directive 2026-06-07: "со STRICT запускать лучше чем без него.
# Всегда". Use STRICT=off only to investigate a regression that is
# clearly orthogonal to CPU features.
#
if [ "$MODE" = "fast" ]; then
    # 4 representative configs covering both firmwares + uniprocessor +
    # small SMP. Skips 16c (slowest) — full matrix covers it.
    run_config "BIOS STRICT 1c"    "STRICT=on CORES=1  MEM=2G"
    run_config "BIOS STRICT 4c"    "STRICT=on CORES=4  MEM=4G"
    run_config "UEFI STRICT 1c"    "UEFI=on STRICT=on CORES=1  MEM=2G"
    run_config "UEFI STRICT 4c"    "UEFI=on STRICT=on CORES=4  MEM=4G"
else
    run_config "BIOS STRICT 1c"    "STRICT=on CORES=1  MEM=2G"
    run_config "BIOS STRICT 4c"    "STRICT=on CORES=4  MEM=4G"
    run_config "BIOS STRICT 16c"   "STRICT=on CORES=16 MEM=8G"
    run_config "UEFI STRICT 1c"    "UEFI=on STRICT=on CORES=1  MEM=2G"
    run_config "UEFI STRICT 4c"    "UEFI=on STRICT=on CORES=4  MEM=4G"
    run_config "UEFI STRICT 16c"   "UEFI=on STRICT=on CORES=16 MEM=8G"
fi

echo "================================================================"
if [ "$FAILED" = "0" ]; then
    echo "ALL CONFIGS PASS"
    exit 0
else
    echo "$FAILED CONFIG(S) FAILED"
    exit 1
fi
