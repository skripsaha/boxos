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

# Push a string as keystrokes, paced.
#
# The whole string used to go to the monitor as one batch, and that is how a
# command arrived at the guest as "touch_s" instead of "touch_stress": a
# keyboard has a queue, the guest empties it only when it is scheduled to, and
# UEFI STRICT 1c — the slowest configuration there is — could not keep up.
# The keys that did not fit were dropped without a word, the shell ran a
# truncated command or none, and the matrix reported a kernel failure for a
# test that never started. A harness that manufactures failures is worse than
# no harness.
#
# QI_CHUNK keys per batch, QI_GAP seconds between batches. Both are cheap: a
# twelve-character command costs three batches and a tenth of a second.
QI_CHUNK=${QI_CHUNK:-1}
QI_GAP=${QI_GAP:-0.03}

type_push() {
    local str=$1
    local i len qcode c batch="" n=0
    len=${#str}
    for (( i=0; i<len; i++ )); do
        c=${str:i:1}
        if ! qcode=$(char_to_qcode "$c"); then
            die "unsupported char '$c' (0x$(printf '%02x' "'$c")) at pos $i"
        fi
        batch+="sendkey ${qcode}
"
        n=$((n+1))
        if [ "$n" -ge "$QI_CHUNK" ]; then
            printf '%s' "$batch" | mon_send >/dev/null
            batch=""; n=0
            sleep "$QI_GAP"
        fi
    done
    if [ -n "$batch" ]; then
        printf '%s' "$batch" | mon_send >/dev/null
    fi
}

# How many leading characters of `want` the guest echoed into `text`.
type_landed() {
    local text=$1 want=$2
    local i
    for (( i=${#want}; i>0; i-- )); do
        case "$text" in *"${want:0:i}") printf '%s' "$i"; return 0 ;; esac
    done
    printf '0'
}

cmd_type() {
    local s=$1
    local len=${#s}
    [ "$len" -eq 0 ] && return 0

    # Pacing alone makes the drop rare; it does not make it impossible, and a
    # rare silent drop is the expensive kind. The shell echoes what it took, so
    # read that back and say what happened. Measured from the log's length
    # BEFORE typing, so the same command typed twice cannot answer for itself.
    local before=0
    if [ -f "$LOG" ]; then before=$(wc -c < "$LOG" | tr -d ' '); fi

    type_push "$s"

    [ -f "$LOG" ] || return 0

    # Slow and lost look identical in one sample and not in three. Watch the
    # echo GROW: while it is still growing the guest is merely behind, and the
    # only right thing is to keep waiting. When it has not moved across three
    # consecutive looks it is not behind, it is short — and only then is the
    # missing tail sent again, once. That distinction is what the first version
    # of this check lacked, and lacking it turned "help" into "helpelp" on a
    # machine busy verifying a volume seal.
    local attempt got=0 last=-1 still=0 echoed retried=0
    for attempt in $(seq 1 30); do
        sleep 0.1
        echoed=$(tail -c "+$((before + 1))" "$LOG" 2>/dev/null | tr -d '\r')
        got=$(type_landed "$echoed" "$s")
        [ "$got" -ge "$len" ] && return 0
        if [ "$got" -eq "$last" ]; then still=$((still+1)); else still=0; fi
        last=$got
        if [ "$still" -ge 3 ] && [ "$retried" -eq 0 ]; then
            retried=1; still=0
            type_push "${s:got}"
        fi
    done

    # Nothing is retyped, and that is deliberate. The first version of this
    # check sent the tail again, which is the obvious repair and the wrong one:
    # a slow echo is indistinguishable from a lost one, and on a machine busy
    # verifying a volume seal the echo IS slow — so "help" arrived as "helpelp",
    # the shell rejected it, and two logcheck scenarios reported a kernel that
    # would not answer the keyboard. A harness may fail to deliver; it may not
    # deliver something other than what it was asked to. The pacing above is
    # the cure; this is only the witness.
    die "type: guest echoed $got of $len characters of \"$s\" — keystrokes are being dropped"
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
