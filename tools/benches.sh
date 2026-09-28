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
# A bench is a directory BENCH_DIR/N, made by mkdir — atomic, so two jobs can
# never take the same one. It names the build directory whose qemu.pid it
# covers. A bench whose machine is gone — no pidfile, or the pid is no QEMU any
# more — is free again once it is older than the start window, so a job killed
# without run-stop, or a guest that switched itself off, holds nothing for long.
# A job that already holds a bench and boots again keeps the one it has.
#
# BENCHES=0 turns the limit off. BENCH_DIR is one path for the whole host (not
# under TMPDIR, which every job sets to its own).

set -u
BENCHES=${BENCHES:-3}
BENCH_DIR=${BENCH_DIR:-/tmp/boxos-benches}
START_WINDOW=60      # seconds from taking a bench to QEMU writing its pidfile

usage() { echo "usage: tools/benches.sh take|give BUILDDIR" >&2; exit 2; }
[ $# = 2 ] || usage
op=$1
build=$(cd "$2" 2>/dev/null && pwd -P) || { echo "[bench] there is no directory $2" >&2; exit 2; }
case "$BENCHES" in ''|*[!0-9]*) echo "[bench] BENCHES='$BENCHES' is not a number" >&2; exit 2 ;; esac
[ "$BENCHES" -gt 0 ] || exit 0

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

take() {
    local n started waited=0 said=0
    mkdir -p "$BENCH_DIR"
    started=$(date +%s)
    while :; do
        for n in $(seq 1 "$BENCHES"); do
            if [ "$(holder "$n")" = "$build" ]; then
                date +%s > "$BENCH_DIR/$n/taken"
                return 0
            fi
        done
        for n in $(seq 1 "$BENCHES"); do
            if machine_gone "$n"; then
                echo "[bench] bench $n of $BENCHES is free again: the machine of $(holder "$n") is gone"
                rm -rf "${BENCH_DIR:?}/$n"
            fi
            if mkdir "$BENCH_DIR/$n" 2>/dev/null; then
                echo "$build" > "$BENCH_DIR/$n/build"
                date +%s > "$BENCH_DIR/$n/taken"
                [ "$said" = 1 ] && echo "[bench] took bench $n of $BENCHES after waiting ${waited} s"
                return 0
            fi
        done
        if [ "$said" = 0 ]; then
            echo "[bench] all $BENCHES benches are taken — waiting for one:"
            for n in $(seq 1 "$BENCHES"); do echo "[bench]   $n: $(holder "$n")"; done
            said=1
        fi
        sleep 1
        waited=$(( $(date +%s) - started ))
    done
}

give() {
    local b
    for b in "$BENCH_DIR"/*/; do
        [ -d "$b" ] && [ "$(cat "$b/build" 2>/dev/null)" = "$build" ] && rm -rf "${b:?}"
    done
    return 0
}

case "$op" in
    take) take ;;
    give) give ;;
    *)    usage ;;
esac
