# stand — the machinery every BoxOS harness stands on.
#
# SOURCED, NEVER RUN. A harness names itself and sources this before it does
# anything else:
#
#     set -u
#     STAND_NAME=logcheck
#     . "$(dirname "$0")/stand.sh"
#
# From that line on it has what logcheck grew over a year of being wrong in
# ways it could measure: the verdict counters (ok, bad, chk, and harness for a
# run that could not talk to its machine), the one `make` that says who is
# asking, the mutation manifest and its three preflights, the exit trap that
# puts the tree back on every way out, the waits that end on a fact or on
# silence and never on a clock, the typer, and the Deed reader. All of it was
# cut out of tools/logcheck.sh verbatim, and the comments still tell
# logcheck's history in logcheck's words — that is where each rule was paid
# for, and "this file" in them means the harness.
#
# What a harness owes in return:
#   STAND_NAME       set before sourcing; every line this prints on the
#                    harness's behalf carries it.
#   STAND_FINISHED   set to 1 once the harness reaches its summary. A run that
#                    leaves without it has judged nothing, and exits 1.
#   stand_exit_hook  optional. Defined, it runs first on the way out, before
#                    the tree is put back. It must not call exit: an exit
#                    inside the exit trap ends the trap, and the tree would
#                    stay as the run left it.
#
# Written here rather than inside one harness, because every harness needs
# them the same way: the stand locks (one harness at a time on one SCRATCH and
# on one tree), and the Caveat judge with its one table and stand_path
# (stand_caveat.sh, sourced below).

: "${STAND_NAME:?stand.sh is sourced by a harness that names itself first — set STAND_NAME}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# ONE directory for every harness under one TMPDIR, and its name still says
# logcheck on purpose: the mutation manifest and the build-key record live
# here, and a run killed under one harness is put back by whichever harness
# runs next — which works only while they all read the same books. A job
# that must not share them runs under a TMPDIR of its own — and in a tree of
# its own, because build/ belongs to the tree, not to the TMPDIR.
SCRATCH="${TMPDIR:-/tmp}/boxos-logcheck"
mkdir -p "$SCRATCH"
cd "$ROOT" || exit 1

PASS=0; FAIL=0
ok()   { printf '  \033[32mPASS\033[0m %s\n' "$1"; PASS=$((PASS+1)); }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1"; FAIL=$((FAIL+1)); }
chk()  { if [ "$1" = 0 ]; then ok "$2"; else bad "$2"; fi }

# ‼ EVERY `make` IN THIS FILE CARRIES HARNESS=on, AND IT IS A WRAPPER.
#
# The pack gate refuses to stamp an image built from a tree that carries an
# injection — that refusal is the point of it, because an unmarked probed image
# is what reached a board on 2026-09-22 and printed 1228 lines nobody could
# account for. This script's job is to build exactly such images, so it says who
# is asking and the plate stamps them UNDER TEST instead of clean.
#
# A WRAPPER rather than a key at each call site, and that was measured the hard
# way: `build()` was given the key and `boot()` was not. boot() runs
# `make run-bg`, run-bg depends on the image, and with the tree mutated the
# image was out of date — so make rebuilt, the gate refused, QEMU never
# started, and the scenario then judged a STALE serial.log. Eighteen gates
# "passed" on evidence from the previous run. One wrapper cannot be
# half-applied, and a command-line variable is passed down to every sub-make
# by GNU make itself.
#
# NOT an exported HARNESS=on: this script already uses that name for its own
# harness-failure counter three hundred lines below, and exporting it would
# hand make the counter instead of the key.
#
# It changes no compiler flag and no byte of source: a clean tree built through
# here is stamped exactly as `make` alone would stamp it. The verdict comes from
# what the tree CONTAINS, never from this key. And it waits for the last
# mutation stamp to pass first (mut_bump, below says why).
#
# STAND_FORMAT (v1 or v2) is the format of the volume every image is made in,
# handed to make as its key (VOLUME_FORMAT, Makefile) by this one wrapper for
# the same reason: a build and the run-bg after it that disagree about it pack
# the image again between them. Unset, the Makefile's own default; a word on
# the command line says otherwise for that one call.
make() {
    mut_stamp_passed
    case " $* " in
        *" run-bg "*) strict_agrees "$@" || return 1 ;;
    esac
    command make HARNESS=on ${STAND_FORMAT:+VOLUME_FORMAT=$STAND_FORMAT} "$@"
}

# stand_format — the format this run's volumes are made in: VOLUME_FORMAT when
# the caller names one, else the one the Makefile makes when nobody does
# (VOLUME_FORMAT_DEFAULT) — read from the Makefile, never written here, so a
# scenario that asks the format and the make that builds its image can never
# answer two formats: the default moves in one line, and a harness that kept a
# default of its own would go on judging the other format's oracles over it.
stand_format() {
    if [ -n "${VOLUME_FORMAT:-}" ]; then
        echo "$VOLUME_FORMAT"
    else
        sed -n 's/^VOLUME_FORMAT_DEFAULT = //p' "$ROOT/Makefile"
    fi
}

# ===========================================================================
# A BOOT ASKS FOR THE KERNEL IT BOOTS, AND STRICT IS HALF KERNEL
# ===========================================================================
#
# Since step 1.5 STRICT=on is a build key as well as a machine key (Makefile,
# "STRICT=on, the kernel's half"): it builds LockRank's panic in. A tree built
# with one STRICT and booted with the other was built again INSIDE run-bg —
# with its output thrown away, and with build/boxos.img packed afresh under
# whatever volume the scenario had made or grown on it (stand's second boot
# found another Deed and judged nothing). A disk handed in by BOOTDISK is not
# built at all: it boots the kernel it carries, whatever the machine is.
#
# So every build here is given the STRICT its boots will ask for (strict_key),
# and a run-bg that would boot a kernel of the other STRICT is refused, said to
# the terminal as the harness's failure, and its stale serial log removed so
# nothing is judged off it: the tree's mark (build/.strict.on|off) is held to
# the words of the run-bg, and a BOOTDISK's plate (tools/buildplate say, the
# keys it was built with) to them too.
exec 8>&1

