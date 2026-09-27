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
# them the same way: the stand lock (one harness at a time on one SCRATCH),
# the Caveat judge with its one table, and stand_path.

: "${STAND_NAME:?stand.sh is sourced by a harness that names itself first — set STAND_NAME}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# ONE directory for every harness under one TMPDIR, and its name still says
# logcheck on purpose: the mutation manifest and the build-key record live
# here, and a run killed under one harness is put back by whichever harness
# runs next — which works only while they all read the same books. A job
# that must not share them runs under a TMPDIR of its own.
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
make() { mut_stamp_passed; command make HARNESS=on "$@"; }

build() {
    make >"$SCRATCH/build.log" 2>&1
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
# the middle of measuring; and both drive the same build/ — which was
# measured, see buildkey_restore: a second agent tore the image out from under
# a rebuild and the run still printed a green summary.
#
# So the stand is taken before anything reads the books, and it names who
# holds it: stand.lock/holder is "pid name". mkdir is the lock because it is
# the one filesystem operation that either makes the directory or fails,
# whoever else is asking in the same instant. A holder whose pid is gone —
# killed by a signal no trap sees, or a desk that went down under it — is
# taken over, and that is said. The takeover is itself a mkdir, inside the
# dead holder's directory, so two harnesses that find the same dead holder
# cannot both take it: the second is refused. What cannot be told apart is
# refused too, never guessed at: a lock that names nobody is one being taken
# at that very instant, or one whose taker died in the microseconds between
# its mkdir and its name — the line says which directory to remove if no
# harness is running.
stand_lock() {
    local lock="$SCRATCH/stand.lock" holder pid
    while ! mkdir "$lock" 2>/dev/null; do
        holder=$(cat "$lock/holder" 2>/dev/null) || holder=""
        pid=${holder%% *}
        case "$pid" in
            ''|*[!0-9]*)
                echo "$STAND_NAME: REFUSING TO RUN — $lock names no holder: a harness"
                echo "          is taking the stand this very instant, or was killed doing"
                echo "          so. If none is running under this TMPDIR, remove it."
                exit 1 ;;
        esac
        if kill -0 "$pid" 2>/dev/null; then
            echo "$STAND_NAME: REFUSING TO RUN — the stand is taken by pid $pid (${holder#* })."
            echo "          It keeps its mutation manifest in $SCRATCH, the books this"
            echo "          run would read. Run again once it is done, or under a TMPDIR"
            echo "          of your own."
            exit 1
        fi
        if ! mkdir "$lock/takeover" 2>/dev/null; then
            echo "$STAND_NAME: REFUSING TO RUN — pid $pid (${holder#* }) left the stand without"
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
        echo "$STAND_NAME: the stand was held by pid $pid (${holder#* }), which is gone — taking it over"
        rm -rf "$lock.gone.$$"
        mv "$lock" "$lock.gone.$$" || exit 1
        rm -rf "$lock.gone.$$"
    done
    printf '%s %s\n' "$$" "$STAND_NAME" > "$lock/holder" || exit 1
}

# Let go of the stand, and only if it is this run's to let go of. Moved aside
# before it is removed, so no harness ever finds it half deleted: a lock with
# no holder in it reads as one being taken, and is refused.
stand_unlock() {
    local lock="$SCRATCH/stand.lock"
    [ "$(cat "$lock/holder" 2>/dev/null)" = "$$ $STAND_NAME" ] || return 0
    rm -rf "$lock.gone.$$"
    mv "$lock" "$lock.gone.$$" 2>/dev/null && rm -rf "$lock.gone.$$"
}

stand_lock
# The way out until the exit trap below is set: a preflight that refuses must
# not leave the stand held by a pid that is about to be gone.
trap stand_unlock EXIT

# Three preflights, because the three failures they catch are different. The
# first is this mechanism's own bookkeeping: a manifest that outlived its run.
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
# tools/ is asked as well as src/: an oracle that proves a host tool edits
# that tool exactly as one that proves the kernel edits the kernel, and an
# edit left behind there is a tool that judges every later run with its
# defect put back. Four files in tools/ carry the two phrases as TEXT and are
# passed over by name: the two harnesses that write them into their mutations,
# this library that insists on them, and the plate that reads them. A harness
# added to tools/ is added to this list, and until it is this refuses to run
# and names it — the safe way to be wrong. -I because every mutation is an
# edit to a text source, and the one binary that carries the words is the
# plate's own compiled search for them.
MUT_LEFTOVER=$( { grep -rl -e "logcheck mutation" -e "logcheck injection" src
                  grep -rlI -e "logcheck mutation" -e "logcheck injection" \
                       --exclude=logcheck.sh --exclude=powercut.sh --exclude=stand.sh \
                       --exclude=buildplate.c tools
                } 2>/dev/null || true)
