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

# Per-command poll budget (units of 0.5 s). Big tests (touch_stress,
# write_stress, decks) genuinely take ~30 s on saturated hosts, so 240
# = 120 s ceiling. Empty loop exits the instant the marker appears so
# fast tests cost nothing. Fast mode trims to 60 s ceiling since the
# heavy tests are skipped.
if [ "$MODE" = "fast" ]; then
    POLL_TICKS=120
else
    POLL_TICKS=240
fi

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
    # Single-run tests reuse `want=1` against the first appearance of
    # their pattern. Fast mode drops historical-regression duplicates
    # and the long-tail tests (write_stress ~30 s, touch_stress ~30 s
    # plus historical flakiness — see memory:touch_stress_complete*).
    if [ "$MODE" = "fast" ]; then
        burst="memtest files bench mtest chain cow_test lifecycle write_observer decks htest"
    else
        burst="memtest files bench memtest files bench memtest \
               mtest chain htest cow_test lifecycle write_observer \
               write_concurrent write_stress touch_test decks touch_stress"
    fi
    for c in $burst
    do
        tools/qemu-input.sh type "$c" >/dev/null 2>&1
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
                pat="create+write64+delete (TagFS+disk)"; want=$bn_seen ;;
            mtest)            pat="\[mtest\] PASS";           want=1 ;;
            chain)            pat="\[chain\] PASS";           want=1 ;;
            htest)            pat="\[htest\] PASS";           want=1 ;;
            cow_test)         pat="\[COW\] PASS";             want=1 ;;
            lifecycle)        pat="\[LIFECYCLE\] PASS";       want=1 ;;
            write_observer)   pat="\[WO\] PASS";              want=1 ;;
            write_concurrent) pat="\[WC\] PASS";              want=1 ;;
            write_stress)     pat="\[WS SUMMARY\] all 3 PASS";want=1 ;;
            touch_test)       pat="\[TT SUMMARY\]";           want=1 ;;
            decks)            pat=", 0 failed";               want=1 ;;
            touch_stress)
                # Wait for the FINAL "[STRESS] Done" marker —
                # touch_stress prints it after S1+S2+S3 regardless of
                # individual PASS/FAIL, so the poll exits as soon as
                # the whole suite finishes; per-substage PASS is checked
                # at grep-summary time.
                pat="\[STRESS\] Done";                        want=1 ;;
        esac

        for i in $(seq 1 $POLL_TICKS); do
            [ "$(grep -cE "$pat" build/serial.log)" -ge "$want" ] && break
            sleep 0.5
        done
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
    # TSC band check — bench prints "TSC freq: 1000100 kHz" via boxlib's
    # cpu_get_tsc_freq_khz(). Match any 100xxxx (≈ 1 GHz) value reported by
    # bench, OR the kernel-side `[CPU] TSC source: ... — 100xxxx kHz`
    # boot log (defensive: matches whichever side is in the log).
    tsc_good=$(grep -cE "TSC freq: 100[0-9]{4}|TSC source:.*— 100[0-9]{4} kHz" build/serial.log)

    # Extended-suite counts.
    mtest_p=$(grep -c     "\[mtest\] PASS"            build/serial.log)
    chain_p=$(grep -c     "\[chain\] PASS"            build/serial.log)
    cow_p=$(grep -c       "\[COW\] PASS"              build/serial.log)
    lc_p=$(grep -c        "\[LIFECYCLE\] PASS"        build/serial.log)
    wo_p=$(grep -c        "\[WO\] PASS"               build/serial.log)
    wc_p=$(grep -c        "\[WC\] PASS"               build/serial.log)
    ws_p=$(grep -c        "\[WS SUMMARY\] all 3 PASS" build/serial.log)
    tt_p=$(grep -c        "\[TT SUMMARY\]"            build/serial.log)
    decks_p=$(grep -c     ", 0 failed"                build/serial.log)
    ts1_p=$(grep -cE      "\[STRESS S1\].*PASS"       build/serial.log)
    ts2_p=$(grep -cE      "\[STRESS S2\].*PASS"       build/serial.log)
    ts3_p=$(grep -cE      "\[STRESS S3\].*PASS"       build/serial.log)

    # Aggregate any negative-FAIL marker emitted by an app.
    # The prefix may have a sub-tag inside the brackets:
    #   [TT 6] FAIL: ...
    #   [STRESS S1] producer-consumer: ... FAIL
    #   [WS S3] PASS — ... (← no fail; line contains FAIL only via marker mismatch)
    # so the pattern accepts "[KEY any-non-]]\]" followed by "FAIL" or "fail"
    # anywhere on the same line. Tightened to FAIL as a word so substrings
    # inside legitimate identifiers don't trip it.
    app_fail=$(grep -cE  "\[(mtest|chain|COW|LIFECYCLE|WO|WC|WS|TT|decks|STRESS)[^]]*\].*(\\bFAIL\\b|\\bfail\\b)" build/serial.log)

    echo "  baseline: memtest=$mt/3 files=$fl/2 bench=$bn/2"
    echo "  apps:     mtest=$mtest_p chain=$chain_p cow=$cow_p lc=$lc_p wo=$wo_p wc=$wc_p ws=$ws_p tt=$tt_p decks=$decks_p ts=$ts1_p/$ts2_p/$ts3_p"
    echo "  negatives: PANIC=$pn ATRC=$at Unknown=$un AppFAIL=$app_fail TSC=~1GHz:$tsc_good/$bn"

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
    if [ "$MODE" != "fast" ]; then
        [ "$wc_p"    -lt 1 ] && ok=0
        [ "$ws_p"    -lt 1 ] && ok=0
        [ "$tt_p"    -lt 1 ] && ok=0
        [ "$ts1_p"   -lt 1 ] && ok=0
        [ "$ts2_p"   -lt 1 ] && ok=0
        [ "$ts3_p"   -lt 1 ] && ok=0
    fi

    [ "$app_fail" -gt 0 ] && ok=0

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
