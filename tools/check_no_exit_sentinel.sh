#!/bin/sh
# check_no_exit_sentinel.sh — fail the build if the legacy 0xFE child-exit IPC
# sentinel is reintroduced anywhere in userspace.
#
# Ф26d migrated every consumer (shell, ipc_test) off the `{0xFE, code}` "child
# tells its spawner it exited" send onto the kernel's process:died Touch event —
# which fires on clean exit AND crash, carries the real exit_code plus the
# (pid, generation) canonical identity, and rides a SEPARATE ring from the
# keyboard/args IPC. exit() now signals death only through SYS_PROC_KILL(self);
# it sends the spawner nothing. The hack is deleted; this guard keeps it deleted:
# a reintroduced sentinel would resurrect a redundant, crash-blind, ring-crossing
# exit signal no consumer reads — silent dead weight at best, a mis-routed Result
# at worst.
#
# Scope: src/userspace ONLY. The kernel legitimately uses 0xFE elsewhere (ACPI
# reset, VGA font glyphs, stage2) — unrelated, out of scope.
#
# Two reintroduction shapes are caught precisely:
#   1. the SHELL_EXIT_SENTINEL macro — any definition or use; and
#   2. the send literal `{ 0xFE, ... }` — the exact byte-array shape of
#      `uint8_t msg[2] = { 0xFE, code };`.
# The bare token 0xFE is NOT matched on its own, so the legit userspace hits stay
# clean: cmath hex tables (0xFE5163), color.h (0xFE000000), cxxtest (0xFEEDBEEF)
# — none contain `{ 0xFE,` or the macro name. Pure source grep; wired into
# `make all`.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DIR="$ROOT/src/userspace"

[ -d "$DIR" ] || { echo "[no-exit-sentinel] cannot read $DIR"; exit 2; }

# 1. the named macro — a re-add of `#define SHELL_EXIT_SENTINEL 0xFE`, or any use.
named=$(grep -rn "SHELL_EXIT_SENTINEL" "$DIR" 2>/dev/null)

# 2. the send literal — `{ 0xFE, ... }`, whitespace-tolerant, case-insensitive.
#    The comma must follow 0xFE, so 0xFE5163 / 0xFE000000 / 0xFEEDBEEF never match.
literal=$(grep -rnEi '\{[[:space:]]*0xFE[[:space:]]*,' "$DIR" 2>/dev/null)

if [ -z "$named" ] && [ -z "$literal" ]; then
    echo "[no-exit-sentinel] OK — no 0xFE child-exit sentinel in src/userspace"
    exit 0
fi

echo "[no-exit-sentinel] FAIL — the legacy 0xFE child-exit sentinel is back:"
[ -n "$named" ]   && { echo "  SHELL_EXIT_SENTINEL:";        echo "$named"   | sed 's/^/    /'; }
[ -n "$literal" ] && { echo "  { 0xFE, ... } send literal:"; echo "$literal" | sed 's/^/    /'; }
echo "[no-exit-sentinel] fix: a child's exit is observed on the process:died"
echo "                Touch event (box/touch.h TouchProcessDied), never an IPC send."
exit 1