# strict_asked WORDS... — the STRICT a make invocation's words ask for: the
# last STRICT= among them, else the environment's, else off (Makefile).
strict_asked() {
    local w v=${STRICT:-off}
    for w in "$@"; do case "$w" in STRICT=*) v=${w#STRICT=} ;; esac; done
    [ "$v" = on ] && echo on || echo off
}

# strict_key WORDS... — the build key for the STRICT WORDS (a machine's keys)
# boot with, spelt out either way, so a build never inherits the one before.
strict_key() { echo "STRICT=$(strict_asked "$@")"; }

# strict_agrees RUN-BG-WORDS... — 0 when the kernel the run-bg would boot was
# built with the STRICT it asks for, or nothing says otherwise.
strict_agrees() {
    local want had disk="" w why
    want=$(strict_asked "$@")
    for w in "$@"; do case "$w" in BOOTDISK=*) disk=${w#BOOTDISK=} ;; esac; done
    if [ -n "$disk" ] && [ "$disk" != build/boxos.img ]; then
        case "$(tools/buildplate say "$disk" 2>/dev/null)" in
            *"keys: "*STRICT=on*) had=on ;;
            *"keys: "*)           had=off ;;
            *)                    return 0 ;;
        esac
        why="the disk $disk carries a kernel built with STRICT=$had"
    elif [ -f build/.strict.on ] || [ -f build/.strict.off ]; then
        had=off; [ -f build/.strict.on ] && had=on
        why="the tree was built with STRICT=$had, and run-bg would build it again unseen"
    else
        return 0
    fi
    [ "$had" = "$want" ] && return 0
    rm -f build/serial.log
    harness "a boot that asks STRICT=$want is refused: $why ($*)" >&8
    return 1
}

build() {
    make "$@" >"$SCRATCH/build.log" 2>&1
    if [ $? -ne 0 ]; then
        echo "BUILD FAILED — tail:"; tail -25 "$SCRATCH/build.log"; exit 1
    fi
}

# ===========================================================================
# THE TREE IS A WITNESS, AND A WITNESS IS RETURNED UNCHANGED
# ===========================================================================
#
# Nearly every oracle below proves its point by MUTATING the source tree —
# putting the defect back so the check must go red, or making one render cost
# what a render costs on glass — then building, running, and putting the tree
# back. What stood here was a pair of shell functions per oracle: one that did
# `cp file "$SCRATCH/file.bak"` and edited the file, one that copied the backup
# over it again. That shape lies in three ways.
#
#   1. IT IS NOT IDEMPOTENT. Run the _on twice — two oracles in one session, a
#      re-run after an interrupted one — and the second `cp` saves the ALREADY
#      MUTATED file as the pristine copy. The restore then restores the
#      mutation, permanently. This is not hypothetical: display.c carried TWO
#      copies of the overtake render-cost injection (800000 volatile loop
#      iterations per rendered frame) for an entire session. Every measurement
#      taken in that session went through them, a green oracle was called a
#      regression, and three innocent suspects in print.c were named in a
#      handoff note.
#   2. NOTHING RUNS ON THE WAY OUT. A failed build exits, `set -u` exits, ^C
#      exits, and half the run_* functions `return` early after "never reached
#      a shell" — every one of those paths leaves the mutation standing, and
#      the next thing anybody builds is not the tree they believe they built.
#   3. IT IS SILENT. A mutated tree looks exactly like a clean one.
#
# So mutation is ONE mechanism with the three properties those pairs lacked:
# it REFUSES to mutate a file that is already mutated, it RECORDS what it
# touched in a manifest that outlives this process, and it is undone by a trap
# on EXIT, which no early return, no `exit 1` and no ^C can skip. The manifest
# is read at startup too: one left behind by a run that was killed is restored
# before this run builds anything, and says so out loud.
MUT_MANIFEST="$SCRATCH/mutated.list"
MUT_PRISTINE="$SCRATCH/pristine"
MUT_DISCARD="$SCRATCH/discard"

# make(1) compares mtimes, so a file rewritten inside the same second as the
# object built from it may not rebuild at all: a mutated or restored file is
# stamped one second AHEAD of the clock, newer than anything built from it.
# ‼ AND make() WAITS FOR THE STAMP TO PASS. An object compiled while its
# source is still in the future is OLDER than it, so the next make — a `make
# run-bg` — recompiled, relinked and REPACKED build/boxos.img over whatever a
# scenario had grown or planted in it: growmut passed that way, booting the
# unstretched ground and judging nothing. Past the stamp, a known instant a
# second or two away, every object is newer than every source ever bumped.
MUT_STAMP="$SCRATCH/mut.stamp"
mut_bump() {
    local at when
    at=$(( $(date +%s) + 1 ))
    when=$(date -r "$at" +%Y%m%d%H%M.%S 2>/dev/null) \
        || when=$(date -d "@$at" +%Y%m%d%H%M.%S 2>/dev/null) \
        || { echo "mutate: no usable date(1)"; exit 1; }
    touch -t "$when" "$1" || exit 1
    echo "$at" > "$MUT_STAMP"
}
mut_stamp_passed() {
    local stamp
    stamp=$(cat "$MUT_STAMP" 2>/dev/null)
    case "$stamp" in ''|*[!0-9]*) return 0 ;; esac
    while [ "$(date +%s)" -le "$stamp" ]; do sleep 1; done
}