if [ -n "$MUT_LEFTOVER" ]; then
    echo "$STAND_NAME: REFUSING TO RUN — the source tree still carries a mutation:"
    printf '          %s\n' $MUT_LEFTOVER
    echo "          Nothing measured on this tree would mean anything. Put these"
    echo "          files back (git checkout, or the pristine copies under"
    echo "          $MUT_PRISTINE) and run again."
    exit 1
fi

# Preflight 3 — every mutation anchor still names code that is there.
#
# An anchor is a literal copied out of a source file. Rename that code, as a
# perfectly legitimate change may, and the anchor is orphaned. `mutate` and
# `mut_done` do catch it — that is what they are for — but only after a build
# and a boot, and only for whichever oracle happened to run; the other thirty
# keep their stale anchors until somebody reaches them. Measured 2026-09-12:
# `BrookCleanupProcess` was renamed to `BrookCleanupStrand` and `orderlifemut`
# was the only oracle that noticed, half an hour into a suite.
#
# The literals are right here, in the harness that sources this — "$0" is its
# path, and sourcing does not change it. Reading them all out and asking the
# tree costs a second and answers for every oracle at once.
python3 - "$0" "$STAND_NAME" <<'ANCHOR_EOF' || exit 1
import os, re, subprocess, sys
script = open(sys.argv[1]).read()
lits = re.findall(r'^\s*(?:a|anchor)\s*=\s*"((?:[^"\\]|\\.)*)"', script, re.M)
stale = []
for lit in sorted(set(lits)):
    text = lit.encode().decode('unicode_escape')
    if len(text.strip()) < 6:
        continue
    # --exclude the harness itself. Without it the search finds the anchor
    # inside the very line that declares it, every anchor looks alive, and the
    # check is a green light wired to nothing — which is worse than no check
    # at all.
    if subprocess.run(["grep", "-rlF",
                       "--exclude=" + os.path.basename(sys.argv[1]), text,
                       "src", "tools"], capture_output=True).returncode != 0:
        stale.append(text)
if stale:
    print("%s: REFUSING TO RUN — %d mutation anchor(s) name code that is"
          % (sys.argv[2], len(stale)))
    print("          no longer in the tree. The oracle that uses one cannot put")
    print("          its defect back, so its green half proves nothing. Fix the")
    print("          anchor; do not skip the oracle.")
    for s in stale:
        print("          stale: %r" % s[:100])
    sys.exit(1)
ANCHOR_EOF

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
    # And the stand last, once the books it guards are closed.
    stand_unlock
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
WAIT_LOG=build/serial.log
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

# wait_for_line [-E] FILE PATTERN [QUIET_S] [FROM_LINE]
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
wait_for_line() {
    local _wf_flag=""
    if [ "$1" = "-E" ]; then _wf_flag="-E"; shift; fi
    local _wf_file="$1" _wf_pat="$2"
    local _wf_quiet="${3:-$WAIT_QUIET_S}" _wf_from="${4:-1}"
    local _wf_size _wf_last="" _wf_still=0 _wf_spent=0
    while : ; do
        if [ -f "$_wf_file" ]; then
            if [ "$_wf_from" -gt 1 ]; then
                tail -n +"$_wf_from" "$_wf_file" 2>/dev/null \
                    | grep -q $_wf_flag -- "$_wf_pat" && return 0
            else
                grep -q $_wf_flag -- "$_wf_pat" "$_wf_file" 2>/dev/null && return 0
            fi
            _wf_size=$(wc -c < "$_wf_file" 2>/dev/null | tr -d ' ')
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
            echo "  (waited for '$_wf_pat': $_wf_file grew for ${WAIT_RUNAWAY_S}s and never said it — giving up on a machine that is still talking)"
            return 1
        fi
        sleep 1
    done
}

