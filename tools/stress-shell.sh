#!/usr/bin/env bash
# tools/stress-shell.sh — type a wide variety of inputs into a backgrounded
# QEMU and watch the serial log for any kernel `[EXCEPTION]`. Pairs with
# `make run-bg` and `tools/qemu-input.sh`.
#
# Exit:
#   0 — every command in the battery completed without a kernel exception
#   1 — exception detected (or QEMU died); excerpt printed to stdout
#
# Env overrides:
#   STRESS_LOG  = build/serial.log
#   STRESS_INPUT = tools/qemu-input.sh
#   STRESS_PAUSE = 0.6        seconds to sleep between commands
set -uo pipefail

LOG=${STRESS_LOG:-build/serial.log}
INPUT=${STRESS_INPUT:-tools/qemu-input.sh}
PAUSE=${STRESS_PAUSE:-0.6}

if [ ! -x "$INPUT" ]; then
    echo "stress: $INPUT not found or not executable" >&2
    exit 2
fi
if [ ! -f "$LOG" ]; then
    echo "stress: $LOG not present — start QEMU with 'make run-bg' first" >&2
    exit 2
fi

# Mark log so the post-mortem section ignores boot-time lines.
echo "===== STRESS START $(date +%H:%M:%S) =====" >> "$LOG"

# (cmd , optional pause-seconds-after )
COMMANDS=(
  "help"
  "clear"
  "help"
  "info"
  "today"
  "me"
  "name"
  "files"
  "decks"
  "show"
  "tag"
  "untag"
  "fsck"
  "defrag"
  "ipc_test"
  "say hello"
  "say there are spaces here"
  "help help"
  "no_such_cmd_42"
  "files something_that_doesnt_exist"
  "create x"
  "tag autostart system"
  "use system"
  "help"
  "use"
  "name foo"
  "say !@#%^*()_+-=[]{};:,./?"
  "say abc 123 456"
  "decks"
  "info"
  ""
  ""
  "help"
  "clear"
  "say end_of_pass_1"
  "files"
  "today"
  "me"
  "show"
  "say end_of_pass_2"
)

i=0
fail=0
for cmd in "${COMMANDS[@]}"; do
  i=$((i+1))
  printf "[%02d/%02d] %-50s " "$i" "${#COMMANDS[@]}" "${cmd:-<empty>}"
  if [ -n "$cmd" ]; then
    "$INPUT" type "$cmd" >/dev/null
  fi
  "$INPUT" key ret >/dev/null
  sleep "$PAUSE"
  if grep -q '\[EXCEPTION\]' "$LOG" 2>/dev/null; then
    echo "FAULT"
    fail=1
    break
  fi
  if ! "$INPUT" alive; then
    echo "QEMU DEAD"
    fail=1
    break
  fi
  echo "ok"
done

echo "----- summary -----"
if [ $fail -eq 0 ]; then
  echo "ALL ${#COMMANDS[@]} COMMANDS COMPLETED, NO EXCEPTION"
  exit 0
fi

echo "STOPPED AT COMMAND $i: ${COMMANDS[$((i-1))]}"
echo "------ exception excerpt ------"
grep -A 14 '\[EXCEPTION\]' "$LOG" | head -40
exit 1