# The refusal, in one place, because it is the whole point of the mechanism:
# a file already under the manifest must never be copied again as its own
# pristine copy.
mut_claim() {
    local f="$1"
    if [ -f "$MUT_MANIFEST" ] && grep -Fxq "$f" "$MUT_MANIFEST"; then
        echo "mutate: $f is ALREADY mutated. Refusing to save a mutated file as"
        echo "        its own pristine copy — that is precisely the bug that let"
        echo "        an injection survive a whole session of measurements."
        return 1
    fi
    return 0
}

# Recorded only AFTER the pristine copy exists, so a manifest entry always has
# something behind it to restore.
mut_record() { printf '%s\n' "$1" >> "$MUT_MANIFEST"; }

# mutate FILE — take FILE under the manifest, BEFORE it is edited.
mutate() {
    local f="$1"
    [ -f "$f" ] || { echo "mutate: no such file: $f"; exit 1; }
    mut_claim "$f" || exit 1
    mkdir -p "$MUT_PRISTINE/$(dirname "$f")" || exit 1
    cp -p "$f" "$MUT_PRISTINE/$f" || exit 1
    mut_record "$f"
}

# mutate_product FILE — the same, for a file `make` produces rather than a file
# a person wrote. build/boxos.img is a hundred and thirty megabytes of build
# output: keeping a pristine copy of it per mutation would be hoarding a thing
# that can simply be MADE again. So a mutated product is answered by DELETING
# it, and the next build remakes it from clean sources. Same manifest, same
# refusal, same trap — the only difference is what "put it back" means.
mutate_product() {
    local f="$1"
    [ -f "$f" ] || { echo "mutate_product: no such file: $f"; exit 1; }
    mut_claim "$f" || exit 1
    mkdir -p "$MUT_DISCARD/$(dirname "$f")" || exit 1
    : > "$MUT_DISCARD/$f" || exit 1
    mut_record "$f"
}

# mut_done FILE MARKER [COUNT] — the edit is written: prove it landed (an
# anchor that silently stopped matching is a mutation oracle that quietly
# tests nothing), then make sure the build cannot miss it. Given COUNT, the
# marker must appear exactly that many times — the shape a helper that makes
# more than one edit to one file needs.
mut_done() {
    local f="$1" marker="$2" want="${3:-}" got
    # ‼ AND THE MARKER MUST NAME THIS HARNESS.
    #
    # tools/buildplate reads the tree before it stamps an image, and the way it
    # knows the tree is mutated is that a mutated file SAYS SO: every marker
    # here carries the words "logcheck injection" or "logcheck mutation", and
    # nothing in a clean src/ does. That is what lets the plate stamp an image
    # UNDER TEST instead of letting it pass as a shipped build — the flaw that
    # cost the 2026-09-22 board run.
    #
    # It was a convention across forty-eight call sites. A convention holds
    # until somebody adds the forty-ninth, so it is a rule now: a marker that
    # does not name logcheck is refused here, where the author is looking.
    case "$marker" in
        *"logcheck injection"*|*"logcheck mutation"*) ;;
        *) echo "mutate: the marker '$marker' does not say 'logcheck injection'"
           echo "        or 'logcheck mutation'. tools/buildplate reads the tree"
           echo "        for exactly those words to decide whether an image is"
           echo "        UNDER TEST, so a mutation that does not say what it is"
           echo "        can be packed and flashed as though it were a shipped"
           echo "        build. Name it and try again."
           exit 1 ;;
    esac
    if [ -n "$want" ]; then
        got=$(grep -c "$marker" "$f")
        [ "$got" = "$want" ] || {
            echo "mutate: $f carries $got copy(ies) of '$marker', expected $want — the edit did not land"
            exit 1; }
    else
        grep -q "$marker" "$f" || {
            echo "mutate: $f does not carry '$marker' — the edit did not land"; exit 1; }
    fi
    mut_bump "$f"
}

# Take one path out of the manifest. `grep -Fxv` exits non-zero when it matched
# EVERY line — that is the manifest becoming empty, which is the normal end of
# the last restore and not a failure; an empty manifest and a missing one mean
# the same thing everywhere here. If the write itself fails the move fails with
# it and the manifest is left as it was, which is the safe direction: a path
# still listed is simply restored again, and restoring twice costs nothing.
mut_forget() {
    local f="$1"
    [ -f "$MUT_MANIFEST" ] || return 0
    grep -Fxv "$f" "$MUT_MANIFEST" > "$MUT_MANIFEST.new"
    mv -f "$MUT_MANIFEST.new" "$MUT_MANIFEST" 2>/dev/null
    rm -f "$MUT_MANIFEST.new"
}

