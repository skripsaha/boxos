#!/usr/bin/env bash
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
TOOLS="$REPO/tools"
BUILD="$REPO/build"
SERIAL="$BUILD/serial.log"

fail() { echo "FAIL: $1"; }

kill_qemu() {
    pkill -f "qemu-system-x86_64" 2>/dev/null || true
    sleep 1
}

echo "=== Step 1: build ==="
cd "$REPO"
make clean && make || { echo "FAIL: build failed"; exit 1; }

run_scenario() {
    local label="$1"
    local app="$2"
    local cores="${3:-1}"
    local mem="${4:-512M}"
    local wait_secs="${5:-60}"
    local pass_grep="${6:-}"
    local fail_grep="${7:-FAIL}"

    echo ""
    echo "=== $label (cores=$cores) ==="

    kill_qemu

    make run-bg CORES="$cores" MEM="$mem" 2>/dev/null || true
    sleep 8

    "$TOOLS/qemu-input.sh" type "$app" 2>/dev/null || true
    "$TOOLS/qemu-input.sh" key ret   2>/dev/null || true

    sleep "$wait_secs"

    kill_qemu

    if [ ! -f "$SERIAL" ]; then
        echo "[$label] FAIL: serial.log not found"
        return 1
    fi

    echo "--- serial.log (last 60 lines) ---"
    tail -60 "$SERIAL"
    echo "---"

    if [ -n "$fail_grep" ] && grep -q "$fail_grep" "$SERIAL" 2>/dev/null; then
        echo "[$label] FAIL: found '$fail_grep' in serial.log"
        return 1
    fi

    if [ -n "$pass_grep" ] && ! grep -q "$pass_grep" "$SERIAL" 2>/dev/null; then
        echo "[$label] FAIL: did not find '$pass_grep' in serial.log"
        return 1
    fi

    echo "[$label] PASS"
    return 0
}

passed=0
total=3

# Scenario 1: touch_test single-core
if run_scenario "touch_test single-core" "touch_test" 1 512M 60 "9/9 passed" "FAIL"; then
    passed=$((passed + 1))
fi

# Scenario 2: touch_test multi-core
if run_scenario "touch_test multi-core" "touch_test" 4 1G 60 "9/9 passed" "FAIL"; then
    passed=$((passed + 1))
fi

# Scenario 3: touch_stress single-core
if run_scenario "touch_stress single-core" "touch_stress" 1 512M 90 "" "FAIL"; then
    passed=$((passed + 1))
fi

echo ""
echo "TOUCH SUITE: $passed/$total scenarios passed"

if [ "$passed" -eq "$total" ]; then
    exit 0
else
    exit 1
fi
