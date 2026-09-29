#!/bin/bash
# benches — how many machines the stand may run on this host at once.
#
#   tools/benches.sh take BUILDDIR    wait for a free bench, then hold it for BUILDDIR
#   tools/benches.sh give BUILDDIR    give back the bench BUILDDIR holds
#
# `make run-bg` takes a bench before QEMU starts and `make run-stop` gives it
# back; nothing else has to know. Several jobs work on this host at once, each
# in its own worktree, and every one of them boots QEMU under TCG. Past a few
# machines the host's cores are shared out so thinly that a guest misses the
# waits the harnesses give it, and a red that is only the host's load costs a
# re-run and a doubt. Reading and writing code in parallel costs nothing; only
# the machines queue.
#
# A bench is a directory BENCH_DIR/N naming the build directory whose qemu.pid
# it covers. A bench whose machine is gone — no pidfile, or the pid is no QEMU
# any more — is free again once it is older than the start window, so a job
# killed without run-stop, or a guest that switched itself off, holds nothing
# for long. A job that already holds a bench and boots again keeps the one it
# has.
#
# ‼ EVERY LOOK AT THE BENCHES AND EVERY CHANGE TO THEM IS MADE UNDER ONE LOCK.
# Freeing a stale bench is a look (is its machine gone?) and then a removal,
# and two jobs that both looked could both remove — the second one removing
# the bench the first had just taken afresh, and two machines then running on
# one bench. So taking, freeing and giving back all run under an flock on
# BENCH_DIR/.lock, held by a python3 child for the length of one look: the
# kernel lets go of it when that child ends, however it ends, so a job killed
# in the middle leaves no lock behind.
#
# ‼ THE WAIT HAS NO BOUND OF ITS OWN, AND SAYS WHAT IT WAITS FOR. A bench is
# freed by its machine going away, which is the event the wait is for; a
# bench whose QEMU still runs is held, whoever started it. What the wait owes
# is to say so: on stderr (which a harness folds into the log it keeps of
# run-bg), each bench, its build directory, its QEMU's pid and how long it
# has been held — again whenever that changes, and once a minute while it
# does not, so a log that stops there names the machines to stop.
#
# BENCHES=0 turns the limit off. BENCH_DIR is one path for the whole host (not
# under TMPDIR, which every job sets to its own).

set -u
BENCHES=${BENCHES:-3}
BENCH_DIR=${BENCH_DIR:-/tmp/boxos-benches}
START_WINDOW=60      # seconds from taking a bench to QEMU writing its pidfile
SAY_AGAIN=60         # seconds between two lines of a wait nothing changed in

usage() { echo "usage: tools/benches.sh take|give BUILDDIR" >&2; exit 2; }
[ $# = 2 ] || usage
op=$1
build=$(cd "$2" 2>/dev/null && pwd -P) || { echo "[bench] there is no directory $2" >&2; exit 2; }
case "$BENCHES" in ''|*[!0-9]*) echo "[bench] BENCHES='$BENCHES' is not a number" >&2; exit 2 ;; esac
[ "$BENCHES" -gt 0 ] || exit 0
self=$(cd "$(dirname "$0")" && pwd -P)/$(basename "$0")

# holder N — the build directory bench N covers, empty when it names none.
holder() { cat "$BENCH_DIR/$1/build" 2>/dev/null; }

# machine_gone N — true when bench N was taken longer ago than the start
# window and the machine it covers is not running.
machine_gone() {
    local b=$BENCH_DIR/$1 t pid
    t=$(cat "$b/taken" 2>/dev/null) || t=$(date -r "$b" +%s 2>/dev/null) || return 1
    [ $(( $(date +%s) - t )) -ge $START_WINDOW ] || return 1
    pid=$(cat "$(holder "$1")/qemu.pid" 2>/dev/null)
    [ -n "$pid" ] && ps -p "$pid" -o comm= 2>/dev/null | grep -q qemu && return 1
    return 0
}

# locked OP — this script's own OP (_take or _give) for BUILDDIR, run with the
# host's bench lock held; answers what OP answers.
locked() {
    mkdir -p "$BENCH_DIR"
    python3 -c 'import fcntl, subprocess, sys
lock = open(sys.argv[1], "a")
fcntl.flock(lock, fcntl.LOCK_EX)
sys.exit(subprocess.call(sys.argv[2:]))' "$BENCH_DIR/.lock" "$self" "$1" "$build"
}

# try_take — under the lock: the bench this build already holds, else a free
# one, freeing the stale ones first. Answers 0 when this build holds a bench.
try_take() {
    local n
    for n in $(seq 1 "$BENCHES"); do
        if [ "$(holder "$n")" = "$build" ]; then
            date +%s > "$BENCH_DIR/$n/taken"
            return 0
        fi
    done
    for n in $(seq 1 "$BENCHES"); do
        if machine_gone "$n"; then
            echo "[bench] bench $n of $BENCHES is free again: the machine of $(holder "$n") is gone" >&2
            rm -rf "${BENCH_DIR:?}/$n"
        fi
        if mkdir "$BENCH_DIR/$n" 2>/dev/null; then
            echo "$build" > "$BENCH_DIR/$n/build"
            date +%s > "$BENCH_DIR/$n/taken"
            return 0
        fi
    done
    return 1
}

# try_give — under the lock: every bench this build holds, given back.
try_give() {
    local b
    for b in "$BENCH_DIR"/*/; do
        [ -d "$b" ] && [ "$(cat "$b/build" 2>/dev/null)" = "$build" ] && rm -rf "${b:?}"
    done
    return 0
}

# benches_held — one line per bench: its build directory, its QEMU and how
# long it has been held.
benches_held() {
    local n now t pid
    now=$(date +%s)
    for n in $(seq 1 "$BENCHES"); do
        t=$(cat "$BENCH_DIR/$n/taken" 2>/dev/null) || t=$now
        pid=$(cat "$(holder "$n")/qemu.pid" 2>/dev/null)
        echo "[bench]   $n: $(holder "$n") — QEMU ${pid:-not started yet}, held $((now - t)) s"
    done
}

take() {
    local started now waited=0 said="" said_at=0 held
    started=$(date +%s)
    while :; do
        if locked _take; then
            [ "$waited" -gt 0 ] && echo "[bench] took a bench after waiting ${waited} s" >&2
            return 0
        fi
        now=$(date +%s)
        waited=$((now - started))
        held=$(benches_held | sed -E 's/, held [0-9]+ s$//')
        if [ "$held" != "$said" ] || [ $((now - said_at)) -ge $SAY_AGAIN ]; then
            echo "[bench] all $BENCHES benches are taken — ${waited} s waited so far for one:" >&2
            benches_held >&2
            said=$held
            said_at=$now
        fi
        sleep 1
    done
}

case "$op" in
    take)  take ;;
    give)  locked _give ;;
    _take) try_take ;;      # the two below only ever run under the lock (locked)
    _give) try_give ;;
    *)     usage ;;
esac