# restore FILE — put one file back and take it off the manifest. Idempotent by
# construction: a path with nothing held for it is already as it should be, so
# a second call, or a call on a path an earlier run already put back, does
# nothing and says nothing.
#
# ‼ AND IT TAKES THE IMAGE WITH IT. A put-back restores the SOURCE; it cannot
# restore the image that was packed while the source was mutated, and that
# image outlives the mutation by sitting in build/ looking exactly like a clean
# one. `git status` is silent about it — build/ is ignored — so the tree reads
# clean while build/boxos.img carries the harness's injection.
#
# That is not a hypothetical: the image flashed to the Zenbook on 2026-09-22
# was one of these. 1228 of its 2274 log lines were printed by probe_on()'s
# injection into TouchLogbookResolve, probe_off had already put the source
# back, and nothing anywhere could say so. Every timing conclusion from that
# run had to be thrown away.
#
# So the product dies with the mutation, which is this file's own doctrine for
# a mutated product (see mutate_product above): an image is a thing that can
# simply be MADE again, and the next `build` makes it from clean sources. The
# deletion goes HERE rather than in probe_off or in restore_all, because this
# is the one funnel every put-back in this file already passes through — every
# *_off helper, restore_all, and the startup preflight. It costs nothing: a
# restored source is stamped ahead of its object by mut_bump, so the next make
# was going to recompile, relink and repack anyway.
restore() {
    local f="$1"
    if [ -f "$MUT_DISCARD/$f" ]; then
        rm -f "$f" "$MUT_DISCARD/$f"
        rm -f build/boxos.img
        mut_forget "$f"
        return 0
    fi
    if [ ! -f "$MUT_PRISTINE/$f" ]; then
        mut_forget "$f"
        return 0
    fi
    cp -p "$MUT_PRISTINE/$f" "$f" || exit 1
    rm -f "$MUT_PRISTINE/$f"
    rm -f build/boxos.img
    mut_bump "$f"
    mut_forget "$f"
}

# restore_all — put everything back.
#
# Read from a SNAPSHOT rather than from the manifest, because `restore` rewrites
# the manifest as it goes and a loop must not be reading a file that is being
# rewritten under it. Read a LINE AT A TIME, because a path is a path: the word
# splitting and globbing of `for f in $(cat ...)` is wrong even where no path
# here happens to contain a space or a bracket, and a mechanism whose job is to
# be trusted must not depend on that staying true.
restore_all() {
    [ -f "$MUT_MANIFEST" ] || return 0
    local snap="$MUT_MANIFEST.undo" f
    cp "$MUT_MANIFEST" "$snap" || return 1
    while IFS= read -r f; do
        [ -n "$f" ] && restore "$f"
    done < "$snap"
    rm -f "$snap" "$MUT_MANIFEST"
}

# ===========================================================================
# A BUILD KEY IS A MUTATION TOO, AND IT IS THE ONE GIT CANNOT SEE
# ===========================================================================
#
# `make IOMMU=off` leaves a tree whose kernel is not the one anybody else
# expects, and unlike a mutated source file it leaves NOTHING for `git status`
# to show: every file is clean and only build/.iommu.off says which kernel is
# on the disk. A scenario that builds with a key and puts it back on its last
# line puts it back on exactly one path out of the function — not on ^C, not
# on `set -u`, not on an early `return` after "never reached a shell", and not
# on a failed build. The next `make run` then boots the other kernel, silently,
# and the person at the desk has no way to notice.
#
# So a build key is taken under the doctrine the mutated files already live
# under: recorded where the record outlives this process, undone by the EXIT
# trap that no early return can skip, and answered at startup when a killed
# run left one behind.
BUILDKEY_MANIFEST="$SCRATCH/buildkey"

# build_with_key KEY=VALUE... — build under the key and record that the tree is
# not the ordinary one. The record is written BEFORE make runs, because a build
# that fails half way through has already replaced objects.
#
# Returns make's status rather than exiting, which is the difference from
# build(): a scenario that builds twice has to be able to say WHICH build
# failed.
build_with_key() {
    printf '%s\n' "$*" > "$BUILDKEY_MANIFEST"
    make "$@" >"$SCRATCH/build.log" 2>&1
}

# Put the ordinary kernel back on the disk. Idempotent by construction: with
# nothing recorded there is nothing to undo, so the EXIT trap may call it after
# a scenario already has.
buildkey_restore() {
    [ -f "$BUILDKEY_MANIFEST" ] || return 0
    local key
    key=$(cat "$BUILDKEY_MANIFEST")
    echo "$STAND_NAME: the tree was built with '$key' — rebuilding it plain, because"
    echo "          the kernel on this disk is not the one the next run expects"
    echo "          and no file in git says so."
    if make >"$SCRATCH/build.restore.log" 2>&1; then
        rm -f "$BUILDKEY_MANIFEST"
        return 0
    fi
    # The record is KEPT, and the status is non-zero. Both were measured: a
    # second agent building into the same build/ tore the image out from under
    # this rebuild, the restore failed, and the run still printed "24 passed, 0
    # failed" over a tree that was carrying the other kernel. A run that leaves
    # the tree wrong is not a green run, and the record left behind is what
    # makes the next run of this file put it right before it measures anything.
    echo "$STAND_NAME: THE REBUILD FAILED — the tree still carries '$key'. The"
    echo "          record is kept, so the next run puts it back first."
    echo "          tail: $SCRATCH/build.restore.log"
    tail -10 "$SCRATCH/build.restore.log" | sed 's/^/          /'
    return 1
}

