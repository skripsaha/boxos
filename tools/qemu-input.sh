#!/usr/bin/env bash
# tools/qemu-input.sh — drive a backgrounded QEMU via its monitor socket.
#
# Pairs with `make run-bg`. The headless QEMU exposes:
#   - monitor socket (build/qemu.mon)         — for sendkey, screendump, info...
#   - serial log    (build/serial.log)        — kernel debug_printf output
#   - pid file      (build/qemu.pid)
#
# Subcommands:
#   type "string"          — type a string char-by-char as PS/2 keystrokes
#   key  NAME              — send one named key (ret, esc, tab, backspace, ...)
#                            or a chord (ctrl-c, shift-a, alt-f4)
#   raw  "monitor cmd"     — send a raw monitor command, print its reply
#   wait REGEX [TIMEOUT]   — block until REGEX appears in serial log (def. 10s)
#   tail [N]               — print last N lines of serial log (def. 40)
#   shot [PATH]            — screendump VGA framebuffer to PPM (def. /tmp/boxos.ppm)
#   alive                  — exit 0 if QEMU process is alive, 1 otherwise
#
# Env overrides:
#   QEMU_MON   = build/qemu.mon
#   QEMU_LOG   = build/serial.log
#   QEMU_PID   = build/qemu.pid
#   NC_TIMEOUT = 1   (seconds nc waits for monitor reply after EOF)

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
MON=${QEMU_MON:-$ROOT/build/qemu.mon}
LOG=${QEMU_LOG:-$ROOT/build/serial.log}
PID=${QEMU_PID:-$ROOT/build/qemu.pid}
NC_TIMEOUT=${NC_TIMEOUT:-1}

die() { echo "qemu-input: $*" >&2; exit 1; }

require_socket() {
    [ -S "$MON" ] || die "monitor socket not found: $MON (start QEMU with 'make run-bg')"
}

# Send commands on stdin to the monitor; reply (with banner) on stdout.
mon_send() {
    require_socket
    nc -U "$MON" -w "$NC_TIMEOUT"
}

char_to_qcode() {
    case "$1" in
        [a-z])     printf '%s' "$1" ;;
        [A-Z])     printf 'shift-%s' "$(printf '%s' "$1" | tr 'A-Z' 'a-z')" ;;
        [0-9])     printf '%s' "$1" ;;
        ' ')       printf 'spc' ;;
        '!')       printf 'shift-1' ;;
        '@')       printf 'shift-2' ;;
        '#')       printf 'shift-3' ;;
        '$')       printf 'shift-4' ;;
        '%')       printf 'shift-5' ;;
        '^')       printf 'shift-6' ;;
        '&')       printf 'shift-7' ;;
        '*')       printf 'shift-8' ;;
        '(')       printf 'shift-9' ;;
        ')')       printf 'shift-0' ;;
        '-')       printf 'minus' ;;
        '_')       printf 'shift-minus' ;;
        '=')       printf 'equal' ;;
        '+')       printf 'shift-equal' ;;
        '[')       printf 'bracket_left' ;;
        ']')       printf 'bracket_right' ;;
        '{')       printf 'shift-bracket_left' ;;
        '}')       printf 'shift-bracket_right' ;;
        '\')       printf 'backslash' ;;
        '|')       printf 'shift-backslash' ;;
        ';')       printf 'semicolon' ;;
        ':')       printf 'shift-semicolon' ;;
        "'")       printf 'apostrophe' ;;
        '"')       printf 'shift-apostrophe' ;;
        ',')       printf 'comma' ;;
        '<')       printf 'shift-comma' ;;
        '.')       printf 'dot' ;;
        '>')       printf 'shift-dot' ;;
        '/')       printf 'slash' ;;
        '?')       printf 'shift-slash' ;;
        '`')       printf 'grave_accent' ;;
        '~')       printf 'shift-grave_accent' ;;
        $'\n')     printf 'ret' ;;
        $'\t')     printf 'tab' ;;
        *)         return 1 ;;
    esac
}

cmd_type() {
    local s=$1
    local i len qcode batch=""
    len=${#s}
    [ "$len" -eq 0 ] && return 0
    for (( i=0; i<len; i++ )); do
        local c=${s:i:1}
        if ! qcode=$(char_to_qcode "$c"); then
            die "unsupported char '$c' (0x$(printf '%02x' "'$c")) at pos $i"
        fi
        batch+="sendkey ${qcode}
"
    done
    printf '%s' "$batch" | mon_send >/dev/null
}

cmd_key() {
    [ $# -ge 1 ] || die "key: missing argument"
    printf 'sendkey %s\n' "$1" | mon_send >/dev/null
}

cmd_raw() {
    [ $# -ge 1 ] || die "raw: missing argument"
    printf '%s\n' "$*" | mon_send
}

cmd_wait() {
    [ $# -ge 1 ] || die "wait: missing REGEX"
    local pattern=$1 tmo=${2:-10}
    [ -f "$LOG" ] || die "serial log not found: $LOG"
    local start=$(date +%s)
    while true; do
        if grep -q -E -- "$pattern" "$LOG" 2>/dev/null; then
            return 0
        fi
        local now=$(date +%s)
        if (( now - start >= tmo )); then
            echo "qemu-input wait: timeout ${tmo}s for /$pattern/" >&2
            return 1
        fi
        sleep 0.2
    done
}

cmd_tail() {
    local n=${1:-40}
    [ -f "$LOG" ] || die "serial log not found: $LOG"
    tail -n "$n" "$LOG"
}

cmd_shot() {
    local out=${1:-/tmp/boxos.ppm}
    cmd_raw "screendump $out" >/dev/null
    sleep 0.2
    [ -f "$out" ] && echo "$out" || die "screendump produced no file at $out"
}

cmd_alive() {
    [ -f "$PID" ] || return 1
    kill -0 "$(cat "$PID")" 2>/dev/null
}

case ${1:-} in
    type)  shift; cmd_type "${1:-}" ;;
    key)   shift; cmd_key  "${1:-}" ;;
    raw)   shift; cmd_raw  "$*" ;;
    wait)  shift; cmd_wait "$@" ;;
    tail)  shift; cmd_tail "${1:-40}" ;;
    shot)  shift; cmd_shot "${1:-/tmp/boxos.ppm}" ;;
    alive) shift; cmd_alive ;;
    ""|-h|--help|help)
        sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
        ;;
    *) die "unknown subcommand '$1' (try: type|key|raw|wait|tail|shot|alive)" ;;
esac
