#!/bin/sh
# stress_matrix.sh — multi-config stress test for BoxOS stability.
#
# For each (FW × CORES × STRICT) tuple, boot the OS, send a burst of
# commands (memtest×3 + files×2 + bench×2), then count successes /
# failures from build/serial.log. A PASS requires:
#   - All sent memtests printed "All 15 tests passed."
#   - All sent files printed "Files:" header.
#   - All sent benches printed "create+write64+delete (TagFS+disk)".
#   - Zero PANIC / EXCEPTION lines.
#   - Zero "ATRC" substrings (a canary for character-loss corruption).
#   - Zero "Unknown command" (first-cmd-no-op race).
#   - TSC freq reported in the 1 GHz range (lines matching "TSC freq: 100").
#
# Exit non-zero if any config fails.

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2

FAILED=0

run_config() {
    cfg_name=$1
    extra=$2
    echo "================================================================"
    echo "  CONFIG: ${cfg_name}"
    echo "  ${extra}"
    echo "================================================================"

    # Halt any prior QEMU; build (cached) is fine.
    make run-stop >/dev/null 2>&1 || true

    # Boot. Use eval so env strings expand correctly.
    eval "${extra} make run-bg" >/dev/null 2>&1
    # Wait for the shell prompt (UEFI GOP init is slow). Poll instead of a
    # fixed sleep so a slow-booting config is not raced.
    for i in $(seq 1 90); do
        grep -q "BoxOS Shell" build/serial.log 2>/dev/null && break
        sleep 0.5
    done
    sleep 2

    # Mixed command burst. Poll each command to completion (its marker count
    # reaches the cumulative expected) before sending the next. A fixed gap
    # was occasionally too short for memtest on a jittery SMP config, dropping
    # one "All 15 tests passed" line and producing a spurious 2/3 — the kernel
    # was fine, the test was racing. This makes the burst deterministic.
    mt_seen=0; fl_seen=0; bn_seen=0
    for c in memtest files bench memtest files bench memtest; do
        tools/qemu-input.sh type "$c" >/dev/null 2>&1
        tools/qemu-input.sh key ret  >/dev/null 2>&1
        case "$c" in
            memtest) mt_seen=$((mt_seen+1)); pat="All 15 tests passed";               want=$mt_seen ;;
            files)   fl_seen=$((fl_seen+1)); pat="^Files:";                            want=$fl_seen ;;
            bench)   bn_seen=$((bn_seen+1)); pat="create+write64+delete (TagFS+disk)"; want=$bn_seen ;;
        esac
        for i in $(seq 1 40); do   # up to ~20s per command
            [ "$(grep -c "$pat" build/serial.log)" -ge "$want" ] && break
            sleep 0.5
        done
    done
    sleep 2

    # Halt before grep so the file isn't being written.
    make run-stop >/dev/null 2>&1 || true
    sleep 1

    if [ ! -f build/serial.log ]; then
        echo "  FAIL — serial.log missing"
        FAILED=$((FAILED + 1))
        return
    fi

    mt=$(grep -c "All 15 tests passed" build/serial.log)
    bn=$(grep -c "create+write64+delete (TagFS+disk)" build/serial.log)
    fl=$(grep -c "^Files:" build/serial.log)
    pn=$(grep -cE "PANIC|^\\[EXCEPTION\\]" build/serial.log)
    at=$(grep -c "ATRC" build/serial.log)
    un=$(grep -c "Unknown command" build/serial.log)
    tsc_good=$(grep -c "TSC freq: 100" build/serial.log)

    echo "  memtest: $mt/3   files: $fl/2   bench: $bn/2"
    echo "  PANIC: $pn        ATRC: $at        Unknown: $un        TSC=~1GHz: $tsc_good/$bn"

    ok=1
    [ "$mt" -lt 3 ] && ok=0
    [ "$fl" -lt 2 ] && ok=0
    [ "$bn" -lt 2 ] && ok=0
    [ "$pn" -gt 0 ] && ok=0
    [ "$at" -gt 0 ] && ok=0
    [ "$un" -gt 0 ] && ok=0
    [ "$tsc_good" -lt "$bn" ] && ok=0

    if [ "$ok" = "1" ]; then
        echo "  RESULT: PASS"
    else
        echo "  RESULT: FAIL"
        FAILED=$((FAILED + 1))
    fi
    echo ""
}

# Build once (must succeed before any config tests).
echo "Building kernel + userspace..."
make >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 2; }

run_config "BIOS 1c"           "CORES=1  MEM=2G"
run_config "BIOS 4c"           "CORES=4  MEM=4G"
run_config "BIOS 16c"          "CORES=16 MEM=8G"
run_config "UEFI 1c"           "UEFI=on  CORES=1  MEM=2G"
run_config "UEFI 4c"           "UEFI=on  CORES=4  MEM=4G"
run_config "UEFI 16c"          "UEFI=on  CORES=16 MEM=8G"
run_config "UEFI STRICT 4c"    "UEFI=on  STRICT=on CORES=4  MEM=4G"
run_config "UEFI STRICT 16c"   "UEFI=on  STRICT=on CORES=16 MEM=8G"

echo "================================================================"
if [ "$FAILED" = "0" ]; then
    echo "ALL CONFIGS PASS"
    exit 0
else
    echo "$FAILED CONFIG(S) FAILED"
    exit 1
fi