# ===========================================================================
# ONE STAND, ONE HARNESS AT A TIME
# ===========================================================================
#
# Everything above keeps its books in SCRATCH, and SCRATCH is one directory
# for every harness under the same TMPDIR. Two harnesses on it at once are not
# two runs. The second one's first preflight finds the first one's manifest,
# takes it for a run that was killed, and puts back the tree the first is in
# the middle of measuring. And every harness drives the tree's own build/ —
# its image, its QEMU pid, its serial log — whatever TMPDIR it runs under:
# a second agent tore the image out from under a rebuild and the run still
# printed a green summary (see buildkey_restore). So there are two stands,
# taken the same way: the TMPDIR's books, and the tree. The tree's lies in
# the checkout's own git directory — one per worktree, which git does not
# show and `make clean` does not remove, where build/ is removed by one and
# absent from a fresh clone.
#
# A stand is taken before anything reads the books, and it names who holds
# it: stand.lock/holder is "pid name scratch". mkdir is the lock because it is
# the one filesystem operation that either makes the directory or fails,
# whoever else is asking in the same instant. A holder whose pid is gone —
# killed by a signal no trap sees, or a desk that went down under it — is
# taken over, and that is said. The takeover is itself a mkdir, inside the
# dead holder's directory, so two harnesses that find the same dead holder
# cannot both take it: the second is refused. What cannot be told apart is
# refused too, never guessed at: a lock that names nobody is one being taken
# at that very instant, or one whose taker died in the microseconds between
# its mkdir and its name — the line says which directory to remove if no
# harness is running. Both stands refuse and never wait, so the order they
# are taken in cannot deadlock two harnesses.
STAND_TREE_LOCK="$(git -C "$ROOT" rev-parse --absolute-git-dir 2>/dev/null || { mkdir -p "$ROOT/build" && echo "$ROOT/build"; })/stand.lock"
STAND_BOOKS_LOCK="$SCRATCH/stand.lock"

# stand_lock LOCK WHAT — take LOCK; WHAT says what it guards.
stand_lock() {
    local lock=$1 what=$2 holder pid
    while ! mkdir "$lock" 2>/dev/null; do
        holder=$(cat "$lock/holder" 2>/dev/null) || holder=""
        pid=${holder%% *}
        case "$pid" in
            ''|*[!0-9]*)
                echo "$STAND_NAME: REFUSING TO RUN — $lock names no holder: a harness"
                echo "          is taking $what this very instant, or was killed doing so."
                echo "          If none is running, remove it."
                exit 1 ;;
        esac
        if kill -0 "$pid" 2>/dev/null; then
            echo "$STAND_NAME: REFUSING TO RUN — $what is taken by pid $pid (${holder#* })."
            echo "          Run again once it is done; a run of your own beside it needs a"
            echo "          worktree of its own and a TMPDIR of its own."
            exit 1
        fi
        if ! mkdir "$lock/takeover" 2>/dev/null; then
            echo "$STAND_NAME: REFUSING TO RUN — pid $pid (${holder#* }) left $what without"
            echo "          letting go of it, and another harness is taking it over right"
            echo "          now. If none is, remove $lock."
            exit 1
        fi
        # Asked again under the takeover: the directory it landed in must still
        # be the dead holder's, not one another harness made since the look.
        if [ "$(cat "$lock/holder" 2>/dev/null)" != "$holder" ]; then
            rmdir "$lock/takeover" 2>/dev/null
            continue
        fi
        echo "$STAND_NAME: $what was held by pid $pid (${holder#* }), which is gone — taking it over"
        rm -rf "$lock.gone.$$"
        mv "$lock" "$lock.gone.$$" || exit 1
        rm -rf "$lock.gone.$$"
    done
    printf '%s %s %s\n' "$$" "$STAND_NAME" "$SCRATCH" > "$lock/holder" || exit 1
}

# stand_unlock LOCK — let go of LOCK, and only if it is this run's to let go
# of. Moved aside before it is removed, so no harness ever finds it half
# deleted: a lock with no holder in it reads as one being taken, and is
# refused.
stand_unlock() {
    local lock=$1
    [ "$(cat "$lock/holder" 2>/dev/null)" = "$$ $STAND_NAME $SCRATCH" ] || return 0
    rm -rf "$lock.gone.$$"
    mv "$lock" "$lock.gone.$$" 2>/dev/null && rm -rf "$lock.gone.$$"
}

stand_lock "$STAND_TREE_LOCK" "the tree $ROOT, whose build/ this run would drive,"
# The way out until the exit trap below is set: a stand that refuses must not
# leave the other held by a pid that is about to be gone.
trap 'stand_unlock "$STAND_BOOKS_LOCK"; stand_unlock "$STAND_TREE_LOCK"' EXIT
stand_lock "$STAND_BOOKS_LOCK" "the mutation books in $SCRATCH"

# Three preflights, because the three failures they catch are different (the
# second and third in stand_preflight.sh). The first is this mechanism's own
# bookkeeping: a manifest that outlived its run.
# The second needs no bookkeeping at all and catches what the OLD pairs left
# behind — every injection and mutation a harness makes writes a comment carrying
# one of these two words, so the tree can simply be asked whether it holds one.
# A tree that does is not a tree any measurement may be taken on.
# The third is the twin of the second, pointing the other way: the second asks
# whether the tree still carries something this file put there, and the third
# asks whether the tree still carries what this file EXPECTS to find.
if [ -s "$MUT_MANIFEST" ]; then
    echo "$STAND_NAME: a previous run was killed with the tree mutated:"
    sed 's/^/          /' "$MUT_MANIFEST"
    restore_all
    echo "$STAND_NAME: the tree has been put back."
fi
if [ -s "$BUILDKEY_MANIFEST" ]; then
    echo "$STAND_NAME: a previous run was killed with the tree built under a key."
    buildkey_restore
fi
. "$ROOT/tools/stand_preflight.sh"