# wait_until QUIET_S WHAT TEST [FILE] — the same watchdog for a fact that is
# not one line: a COUNT of banners, a prompt read as the last two bytes of the
# file, either of two different sentences in two different places. TEST is a
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
qi_why() { tr '\n' ' ' < "$SCRATCH/qemu-input.err" 2>/dev/null; }

# ‼ A GUEST THAT WILL NOT ECHO IS NOT A HARNESS FAILURE, AND CALLING IT ONE
#   POINTS THE OPERATOR AT THE WRONG MACHINE.
#
# qemu-input.sh verifies the echo per character itself and gives up with
# "guest echoed N of M characters ... keystrokes are being dropped".
# Everything else it can fail with — no monitor socket, a socket path longer
# than sun_path, no nc, QEMU already gone — is this desk. That one sentence is
# the GUEST: a shell that takes characters and prints nothing is exactly what
# a controller whose rings nobody translates looks like, and it is a verdict
# on BoxOS.
#
# Measured, on a kernel with the write bit taken out of the identity map:
# three correct FAILs about the keyboard and the volume, and beside them a
# yellow HARNESS line saying the run "could not talk to the machine it was
# judging, so its verdicts are not evidence" — the opposite of what happened.
#
# OPT-IN per scenario, and off by default on purpose: a scenario that does not
# go on to judge the keystroke itself would otherwise have NO count for a
# guest that went deaf, and a run with nothing counted must never be green. A
# scenario turns this on exactly when a check of its own answers for it.
TYPE_SILENCE_IS_JUDGED=${TYPE_SILENCE_IS_JUDGED:-0}
type_cmd() {   # the characters of a command line; NOT the Enter after them
    ./tools/qemu-input.sh type "$1" >/dev/null 2>"$SCRATCH/qemu-input.err" && return 0
    if [ "$TYPE_SILENCE_IS_JUDGED" = 1 ] \
       && grep -q "guest echoed" "$SCRATCH/qemu-input.err" 2>/dev/null; then
        echo "  (the machine took '$1' and echoed nothing back: $(qi_why))"
        echo "   That is this kernel, not this desk. The keystroke check below is"
        echo "   where it is counted."
        return 1
    fi
    harness "could not type '$1' into the machine: $(qi_why)"
    return 1
}
key_press() {
    ./tools/qemu-input.sh key "$1" >/dev/null 2>"$SCRATCH/qemu-input.err" && return 0
    harness "could not send the key '$1': $(qi_why)"
    return 1
}
mon_raw() {
    ./tools/qemu-input.sh raw "$1" >/dev/null 2>"$SCRATCH/qemu-input.err" && return 0
    harness "the monitor would not take '$1': $(qi_why)"
    return 1
}
mon_shot() {
    ./tools/qemu-input.sh shot "$1" >/dev/null 2>"$SCRATCH/qemu-input.err" && return 0
    harness "could not screendump into '$1': $(qi_why)"
    return 1
}

# Wait until the shell is READY, which is not the same as the last command
# having printed its answer.
#
# ‼ A COMMAND THAT FINISHES IS NOT A SHELL THAT IS FREE. It still has to reap
# the child and put its own house in order, and that is reads — on a medium
# throttled to a hundred and twenty-eight bytes a second, tens of seconds of
# them. Throttling the medium in that window and typing into it is how four
# runs reported a machine that was simply busy as a machine that was broken.
#
# The prompt is two bytes, `~ ` with no newline after it, so the fact is the
# END OF THE FILE rather than a line in it — read as hex, because command
# substitution eats the trailing space that is half the evidence.
wait_for_prompt() {
    # The quiet window is the SLOW one on purpose: every caller of this has
    # throttled the medium first, and a shell reading at 64 KiB/s is honestly
    # silent for a long time while it is working. Silence is the giving-up
    # fact, so it has to be long enough to mean what it says.
    wait_until "$WAIT_QUIET_SLOW_S" "the shell's prompt at the end of the log" \
        '[ "$(tail -c 2 build/serial.log 2>/dev/null | xxd -p)" = "7e20" ] &&
          { sleep 2; [ "$(tail -c 2 build/serial.log 2>/dev/null | xxd -p)" = "7e20" ]; }' \
        || { echo "  (the shell never came back to its prompt)"; return 1; }
    return 0
}

