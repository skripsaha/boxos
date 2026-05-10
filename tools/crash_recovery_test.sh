#!/usr/bin/env bash
#
# crash_recovery_test.sh — validates DiskBook + TagFS crash safety.
#
# Test plan:
#   1. Fresh disk image. Boot QEMU.
#   2. Run `persist write` → file written via async ObjWrite path,
#      meta committed, DiskBook txn closed.
#   3. WHILE QEMU is still running shell prompt (post-write but
#      pre-bye), kill -9 the QEMU process. This simulates a
#      hard power-off — no clean tagfs_sync, no bitmap flush.
#   4. Boot QEMU again on the SAME disk image. Mount-time fsck +
#      DiskBook replay should bring the filesystem back to a
#      consistent state.
#   5. Run `persist verify` → expect PASS (the write was committed
#      to disk before the kill via the W_AHCI_DONE → W_LOG_META
#      → meta_pool_write chain).
#
# Pass criteria: persist verify reports "file survived reboot".
# Fail criteria: PERSIST FAIL or kernel panic on mount.

set -e

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJ"

LOG="$PROJ/build/serial.log"
PIDFILE="$PROJ/build/qemu.pid"
QINPUT="$PROJ/tools/qemu-input.sh"

cleanup() {
    pkill -9 -f qemu-system-x86_64 2>/dev/null || true
    sleep 2
    rm -f "$LOG" "$PROJ/build/qemu.mon" "$PIDFILE"
}

wait_for() {
    local pat="$1"
    local timeout="${2:-90}"
    local start
    start=$(date +%s)
    while true; do
        if grep -qE "$pat" "$LOG" 2>/dev/null; then
            return 0
        fi
        local now
        now=$(date +%s)
        if (( now - start > timeout )); then
            echo "[CRT] TIMEOUT waiting for: $pat"
            return 1
        fi
        sleep 2
    done
}

echo "[CRT] === CRASH RECOVERY TEST ==="

cleanup

# Fresh disk
rm -f "$PROJ/build/boxos.img"
echo "[CRT] Building..."
make AHCI=on > /tmp/crt_build.log 2>&1

echo "[CRT] Phase 1: boot, write, kill -9"
make run-bg AHCI=on CORES=4 MEM=2G > /tmp/crt_run1.log 2>&1
sleep 4
wait_for "BoxOS Shell" 90 || { cleanup; exit 1; }

"$QINPUT" type "persist write" > /dev/null
"$QINPUT" key ret > /dev/null

# Wait write to complete (async path emits WROTE on KResultPush)
wait_for "PERSIST.*WROTE" 30 || { cleanup; exit 1; }

# Give a moment for the system to settle (meta_pool flush, bitmap state)
# but DON'T issue bye — we want a hard crash. The async write committed
# meta_pool through W_LOG_META → meta_pool_write, which leaves the
# record in g_current_block (in-memory mirror only). The bitmap and
# committed meta_pool block flush happen at tagfs_sync (called from
# bye). So the kill below tests whether DiskBook journal replay can
# reconstruct the metadata.
sleep 2

echo "[CRT] killing QEMU (simulating power loss)..."
QPID=$(cat "$PIDFILE" 2>/dev/null || echo "")
if [ -n "$QPID" ]; then
    kill -9 "$QPID" 2>/dev/null || true
fi
pkill -9 -f qemu-system-x86_64 2>/dev/null || true
sleep 3

echo "[CRT] Phase 1 log tail:"
tail -10 "$LOG" | sed 's/^/  /'

# Save phase-1 log for inspection
cp "$LOG" /tmp/crt_phase1.log

echo "[CRT] Phase 2: boot on same disk, verify"
rm -f "$LOG" "$PROJ/build/qemu.mon" "$PIDFILE"
make run-bg AHCI=on CORES=4 MEM=2G > /tmp/crt_run2.log 2>&1
sleep 4
wait_for "BoxOS Shell" 90 || { cleanup; exit 1; }

"$QINPUT" type "persist" > /dev/null
"$QINPUT" key ret > /dev/null

if wait_for "PERSIST.*(PASS|FAIL)" 30; then
    if grep -q "PERSIST.*PASS" "$LOG"; then
        echo "[CRT] === PASS — file survived hard crash + reboot ==="
        cleanup
        exit 0
    else
        echo "[CRT] === FAIL — persist verify failed ==="
        grep "PERSIST" "$LOG" | sed 's/^/  /'
        cleanup
        exit 1
    fi
else
    echo "[CRT] === FAIL — persist verify timed out ==="
    tail -10 "$LOG" | sed 's/^/  /'
    cleanup
    exit 1
fi