# ‼ THE EXIT TRAP IS NOT ENOUGH BY ITSELF, AND THAT WAS MEASURED HERE.
#
# `#!/bin/bash` on this desk is bash 3.2.57. Two things were measured against
# it before this was written:
#
#   - A script in a tight builtin loop, sent SIGINT, does not run its EXIT trap
#     and does not even stop. So INT and TERM are trapped by name.
#   - A script sent SIGINT while waiting on a child DOES leave — through the
#     EXIT trap, WITHOUT running the INT trap, and with STATUS 0. A harness
#     that answers 0 after a ^C is a false green, and a false green is the one
#     thing worse than a red.
#
# So the way out is one place, and it refuses to report success for a run that
# never reached its own summary. Everything else keeps its status: the `exit 1`
# from inside mutate and mut_done, a failed build, `set -u`, the usage branch.
STAND_FINISHED=0
on_exit() {
    local rc=$?
    # The harness's own way out goes first: after the status is taken, before
    # the tree is put back.
    declare -F stand_exit_hook >/dev/null && stand_exit_hook
    restore_all
    # After restore_all, never before: the rebuild has to be made from sources
    # that are already back to what they were. A rebuild that fails takes the
    # whole run down with it — the tree is not the one it was handed.
    buildkey_restore || rc=1
    # And the stands last, once the books they guard are closed.
    stand_unlock "$STAND_BOOKS_LOCK"
    stand_unlock "$STAND_TREE_LOCK"
    if [ "$STAND_FINISHED" = 0 ] && [ "$rc" = 0 ]; then
        echo "$STAND_NAME: the run ended before its summary. The tree has been put"
        echo "          back, and this counts as a failure — a run that was cut"
        echo "          short has judged nothing."
        exit 1
    fi
    exit "$rc"
}
trap 'exit 130' INT
trap 'exit 143' TERM
trap on_exit EXIT

# ===========================================================================
# A WAIT IS ON A FACT. GIVING UP IS ON SILENCE.
# ===========================================================================
#
# What stood at nearly every wait in this file was "N iterations of sleep 1",
# and when the loop ran out the scenario announced that the machine "never
# reached a shell". That sentence is a guess wearing a fact's clothes: N was
# chosen by whoever wrote the scenario, on the host they had that day, and a
# loaded build machine or a sixteen-core UEFI boot simply takes longer. Every
# one of those ceilings can only ever be too small — there is no N that is
# right, which is why they kept being raised.
#
# The serial log is a GROWING FILE, so there is a real fact to wait on instead.
# "This log has not grown in T seconds" is a statement about the machine: it is
# not printing, and a machine that is not printing and has not said the thing
# is wedged. "T seconds have passed" is a statement about the wall clock and
# says nothing about the machine at all. So a wait here returns the instant the
# fact appears, however long that takes, and gives up only once the machine has
# gone SILENT.
#
# The quiet window is still a number, and it is a number of a different kind:
# not "how long can this machine take", which is unbounded, but "how long can
# this machine say NOTHING and still be working", which is bounded by how
# talkative it is. A boot narrates itself; a shell echoes what it is given.
#
# The one ceiling left is a runaway guard, named as such. A guest that prints
# for ever without ever saying the thing is neither wedged nor finished, and
# the harness still has to come back to its caller. When it fires it says which
# of the two happened, because they are different faults with different cures.
WAIT_QUIET_S=${WAIT_QUIET_S:-60}
# A scenario that has deliberately throttled the medium — 128 bytes a second,
# 64 KiB/s — has a machine that can be honestly silent for a long time while it
# reads. Those waits say so by name instead of quietly raising the ordinary one.
WAIT_QUIET_SLOW_S=${WAIT_QUIET_SLOW_S:-240}
WAIT_RUNAWAY_S=${WAIT_RUNAWAY_S:-900}
# A kernel built VOLUME_TESTS=on says each startup test as it begins
# (tests.c TestRun), and a test past every one the wait has seen begun is the
# fact its runaway guard counts from. Without it the guard was a ceiling on
# the whole boot: quill's three boots run the 427 tests on a volume a gigabyte
# larger, the tests that fill it to the wall lay all of it at a few MB/s, and a
# boot was measured at 298 s to 647 s to its shell by the host's load, its
# longest test at 105 s — so the 900 s a whole boot was held to is a load
# away, and a test is held to it alone. A test number that falls back is a
# machine that began its tests again — reset, as STRICT keeps no -no-reboot
# (Makefile) — and from it nothing counts as progress: a guest that resets in
# a loop prints for ever and begins test 1 every boot.
#
# The startup tests are the default of a wait's progress, not the only one: a
# wait for a run the harness itself asked to be long says what the run says of
# its own progress (wait_for_line -P) — loggerhour's hour of a line a second
# was held to the 900 s the guard gave the whole wait, and stopped a quarter
# in. The count a progress line says is the first number on it.
STAND_TEST_BEGUN='^\[TESTS\] [0-9]+ of [0-9]+: '
WAIT_LOG=build/serial.log
# The last thing a halt says about the volume, whichever way its sync went
# (system_halt.c, halt_sync_storage). Not a bare ‼: that also opens "DID NOT
# WRITE ITS RECORDS", which comes BEFORE the flush, and a machine stopped
# there is stopped in the middle of it.
STAND_HALT_ENDS='^\[HALT\] (Storage sync complete|the medium on seat [0-9]+ took|‼ THE VOLUME ON SEAT [0-9]+ .* WAS NOT MADE DURABLE|the volume on seat [0-9]+ stands under glass)'
# How long a burst of console output may pause and still be the same burst.
# This is the other kind of number: not "how long does the machine take", which
# is unbounded, but "how big a gap inside one program's output still counts as
# the same output", which is a property of the writer. The overtake scenario
# deliberately slows one render to a crawl, so it needs the more generous end.
OVERTAKE_DRAIN_QUIET_S=${OVERTAKE_DRAIN_QUIET_S:-8}
# A keystroke either produces a reaction or it does not; there is no slow path
# between the two. Scenarios that ask "did the guest react to this key" — and
# especially the ones that exist to prove it did NOT — use this shorter window,
# because waiting out the ordinary one would only be waiting.
REACT_QUIET_S=${REACT_QUIET_S:-20}
# The banner is not the prompt: a boot goes on printing after "BoxOS Shell".
# This is how long that burst may pause and still be the same burst — the same
# kind of number as OVERTAKE_DRAIN_QUIET_S, a property of the writer, not a
# guess at how long a boot takes. A log that has been quiet this long is a
# machine sitting at its prompt.
BOOT_BURST_QUIET_S=${BOOT_BURST_QUIET_S:-3}
# How many screendumps the paint scenario may take before it accepts what it
# sees. Not a deadline on the machine: `playtime` pours until it is stopped, so
# every extra shot can only find MORE of the screen painted, never less. The
# budget exists so that a machine which never paints at all still returns.
PAINT_SHOT_TRIES=${PAINT_SHOT_TRIES:-15}