# Type a command line and make sure the machine TOOK it.
#
# ‼ A KEYSTROKE THAT DOES NOT LAND IS SILENT, AND THAT COST FOUR RUNS.
#
# `qemu-input.sh type` talks to the monitor, and a monitor command that fails
# says nothing at all on the guest's serial line — the replug scenario already
# has that written down for `drive_add`. The same is true of the keystrokes
# themselves: type into a shell that is still working through the last command
# and the characters go nowhere, the scenario waits out its ceiling, and it
# reports a machine that never had a chance as a machine that failed.
#
# The shell echoes what it was given, so the echo is the fact that it landed.
# Tried twice, and said out loud if it still did not, because a scenario that
# quietly tests nothing is worse than one that fails.
# A shell standing at its prompt echoes what it is given at once, so a log that
# has gone quiet WITHOUT the echo is a shell that never got the keystrokes —
# and that is worth one more try. A log still growing is a shell still working
# through the last command, and the wait simply carries on.
TYPE_ECHO_QUIET_S=${TYPE_ECHO_QUIET_S:-10}
type_line() {
    local cmd="$1" attempt=0
    while [ $attempt -lt 2 ]; do
        if type_cmd "$cmd"; then
            sleep 1
            key_press ret
            wait_for_line build/serial.log "~ $cmd" "$TYPE_ECHO_QUIET_S" && return 0
        fi
        attempt=$((attempt+1))
    done
    echo "  (the machine never echoed '$cmd' — the keystrokes did not land)"
    return 1
}

# The sixteen bytes themselves, read off a raw image, so a claim about which
# volume something is stops being an assumption. The Deed sits at the head of
# the volume's ground; the ground is MBR entry 0, which is the one the image
# rule writes first (`make_mbr.py $@ $$START,$$GROUND,7f,boot ...`); and the
# uuid is at offset 16 of the Deed — the offset src/kernel/tagfs/deed/deed.h
# pins with a STATIC_ASSERT and stage2 reads by hand. Says nothing and fails
# when there is no readable Deed there, which is the honest answer for a copy
# that came out short or torn.
deed_uuid() {
    python3 - "$1" <<'PY'
import sys
with open(sys.argv[1], "rb") as f:
    f.seek(446 + 8)                      # MBR entry 0, its first LBA
    lba = int.from_bytes(f.read(4), "little")
    if lba == 0:
        sys.exit(1)
    f.seek(lba * 512)
    deed = f.read(32)
if len(deed) < 32 or deed[:8] != b"BOXDEED\0":
    sys.exit(1)
print(deed[16:32].hex())
PY
}

# ===========================================================================
# A KNOWN DEFECT, REPRODUCED ON PURPOSE, IS HELD — NOT PASSED, NOT SKIPPED
# ===========================================================================
#
# A test written for a defect that a later step of the plan discharges has two
# honest answers before that step: the defect was SEEN, with a number measured
# at run time, or the test is broken. Counting the first as a FAIL keeps every
# run red until the step lands and teaches everyone to stop reading the
# colour; counting it as a PASS, or skipping it, lets the test rot unwatched,
# and a test that has quietly stopped seeing its defect is then green on the
# day the step claims to have fixed it.
#
# So a test OBSERVES and this JUDGES, in the same words as the kernel's own
# startup tests. The observation is one of three: `defect` (the named defect,
# seen positively), `behaves` (the correct behaviour, seen), or `fail` (anything
# else — a broken test must never hide behind a caveat). A caveated test that
# behaves is a FAIL as well: it does not test what it claims, or its step has
# landed and the caveat has to go, which is deleting its row below in that
# step's commit. Held caveats are counted in CAVEAT and never change a run's
# exit status.
CAVEAT=0