# wait_for_line [-E] [-P PROGRESS] FILE PATTERN [QUIET_S] [FROM_LINE]
#
# Returns 0 the moment PATTERN appears in FILE — from line FROM_LINE on, if one
# is given, which is how a scenario asks about the window since it last looked.
# Returns 1 once FILE has stopped growing for QUIET_S seconds without it, and
# says which of the two ways it gave up.
#
# -E makes PATTERN an extended regular expression, exactly as `grep -E` does.
# The default is grep's basic syntax, because that is what the call sites that
# moved in here were written in, and an anchor silently reinterpreted is the
# failure this whole file exists to prevent.
#
# -P PROGRESS is the extended expression of the lines the machine says its
# progress by, the count on each the first number it carries; the runaway
# guard counts from a count past every one seen in the window. The default is
# the startup tests (STAND_TEST_BEGUN).
wait_for_line() {
    local _wf_flag="" _wf_said="$STAND_TEST_BEGUN"
    while : ; do
        case "$1" in
            -E) _wf_flag="-E"; shift ;;
            -P) [ $# -ge 2 ] || { harness "wait_for_line: -P names no progress to count"; return 1; }
                _wf_said="$2"; shift 2 ;;
            *) break ;;
        esac
    done
    local _wf_file="$1" _wf_pat="$2"
    local _wf_quiet="${3:-$WAIT_QUIET_S}" _wf_from="${4:-1}"
    local _wf_size _wf_last="" _wf_still=0 _wf_spent=0 _wf_most=0 _wf_top _wf_fell _wf_over
    while : ; do
        if [ -f "$_wf_file" ]; then
            if [ "$_wf_from" -gt 1 ]; then
                tail -n +"$_wf_from" "$_wf_file" 2>/dev/null \
                    | grep -q $_wf_flag -- "$_wf_pat" && return 0
            else
                grep -q $_wf_flag -- "$_wf_pat" "$_wf_file" 2>/dev/null && return 0
            fi
            _wf_size=$(wc -c < "$_wf_file" 2>/dev/null | tr -d ' ')
            # The progress said in this wait's window: the highest count, and
            # the first that came at or below the one before it (0: none). Read
            # as bytes (LC_ALL=C), as the typer reads the log: awk in the host's
            # UTF-8 locale stops at a byte that is not UTF-8 ("towc: multibyte
            # conversion failure", powercut race's boots of torn media), says
            # nothing on stdout, and the count it gave up was no count at all.
            read -r _wf_top _wf_fell _wf_over <<<"$(tail -n +"$_wf_from" "$_wf_file" 2>/dev/null |
                STAND_WF_PAT="$_wf_said" LC_ALL=C awk '$0 ~ ENVIRON["STAND_WF_PAT"] && match($0, /[0-9]+/) {
                    n = substr($0, RSTART, RLENGTH) + 0
                    if (!fell && n <= last) { fell = n; over = last }; last = n; if (n > top) top = n }
                    END { print top + 0, fell + 0, over + 0 }')"
            if [ "$_wf_fell" -eq 0 ] && [ "$_wf_top" -gt "$_wf_most" ]; then
                _wf_most=$_wf_top; _wf_spent=0
            fi
        else
            _wf_size=0
        fi
        if [ "$_wf_size" = "$_wf_last" ]; then
            _wf_still=$((_wf_still + 1))
            if [ "$_wf_still" -ge "$_wf_quiet" ]; then
                echo "  (waited for '$_wf_pat': nothing new in $_wf_file for ${_wf_quiet}s — giving up on a machine that has gone quiet)"
                return 1
            fi
        else
            _wf_still=0; _wf_last="$_wf_size"
        fi
        _wf_spent=$((_wf_spent + 1))
        if [ "$_wf_spent" -ge "$WAIT_RUNAWAY_S" ]; then
            if [ "${_wf_fell:-0}" -gt 0 ]; then
                echo "  (waited for '$_wf_pat': ${WAIT_RUNAWAY_S}s past the newest progress $_wf_file said, and never said it — its count fell back, $_wf_fell after $_wf_over: the machine began again — giving up on a machine that restarts)"
            elif [ "$_wf_most" -gt 0 ]; then
                echo "  (waited for '$_wf_pat': ${WAIT_RUNAWAY_S}s past the newest progress $_wf_file said, and never said it — $(tail -n +"$_wf_from" "$_wf_file" | grep -aE -- "$_wf_said" | tail -1 | tr -d '\r'), the log last grown ${_wf_still}s ago — giving up past the runaway guard)"
            else
                echo "  (waited for '$_wf_pat': $_wf_file said no progress in ${WAIT_RUNAWAY_S}s and never said it, the log last grown ${_wf_still}s ago — giving up past the runaway guard)"
            fi
            return 1
        fi
        sleep 1
    done
}