# THE table, and the only one: NAME -> "STEP|DEFECT". A row names a defect
# found red on HEAD, never one that might be — race gets its row only once it
# is measured red — and it is lifted by deleting it, in the commit of the step
# it names. A harness adds the rows of its own tests here.
caveat_row() {
    case "$1" in
        longquery)  echo "1.1b|a query of 256 bytes or more is dropped unread, and the answer is every file in scope" ;;
        bigwrite)   echo "1.3|a write is copied through one kernel-heap allocation of its whole size, so one bigger than the heap is refused and nothing reaches the file" ;;
        sequential) echo "1.3|a file's record takes one extent per 4 KiB written and never merges them, so the 4080-byte record refuses the write at about 2.6 MiB" ;;
        interleave) echo "1.3|a file's record takes one extent per 4 KiB written and never merges them, so the 4080-byte record refuses the write at about 2.6 MiB" ;;
        usememory)  echo "1.6|a Use Context longer than 432 bytes is forgotten, remembered=0" ;;
        fidhistory) echo "1.3b|a mount spends kernel heap on every file id ever issued, the dead ones included" ;;
        systemtag)  echo "1.4|any program may take the system tag off kernel.bin and then delete it — the kernel the volume boots is gone" ;;
        noanchor)   echo "1.7|a file written without anchor has no name after a power cut" ;;
        nextfid)    echo "1.6|a table written ahead of its ledger hands the same file id out twice" ;;
        hog)        echo "1.7|on one core under a CPU hog nothing settles a write before the power goes" ;;
        settlecut)  echo "1.6|a table written ahead of its ledger hands the same file id out twice" ;;
    esac
}

# caveat_step NAME / caveat_defect NAME — the two halves of NAME's row; both
# empty for a test that carries no caveat.
caveat_step()   { local row; row=$(caveat_row "$1"); echo "${row%%|*}"; }
caveat_defect() { local row; row=$(caveat_row "$1"); echo "${row#*|}"; }

# caveat_judge NAME OBSERVED WHAT — the one verdict. WHAT carries the numbers
# the run measured, and for a test that writes it ends with " [path: …]" from
# stand_path, because a verdict about a write is a verdict about one path.
caveat_judge() {
    local name=$1 seen=$2 what=$3 step defect
    case "$seen" in
        defect|behaves|fail) ;;
        *) harness "caveat_judge $name: '$seen' is not an observation — defect, behaves or fail ($what)"
           return ;;
    esac
    step=$(caveat_step "$name")
    defect=$(caveat_defect "$name")
    if [ -n "$step" ]; then
        case "$seen" in
            defect)
                printf '  \033[36mCAVEAT\033[0m caveat held: %s — %s — discharged by step %s (%s)\n' \
                       "$name" "$defect" "$step" "$what"
                CAVEAT=$((CAVEAT+1)) ;;
            behaves)
                bad "caveat lifted early: $name — the test does not test what it claims, or step $step already landed — lift the caveat ($what)" ;;
            fail)
                bad "$name: $what" ;;
        esac
        return
    fi
    case "$seen" in
        behaves) ok "$name: $what" ;;
        defect)  bad "$name failed: the defect it stands for is back — $what" ;;
        fail)    bad "$name: $what" ;;
    esac
}

# stand_path LOG CORES — the way a userspace write went down to the medium on
# the machine that wrote LOG, booted with CORES cores. A red test that writes
# is evidence about ONE path: the same test takes the legacy channel
# synchronously on one machine and the AHCI WriteJob on another, and a verdict
# that does not say which cannot be compared with the next one. Read off the
# kernel's own mount line ("[TagFS] volume on seat N: <seat>", tagfs.c) and
# ObjWrite's own fork (storage_ops.c: AHCI, more than one core and a writer in
# userspace go through the WriteJob; everything else writes synchronously).
# Prints one of AHCI async (WriteJob) | AHCI sync | ATA sync | USB sync. A log
# that mounted nothing, or a seat this does not know, prints what it found
# instead and answers 1, so the path in a verdict is never a guess.
stand_path() {
    local log=$1 cores=$2 seat
    case "$cores" in
        ''|*[!0-9]*) echo "no path: '$cores' is not a number of cores"; return 1 ;;
    esac
    [ -f "$log" ] || { echo "no path: there is no log at $log"; return 1; }
    seat=$(tr -d '\r' < "$log" | sed -nE 's/^\[TagFS\] volume on seat [0-9]+: (.*)$/\1/p' | tail -1)
    case "$seat" in
        "AHCI port "*)
            if [ "$cores" -gt 1 ]; then echo "AHCI async (WriteJob)"; else echo "AHCI sync"; fi ;;
        ATA*) echo "ATA sync" ;;
        usb*) echo "USB sync" ;;
        "")   echo "no path: $log mounted no volume"; return 1 ;;
        *)    echo "no path: seat '$seat' is none this knows"; return 1 ;;
    esac
}