# wait_until QUIET_S WHAT TEST [FILE] — the same watchdog for a fact that is
# not one line: a COUNT of banners, a prompt read at the end of the file,
# either of two different sentences in two different places. TEST is a
# shell expression, written in this file and nowhere else, evaluated once a
# second until it succeeds. The locals here are prefixed so that a TEST may
# freely mention its caller's MARK, FIRST_LINES, i or t.
wait_until() {
    local _wu_quiet="$1" _wu_what="$2" _wu_test="$3" _wu_file="${4:-$WAIT_LOG}"
    local _wu_size _wu_last="" _wu_still=0 _wu_spent=0
    while : ; do
        eval "$_wu_test" && return 0
        if [ -f "$_wu_file" ]; then
            _wu_size=$(wc -c < "$_wu_file" 2>/dev/null | tr -d ' ')
        else
            _wu_size=0
        fi
        if [ "$_wu_size" = "$_wu_last" ]; then
            _wu_still=$((_wu_still + 1))
            if [ "$_wu_still" -ge "$_wu_quiet" ]; then
                echo "  (waited for $_wu_what: nothing new in $_wu_file for ${_wu_quiet}s — giving up on a machine that has gone quiet)"
                return 1
            fi
        else
            _wu_still=0; _wu_last="$_wu_size"
        fi
        _wu_spent=$((_wu_spent + 1))
        if [ "$_wu_spent" -ge "$WAIT_RUNAWAY_S" ]; then
            echo "  (waited for $_wu_what: $_wu_file grew for ${WAIT_RUNAWAY_S}s and never showed it — giving up on a machine that is still talking)"
            return 1
        fi
        sleep 1
    done
}

# wait_for_quiet SECONDS [FILE] — return once FILE has stopped growing for
# SECONDS. This is the "the burst is over" fact, and it is the other half of
# wait_for_line: some scenarios do not wait for a sentence, they wait for the
# machine to FINISH SAYING one. "Five seconds have passed" would say nothing
# about that; "the file has not changed in five seconds" says exactly it.
#
# ‼ Only ever useful AFTER a fact that proves the burst has begun. Called on a
# guest that has not started printing yet it returns at once and truthfully —
# nothing is growing — which is not what the caller meant.
wait_for_quiet() {
    local _wq_quiet="${1:-$WAIT_QUIET_S}" _wq_file="${2:-$WAIT_LOG}"
    local _wq_size _wq_last="" _wq_still=0 _wq_spent=0
    while : ; do
        if [ -f "$_wq_file" ]; then
            _wq_size=$(wc -c < "$_wq_file" 2>/dev/null | tr -d ' ')
        else
            _wq_size=0
        fi
        if [ "$_wq_size" = "$_wq_last" ]; then
            _wq_still=$((_wq_still + 1))
            [ "$_wq_still" -ge "$_wq_quiet" ] && return 0
        else
            _wq_still=0; _wq_last="$_wq_size"
        fi
        _wq_spent=$((_wq_spent + 1))
        if [ "$_wq_spent" -ge "$WAIT_RUNAWAY_S" ]; then
            echo "  (the log has grown without pause for ${WAIT_RUNAWAY_S}s — no burst ever ended)"
            return 1
        fi
        sleep 1
    done
}

# ===========================================================================
# A HARNESS THAT COULD NOT TYPE HAS NOT TESTED ANYTHING
# ===========================================================================
#
# qemu-input.sh fails for reasons that have nothing to do with the machine
# under test: the monitor socket is not there yet, the path to it is longer
# than sun_path allows, nc is missing, QEMU is already gone. A hundred and
# fifty call sites sent both its output and its exit status to /dev/null, so
# every one of those arrived dressed as "the machine never answered a
# keystroke" — this script blaming BoxOS for its own inability to speak.
#
# They are different faults and they are counted and printed differently now.
# A FAIL is a verdict on BoxOS. A HARNESS line is a verdict on this script and
# the desk it is running on, and it makes the whole run inconclusive, because
# a run that could not talk to the machine judged nothing.
HARNESS=0
harness() {
    printf '  \033[33mHARNESS\033[0m %s\n' "$1"
    HARNESS=$((HARNESS+1))
}

# The typer and the shell's prompt (type_cmd, key_press, mon_raw, mon_shot,
# prompt_at_end, wait_for_prompt, type_echoed, type_line): in their own file
# beside this one.
. "$ROOT/tools/stand_typer.sh"

# The sixteen bytes themselves, read off a raw image, so a claim about which
# volume something is stops being an assumption — read by the format's own
# reader on the host (tools/volumeread): the ground the partition table names
# and whose first sector is a Deed, the Deed checked whole against its own
# checksum, and its identity. Of either format: the prologue is the same in
# both. Says nothing and fails when there is no readable Deed there, which is
# the honest answer for a copy that came out short or torn.
deed_uuid() {
    python3 - "$1" <<'PY'
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, "tools")
from volumeread import medium, ondisk
try:
    m = medium.Medium(sys.argv[1])
    ground, _how = medium.find_ground(m, None)
    deed = ondisk.ReadDeed(m.sectors(ground, ondisk.DEED_SECTORS))
except (ValueError, IOError):
    sys.exit(1)
if not deed["ok"] or deed["role"] != ondisk.DEED_ROLE_HEAD:
    sys.exit(1)
print(deed["uuid"].hex())
PY
}

# The Caveat table, its one judge and stand_path (caveat_row, caveat_step,
# caveat_defect, caveat_judge, CAVEAT, stand_path): in their own file beside
# this one.
. "$ROOT/tools/stand_caveat.sh"
