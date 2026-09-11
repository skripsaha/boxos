#!/bin/bash
# logcheck — oracle for the Logbook split (kernel occurrence vocabulary vs
# volume tag registry).
#
#   logcheck.sh healthy   boot with a mounted volume; the volume's vocabulary
#                         must be untouched and the kernel's must be complete
#   logcheck.sh novolume  boot with tagfs_recognise() forced false — the board
#                         failure reproduced on the desk. The kernel's
#                         vocabulary must survive having no medium at all.
#   logcheck.sh yank      the stick is pulled WHILE the volume is being read.
#                         The medium is throttled so the window exists at all
#   logcheck.sh twoctrl   two host controllers, stick on the SECOND — the shape
#                         the board has and no scenario could be until now
#   logcheck.sh logsave   the machine writes down what it said. Two kernels:
#                         one built PRINTTOFILE=on, which must produce a
#                         readable account of its own boot on the volume, and
#                         one without, which must SAY it keeps none
#   logcheck.sh both
#
# Every check is a grep against build/serial.log. No timing assertions.

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCRATCH="${TMPDIR:-/tmp}/boxos-logcheck"
mkdir -p "$SCRATCH"
cd "$ROOT" || exit 1

PASS=0; FAIL=0
ok()   { printf '  \033[32mPASS\033[0m %s\n' "$1"; PASS=$((PASS+1)); }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1"; FAIL=$((FAIL+1)); }
chk()  { if [ "$1" = 0 ]; then ok "$2"; else bad "$2"; fi }

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

# make(1) compares whole-second mtimes, so a file rewritten inside the same
# second as the object built from it does not rebuild at all. What stood here
# was `sleep 1` before every touch: a second of wall clock per mutation, and a
# guess about a filesystem's timestamp granularity dressed as a fact. Instead
# the file is given an mtime strictly AHEAD of anything that could have been
# built from it. That is exact, it costs nothing, and it cannot be wrong.
#
# The stamp is five seconds into the FUTURE on purpose, and nothing waits for
# it. The whole price is this: if the object built from the file happens to be
# written inside those five seconds, the next make rebuilds that ONE file a
# second time. One file, once. Nothing else in this tree reads the stamp —
# git compares content whenever an index entry looks racily clean, so `git
# status` tells the truth either way, and the mutation is off the tree again
# long before anybody runs it.
mut_bump() {
    local when
    when=$(date -v+5S +%Y%m%d%H%M.%S 2>/dev/null) \
        || when=$(date -d '+5 seconds' +%Y%m%d%H%M.%S 2>/dev/null) \
        || { echo "mutate: no usable date(1)"; exit 1; }
    touch -t "$when" "$1"
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
restore() {
    local f="$1"
    if [ -f "$MUT_DISCARD/$f" ]; then
        rm -f "$f" "$MUT_DISCARD/$f"
        mut_forget "$f"
        return 0
    fi
    if [ ! -f "$MUT_PRISTINE/$f" ]; then
        mut_forget "$f"
        return 0
    fi
    cp -p "$MUT_PRISTINE/$f" "$f" || exit 1
    rm -f "$MUT_PRISTINE/$f"
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

# Two preflights, because the two failures they catch are different. The first
# is this mechanism's own bookkeeping: a manifest that outlived its run. The
# second needs no bookkeeping at all and catches what the OLD pairs left
# behind — every injection and mutation in this file writes a comment carrying
# one of these two words, so the tree can simply be asked whether it holds one.
# A tree that does is not a tree any measurement may be taken on.
if [ -s "$MUT_MANIFEST" ]; then
    echo "logcheck: a previous run was killed with the tree mutated:"
    sed 's/^/          /' "$MUT_MANIFEST"
    restore_all
    echo "logcheck: the tree has been put back."
fi
MUT_LEFTOVER=$(grep -rl -e "logcheck mutation" -e "logcheck injection" src 2>/dev/null || true)
if [ -n "$MUT_LEFTOVER" ]; then
    echo "logcheck: REFUSING TO RUN — the source tree still carries a mutation:"
    printf '          %s\n' $MUT_LEFTOVER
    echo "          Nothing measured on this tree would mean anything. Put these"
    echo "          files back (git checkout, or the pristine copies under"
    echo "          $MUT_PRISTINE) and run again."
    exit 1
fi

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
LOGCHECK_FINISHED=0
on_exit() {
    local rc=$?
    restore_all
    if [ "$LOGCHECK_FINISHED" = 0 ] && [ "$rc" = 0 ]; then
        echo "logcheck: the run ended before its summary. The tree has been put"
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

type_cmd() {   # the characters of a command line; NOT the Enter after them
    ./tools/qemu-input.sh type "$1" >/dev/null 2>"$SCRATCH/qemu-input.err" && return 0
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

# Waits for the SHELL BANNER, not for "userspace is starting".
#
# Those are not the same moment and the gap is not small: a machine with no
# display daemon spends five seconds asking for one before it gives up and
# writes the screen itself, so sampling at "Starting userspace" reports a
# machine that never reached a shell when it reaches one shortly after.
# Measured — it cost a wrong FAIL here first.
boot() {
    make run-stop >/dev/null 2>&1
    make run-bg   >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3


    # Type into it. A machine you can look at and not talk to is not a machine
    # that booted — and "can it be typed on" is the only question the board
    # failure was ever really about.
    type_cmd "help"
    sleep 1
    key_press ret
    sleep 3

    # And then an EXTERNAL utility, which is a different question entirely.
    #
    # Every scenario in this file typed "help" and nothing else, and "help" is
    # a builtin — it never leaves the shell. So none of them could reach the
    # path where the shell spawns a program and waits for it to die, and that
    # path was broken for as long as the Logbook split has existed: the shell
    # parked on a tag nothing published to, and the prompt never came back.
    # Five green scenarios, and the machine was unusable from its second
    # command onward.
    type_cmd "hw"
    sleep 1
    key_press ret
    sleep 4

    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.$1.log"
}

# Did the machine answer a keystroke? The shell prints its command list in
# response to "help", so one line out of that list is the proof.
typed_ok() {
    grep -q "Show available commands" "$1"
}

# Did the shell come BACK after running a program?
#
# The prompt is printed by every pass of the main loop, so a prompt appearing
# after the utility's own output is the proof that the loop went round. A
# machine that runs one command and then ignores the keyboard for ever looks,
# from a photograph, exactly like a machine that is working.
external_ok() {
    sed -n '/CPU features/,$p' "$1" | grep -qE '^~ ?$|^~ '
}

# The probe reports every resolve and which book answered. It is installed
# only for the run and removed after, so the shipped tree never carries it.
probe_on() {
    mutate src/kernel/core/touch/logbook.c
    python3 - <<'EOF'
p = "src/kernel/core/touch/logbook.c"
s = open(p).read()
anchor = """void TouchLogbookResolve(const char *tag, TouchTag *out_full, TouchTag *out_bare)
{
    logbook_resolve(tag, out_full, out_bare, true);
}"""
probed = """void TouchLogbookResolve(const char *tag, TouchTag *out_full, TouchTag *out_bare)
{
    logbook_resolve(tag, out_full, out_bare, true);
    /* logcheck injection: the probe says which book answered each resolve */
    kprintf("[PROBE] logbook '%s' full=0x%x bare=0x%x\\n", tag, *out_full, *out_bare);
}"""
assert anchor in s, "probe anchor missing"
open(p, "w").write(s.replace(anchor, probed))
EOF
    mut_done src/kernel/core/touch/logbook.c "logcheck injection"
}
probe_off() {
    restore src/kernel/core/touch/logbook.c
}

mutate_on() {   # reproduce "no volume on this machine"
    mutate src/kernel/tagfs/tagfs.c
    python3 - <<'EOF'
p = "src/kernel/tagfs/tagfs.c"
s = open(p).read()
anchor = "static bool tagfs_recognise(void *ctx, uint8_t seat, uint8_t out_uuid[16])\n{\n"
assert anchor in s, "mutation anchor missing"
s = s.replace(anchor, anchor + "    return false;   /* logcheck mutation: this machine mounted nothing */\n", 1)
open(p, "w").write(s)
EOF
    mut_done src/kernel/tagfs/tagfs.c "logcheck mutation"
}
mutate_off() {
    restore src/kernel/tagfs/tagfs.c
}

# The boarding pass names a volume that is not in the room, while another
# perfectly good TagFS volume is. This is the case that used to mount the
# stranger in silence and then ignore the real medium for the rest of the boot.
stranger_on() {
    mutate src/kernel/core/boarding/boarding.c
    python3 - <<'EOF'
p = "src/kernel/core/boarding/boarding.c"
s = open(p).read()
anchor = """    if (out_uuid) {
        memcpy(out_uuid, v->uuid, 16);
    }
    return true;
}"""
mutated = """    if (out_uuid) {
        memcpy(out_uuid, v->uuid, 16);
        out_uuid[0] ^= 0xFF;   /* logcheck mutation: a pass naming an absent volume */
    }
    return true;
}"""
assert anchor in s, "stranger anchor missing"
open(p, "w").write(s.replace(anchor, mutated, 1))
EOF
    mut_done src/kernel/core/boarding/boarding.c "logcheck mutation"
}
stranger_off() {
    restore src/kernel/core/boarding/boarding.c
}

# The volume turns up AFTER the boot gave up on it — a stick pushed in while
# the machine is running. Mutation: the internal disk is declared not to be
# this machine's, so the boot finds nothing; then a USB stick carrying the real
# image is hot-plugged through the QEMU monitor and the whole chain has to run
# by itself — enumerate, seat, recognise by boarding pass, mount, start the
# volume's programs, and relieve the stand-in shell.
#
# Four cores, because that chain is driven from the K-Core guide loop. On a
# single core it does not run at all — see the note at the end of this file.
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

latearrival_on() {
    mutate src/kernel/tagfs/tagfs.c
    python3 - <<'EOF'
p = "src/kernel/tagfs/tagfs.c"
s = open(p).read()
anchor = "static bool tagfs_recognise(void *ctx, uint8_t seat, uint8_t out_uuid[16])\n{\n"
assert anchor in s, "late-arrival anchor missing"
s = s.replace(anchor, anchor + "    if (BoardroomSeatKind(seat) == BOARD_ATA) return false;   /* logcheck mutation: the internal disk is not this machine's */\n", 1)
open(p, "w").write(s)
EOF
    mut_done src/kernel/tagfs/tagfs.c "logcheck mutation"
}
latearrival_off() {
    restore src/kernel/tagfs/tagfs.c
}

run_latearrival() {
    echo "== latearrival: the volume arrives after the boot gave up =="
    # The mutation stays installed until the run is over: `make run-bg` has the
    # image as a prerequisite and will rebuild it, so restoring the source
    # first quietly boots an unmutated kernel. Cost an entire run to learn.
    latearrival_on; build
    cp build/boxos.img "$SCRATCH/stick.img"

    make run-stop >/dev/null 2>&1
    make run-bg USB=on CORES=4 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"

    mon_raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw"
    sleep 1
    mon_raw "device_add usb-storage,drive=stick,id=usbstick"

    #
    # Waited out to the END of the chain.
    #
    # This used to break on the FIRST "AUTOSTART Started" — which is
    # display.elf — and then stop the machine two seconds later. Reading the
    # rest of the volume's programs off a USB stick takes longer than that
    # often enough that the two checks below were passing on luck: the machine
    # was killed mid-launch and the log was then searched for a line it had
    # not had time to print. Measured — it went red on a tree whose kernel had
    # nothing wrong with it.
    wait_for_line build/serial.log "hands over" "$WAIT_QUIET_S"
    sleep 2

    #
    # ‼ IF IT DID NOT HAND OVER, FIND OUT WHETHER THE MACHINE IS STILL ALIVE.
    #
    # Measured 2026-08-29: this scenario failed once, inside a forty-minute
    # matrix, with the log ending at `[AUTOSTART] Started 'display.elf'` and
    # nothing after it for sixty seconds. Standing alone it then passed three
    # times on that tree and three times on the tree before it — so it is not a
    # regression, and it is not reproducible on demand either.
    #
    # What was missing was not another run. It was the one fact that separates
    # the two things this can be: a machine WEDGED between two autostart
    # launches, and a machine perfectly alive and merely slow reading a
    # 772 KB program off a stick. The stand-in shell is still on the keyboard
    # at that moment, so asking it is free — and a `help` that answers proves
    # the cores are running, which moves the hunt onto the volume read and off
    # the scheduler entirely.
    #
    # Costs four seconds on a run that fails and nothing at all on one that
    # does not.
    if ! grep -q "hands over" build/serial.log 2>/dev/null; then
        echo "-- it did not hand over; asking whether the machine is alive --"
        type_cmd "help"
        sleep 1
        key_press ret
        sleep 3
    fi

    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.latearrival.log"
    latearrival_off
    L="$SCRATCH/serial.latearrival.log"

    grep -q "no volume yet" "$L";      chk $? "the boot found no volume of its own"
    grep -q "Fallback shell ready" "$L"; chk $? "and stood a shell in for one"
    grep -q "mass storage" "$L";       chk $? "the stick enumerated after the boot"
    grep -q "arrived after the room was called to order\|seat 1:" "$L"
    chk $? "the room seated it"

    # ‼ AND IT IS ASKED WHAT IT TAKES, and answers as itself rather than as the
    # slowest medium in the machine. A flash drive swallows its whole bounce
    # buffer in one command, so its neighbouring blocks are fetched together —
    # measured on this path: 216 commands to mount and start a volume became
    # 66, and the wall time from plugging in to a shell halved.
    # 128 is this drive's bounce buffer in sectors (MSD_BOUNCE_BYTES / 512) —
    # a number that comes from the DEVICE. Named exactly rather than loosely,
    # so that going back to one figure imposed on every medium reddens this
    # whichever figure is chosen.
    grep -q "this medium takes 128 sector(s) at a time, so neighbouring blocks are read 4 at a time" "$L"
    chk $? "the flash drive is asked what IT takes, and answers as itself"
    grep -q "seat 1 carries the volume this kernel was read out of" "$L"
    chk $? "and recognised it by the boarding pass, not by a rule"
    grep -q "a medium arrived carrying a volume, and this machine had none" "$L"
    chk $? "TagFS mounted it"
    grep -q "AUTOSTART. Started .display.elf" "$L"; chk $? "the volume's display daemon started"
    grep -q "AUTOSTART. Started .shell.bin" "$L";   chk $? "the volume's shell started"
    grep -q "the stand-in .PID 1. hands over" "$L"; chk $? "and the stand-in handed over"

    # Only meaningful when the line above went red — and then it is the whole
    # of what the next session needs. Not a check: a fact, printed.
    if ! grep -q "the stand-in .PID 1. hands over" "$L"; then
        if typed_ok "$L"; then
            echo "       the machine was ALIVE and answering the keyboard"
            echo "       => the stall is in reading the volume, not in the scheduler"
        else
            echo "       the machine did NOT answer the keyboard"
            echo "       => it was wedged, and the last line it printed says where"
        fi
        echo "       last line: $(tail -1 "$L")"
    fi
}

# ── replug: the stick is pulled out and pushed back in ─────────────────────
#
# The scenario every other one here missed, and the one the board actually
# failed on. Each of the five existing scenarios plugs the stick in exactly
# ONCE, so none of them could ever reach the code that decides what to do with
# a medium arriving in a room that has already seen it.
#
# What went wrong: a seat could never be emptied, so the departing stick left
# its chair behind occupied. It got its unit number straight back on return,
# the room found the chair already holding that number, added nothing, and
# therefore announced nothing — and the volume was never mounted again. On a
# machine that boots from a stick, that is the machine gone until it is
# switched off and on.
#
# Four cores, same as latearrival: the departure is taken down from the idle
# and guide loops, and on one core neither of them runs.
run_replug() {
    echo "== replug: the volume leaves and comes home to the same chair =="
    latearrival_on; build
    cp build/boxos.img "$SCRATCH/stick.img"

    make run-stop >/dev/null 2>&1
    make run-bg USB=on CORES=4 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"

    # First arrival — the same one latearrival proves.
    mon_raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw"
    sleep 1
    mon_raw "device_add usb-storage,drive=stick,id=usbstick"
    # Waited out to the END of the chain, not to its first sign. The stand-in
    # handing over is the last thing the first arrival does; pulling the stick
    # before then tests something else entirely — a medium yanked out from
    # under a program being loaded — and reports it as this.
    wait_for_line build/serial.log "hands over" "$WAIT_QUIET_S"
    sleep 2
    # A mark in the log, so the second half is read apart from the first: every
    # line the checks below care about has a twin above it.
    local FIRST_LINES
    FIRST_LINES=$(wc -l < build/serial.log)

    # Pulled out.
    mon_raw "device_del usbstick"
    wait_for_line build/serial.log "is empty" "$WAIT_QUIET_S"
    sleep 2

    # And now, with the medium out, ask for a program that IS on it and one
    # that is not. Those are different facts and used to print the same
    # sentence — on a live board the machine told the user that a command
    # which exists does not exist, two lines under the kernel saying why.
    type_cmd "files"
    sleep 1; key_press ret; sleep 3
    type_cmd "nosuchthing"
    sleep 1; key_press ret; sleep 3

    # And pushed back in — the same medium, into the machine it left.
    #
    # The drive is added again first. A drive created through the HMP
    # `drive_add` is auto-delete: QEMU takes it away with the device that was
    # using it, so the second `device_add` referred to a drive that no longer
    # existed and did nothing at all. It cost a run to find, because a monitor
    # command that fails is silent on the guest's serial line.
    mon_raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw"
    sleep 1
    mon_raw "device_add usb-storage,drive=stick,id=usbstick"
    wait_for_line build/serial.log "the volume is back" "$WAIT_QUIET_S"
    sleep 3

    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.replug.log"
    latearrival_off
    L="$SCRATCH/serial.replug.log"
    # Everything after the first arrival. The checks are about the SECOND one.
    tail -n +$((FIRST_LINES + 1)) "$L" > "$SCRATCH/serial.replug.second.log"
    local S="$SCRATCH/serial.replug.second.log"

    # The first arrival happened at all — otherwise the rest is vacuous.
    grep -q "a medium arrived carrying a volume, and this machine had none" "$L"
    chk $? "the stick was mounted on its first arrival"
    grep -q "AUTOSTART. Started .shell.bin" "$L"; chk $? "and its shell started"
    grep -q "the stand-in .PID 1. hands over" "$L"; chk $? "and the stand-in handed over"

    # The departure NAMES the chair. Saying only "something left" is what made
    # the room unable to empty the right one.
    grep -qE "seat [0-9]+ is empty . usb[0-9]+" "$S"
    chk $? "the room emptied the chair and named it"
    grep -q "the medium the volume lives on has left" "$S"
    chk $? "TagFS noticed the medium go"

    # The second arrival is announced. This is the whole repair: it used not to
    # be, because the room only ever spoke when it GREW.
    grep -q "arrived after the room was called to order" "$S"
    chk $? "the return was announced, not swallowed"
    grep -q "the volume is back, in seat" "$S"
    chk $? "TagFS took the volume back up"

    # And FINISHED doing so. "The volume is back" is said before the mount is
    # attempted; it is the return starting, not the return working.
    grep -q "\[TagFS\] the volume on seat .* is MOUNTED" "$S"
    chk $? "and the second mount finished, rather than only beginning"

    # Into the chair it left, not a new one. The room must not grow a chair
    # per plug cycle — sixteen of them is what a live board showed.
    grep -q "seat 2:" "$S"
    if [ $? -ne 0 ]; then ok "no new chair was added for the returning stick"
    else bad "no new chair was added for the returning stick"; fi
    grep -qE "the volume is back, in seat 1" "$S"
    chk $? "and it is the same seat number it had before"

    # A program that is on the absent volume, and one that is on no volume at
    # all, must not get the same sentence.
    grep -q "files: is there and would not start" "$S"
    chk $? "a real command on an absent medium says so"
    grep -q "Unknown command: nosuchthing" "$S"
    chk $? "and a command that does not exist is still unknown"

    # ONE stick, ONE unit number, for the whole run.
    #
    # Two cores could both attach the same disk — the "is it already attached"
    # walk was outside the lock that the answer depends on — and only one of
    # the two units is ever unlinked when the device leaves. The other keeps
    # its number for the rest of the boot, so the next stick gets a higher one,
    # and a higher number is a chair nobody is sitting in. Measured on a live
    # board: one flash drive, seats usb0, usb1 and usb2.
    local units
    units=$(grep -oE "USB disk [0-9]+\] usb[0-9]+ " "$L" | sort -u | wc -l | tr -d ' ')
    [ "$units" = 1 ]; chk $? "one stick was one disk throughout ($units named)"

    # And the machine is still usable afterwards, which is the point.
    grep -q "BoxOS Shell" "$L"; chk $? "the machine still has a shell"
}

# ── nofsgsbase: a processor that cannot write its own TLS base ─────────────
#
# Every configuration in this file runs with +fsgsbase, so ring 3 installs its
# own FS base with WRFSBASE and the kernel's fallback is never exercised. That
# fallback was broken for as long as it has existed, and it takes out EVERY
# C++ binary on such a machine — measured on a Braswell laptop, reproduced here
# byte for byte. Broadwell brought FSGSBASE to the Core line and Goldmont to
# Atom; everything older is this configuration, and BoxOS is meant to run on
# machines people already own.
run_nofsgsbase() {
    echo "== nofsgsbase: the CPU cannot write its own TLS base =="
    build
    make run-stop >/dev/null 2>&1
    make run-bg FSGSBASE=off CORES=4 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3
    for c in timezone me; do
        type_cmd "$c"
        sleep 1; key_press ret; sleep 4
    done
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.nofsgsbase.log"
    L="$SCRATCH/serial.nofsgsbase.log"

    grep -q "BoxOS Shell" "$L"; chk $? "boot reaches the shell without fsgsbase"

    # The C++ runtime installs its TLS through the kernel here, and the first
    # fs-relative instruction is two after the request. Nothing may fault.
    ! grep -q "__boxcxx_tls_bootstrap" "$L"
    chk $? "no C++ program died in its TLS bootstrap"
    ! grep -q "EXCEPTION" "$L"; chk $? "no user-mode exception at all"

    grep -q "timezone: not set\|now:" "$L"
    chk $? "a C++ utility ran and printed its answer"
    grep -q "AMP active\|Uptime:" "$L"
    chk $? "and so did the one after it"
}

run_stranger() {
    echo "== stranger: the pass names a volume that is not here =="
    stranger_on; build; boot stranger; stranger_off
    L="$SCRATCH/serial.stranger.log"

    grep -q "no seat is carrying it" "$L"; chk $? "the room says the volume it wants is absent"
    grep -q "none of them is this machine.s . mounting nothing" "$L"
    chk $? "and declines the volume that IS here"

    ! grep -q "TagFS. volume on seat" "$L"; chk $? "no stranger was mounted"
    grep -q "Storage Deck. no volume yet" "$L"; chk $? "the deck says it has no volume"

    # The whole point of declining: the machine must still be usable, because
    # the person who has to re-seat the medium types on it.
    grep -q "BoxOS Shell" "$L"; chk $? "the machine still reaches a shell"
    typed_ok "$L"; chk $? "and answers a keystroke"

    # And it must be LISTENING, not polling on a clock.
    ! grep -q "falling back to the rule" "$L"; chk $? "the guess-rule never ran"
}

# Occurrence names that measurement showed were resolving to INVALID before
# the split, because they are named before any volume mounts.
DEAD_BEFORE="mce:fault:detected mce:fault:fatal cet:fault:cp acpi:ready aml:ready \
display:ready mce:migration:completed apei:memory:error apei:ghes:ready"

run_healthy() {
    echo "== healthy: volume mounted =="
    probe_on; build; boot healthy; probe_off
    L="$SCRATCH/serial.healthy.log"

    grep -q "BoxOS Shell" "$L"; chk $? "boot reaches the shell"
    typed_ok "$L"; chk $? "and answers a keystroke"

    # ‼ THE MOUNT FINISHED, WHICH IS NOT WHAT ANY OTHER LINE HERE SAYS.
    #
    # Everything TagFS prints while mounting — the deed, the far copy, the
    # file count — is printed just as readily by a mount that then fails eight
    # steps later. On the owner's board that is exactly what happened, twice,
    # and it was read off a photograph as a healthy volume while the shell
    # answered `Unknown command` to every file on it. One line closes it.
    grep -q "\[TagFS\] the volume on seat .* is MOUNTED" "$L"
    chk $? "and the volume actually finished mounting"
    ! grep -q "\[TagFS\] this volume is NOT mounted" "$L"
    chk $? "with nothing refused along the way"

    # Running a PROGRAM, and coming back. A builtin never leaves the shell;
    # this is the only check in the file that exercises spawn-and-wait.
    grep -q "CPU features" "$L"; chk $? "an external utility ran"
    external_ok "$L"; chk $? "and the prompt came back after it"

    # (1) every occurrence lands in the kernel half — id has bit 15 set
    local bad_ids
    bad_ids=$(grep '^\[PROBE\] logbook' "$L" | grep -v 'bare=0x8' | grep -v 'bare=0xffff' | head -3)
    [ -z "$bad_ids" ]; chk $? "every occurrence id carries bit 15"
    [ -n "$bad_ids" ] && echo "$bad_ids" | sed 's/^/       /'

    # (2) the names that were dead on every boot now resolve
    local dead=0
    for t in $DEAD_BEFORE; do
        grep -q "logbook '$t' full=0x8" "$L" || { bad "occurrence '$t' still not named"; dead=1; }
    done
    [ $dead = 0 ] && ok "all previously-dead occurrences now resolve"

    # (3) the volume's own vocabulary is untouched — display.elf keeps its tag
    grep -q "AUTOSTART.*display.elf.*tags: display," "$L"; chk $? "volume tags unshifted (display.elf still 'display')"
    grep -q "shell.bin.*tags: shell," "$L"; chk $? "volume tags unshifted (shell.bin still 'shell')"

    # (4) nothing collided in the on-disk registry
    ! grep -q "slot .* already used" "$L"; chk $? "no registry slot collision"

    # Which root ports are one hole in the case. The controller does not say,
    # so this driver pairs them by position and PRINTS that it is doing so —
    # a board wired differently has to be catchable by reading the boot, since
    # nothing else can catch it.
    grep -q "socket(s) have both halves" "$L"
    chk $? "the boot says which of its root ports share a socket"

    # And each port names its own other half, both ways round. A pairing that
    # only holds in one direction is one that will be asked the wrong way.
    local pair_line pair_a pair_b
    pair_line=$(grep -m1 -oE "port [0-9]+: .*same socket as port [0-9]+" "$L")
    pair_a=$(printf '%s' "$pair_line" | sed -nE 's/^port ([0-9]+):.*/\1/p')
    pair_b=$(printf '%s' "$pair_line" | sed -nE 's/.*same socket as port ([0-9]+)$/\1/p')
    [ -n "$pair_a" ] && [ -n "$pair_b" ] &&
        grep -q "port $pair_b: .*same socket as port $pair_a\b" "$L"
    chk $? "the pairing holds both ways (port $pair_a <-> port $pair_b)"

    # (5) the kernel's vocabulary is complete, not a handful of survivors
    local names
    names=$(grep -c '^\[PROBE\] logbook' "$L")
    local distinct
    distinct=$(grep '^\[PROBE\] logbook' "$L" | sed "s/.*logbook //;s/ full.*//" | sort -u | wc -l | tr -d ' ')
    [ "$distinct" -ge 24 ]; chk $? "Logbook holds the full vocabulary ($distinct distinct names, $names resolves)"

    # (6) TagFS itself still healthy
    grep -qE "TagFS: [0-9]+ run, [0-9]+ passed, 0 failed" "$L"; chk $? "TagFS startup tests: 0 failed"

    # (7) the kernel's ear works — mutation-proven harness, see commit
    grep -q "TOUCH WATCH TEST. PASSED: all 20 checks OK" "$L"; chk $? "TouchWatch self-test 20/20"

    # (8) THE MEDIUM IS ASKED ONCE, NOT FIVE TIMES.
    #
    # A boot used to read this seat's partition table five times and its deed
    # four, because five parties that know nothing about each other each asked
    # the same question: the room describing what it seated, the filesystem
    # looking for its volume, the filesystem asking the chosen seat again just
    # for the identity, the mount standing on the ground, and the boot survey.
    # On a flash drive every one of those is a real transfer.
    #
    # The reads themselves are silent, so what is checked here is the symptom
    # that was visible: the volume was DESCRIBED twice, three identical lines
    # each time. One description means one reader reached it.
    local described
    described=$(grep -cE '^\[Deed\] seat [0-9]+: volume [0-9a-f]{32}, ' "$L")
    [ "$described" = 1 ]
    chk $? "the volume is described once, not once per reader ($described)"

    # And the survey says so rather than skipping quietly. A survey that leaves
    # a ground out without naming it is a survey nobody can count.
    grep -q "carries the volume this machine is standing on"  "$L"
    chk $? "the boot survey names the ground already stood on instead of re-reading it"

    # (9) THE MOUNT CHECKS ITS OWN FAR COPY, AND SAYS SO.
    #
    # It always did — and only ever spoke when the far copy did NOT answer, so
    # a volume whose whole extent is present looked exactly like a volume
    # nobody had checked. [TagFS], not [Deed]: the boot survey has its own line
    # with the same words, and that one is about somebody else's ground.
    grep -qE '^\[TagFS\] seat [0-9]+: its far copy agrees' "$L"
    chk $? "the mount checked the far end of the volume it stood on"

    # (10) THE PASS ARRIVED THE WAY THE LOADER WROTE IT.
    #
    # The volume identity the whole boot leans on is read out of a block in
    # low memory that nothing defends. Until the seal it was believed because
    # its magic looked right — which a block half-overwritten by firmware also
    # manages. Both loaders seal it now; the kernel says which of the two
    # things it has: a block that adds up, or a block it had to take on trust.
    grep -q "the loader left a pass with 4 stamp(s)" "$L"
    chk $? "the pass carries its seal as well as its three facts"
    grep -q "sealed, and the seal agrees" "$L"
    chk $? "and the block adds up to what the loader sealed it with"

    # (11) THE MEDIUM SAYS HOW MUCH IT TAKES, AND IT IS ASKED.
    #
    # The room used to hand every medium the same 64 sectors. That is above the
    # eight past which this channel gives up its bus-master DMA path and moves
    # the bytes with the processor — so the one number was not merely a poor
    # fit, it took the fast path away from the disk this machine boots from.
    # Asked instead, the channel says eight and keeps it.
    grep -q "this medium takes 8 sector(s) at a time, so neighbouring blocks are read 1 at a time" "$L"
    chk $? "the legacy channel keeps the width its DMA path works at"

    # (12) THE TWO ENDPOINT-CONTEXT FIELDS NO BOOT EVER COMPUTES.
    #
    # Nothing in this driver prepares an isochronous endpoint, so the
    # isochronous half of the interval and error-count fields is unreachable —
    # it can be neither wrong in a way a running kernel shows nor right in a
    # way anybody can check. It is checked against the specification's own
    # table instead, where being unreachable does not matter.
    grep -q "endpoint-context self-test 15/15" "$L"
    chk $? "the endpoint-context fields agree with xHCI Table 6-45"
}

# ── a volume whose metadata will not read ────────────────────────────────
#
# The board failure this reproduces: a flash drive came up once with every
# file on it, and the boot after that had none. The metadata pool had been
# overwritten, and the mount met a block that was not a pool — which it
# answered by "starting fresh" and returning success, silently, because the
# line saying so was a debug_printf and those compile to nothing.
#
# A volume that reads as empty is a volume the next write finishes off. So the
# rule is: metadata that cannot be read means the volume does NOT mount, the
# machine says why, and nothing is written to the medium.
#
# The damage is done to the IMAGE, not to the code — the same way the deed
# checks are proven. Nothing here depends on a mutation being installed.
# ‼ THE IMAGE IS A BUILD PRODUCT, AND A MUTATED PRODUCT IS ANSWERED BY
# DELETING IT. There is no marker to write into a disk image and the tree-wide
# preflight cannot see one, so the only thing holding this straight is the
# manifest and the trap — which is exactly what `mutate_product` puts it under.
# Restoring it is `rm -f`: the next `build` makes it again from clean sources,
# which is both cheaper and more honest than keeping a hundred and thirty
# megabyte copy of something a Makefile can produce on demand.
badpool_on() {
    mutate_product build/boxos.img
    # ‼ AN IMAGE CARRIES NO COMMENT, so there is no marker for `mut_done` to
    # find and no preflight that could see this one left behind. The proof it
    # can give is its own bytes: the four at that offset must NOT already be
    # the corruption before the write (an image that already holds it is not a
    # clean image), and they must BE it afterwards. A scenario whose offset has
    # drifted then says so instead of booting a perfectly healthy volume and
    # reporting that the kernel failed to notice a corruption nobody made.
    if ! python3 - <<'EOF'
import struct, sys
# ground at 2048, data run begins at volume block 134, the pool is data block 2
off = (2048 + 136 * 8) * 512
f = open("build/boxos.img", "r+b")
f.seek(off)
was = f.read(4)
if was == b"\xAA\xBB\xCC\xDD":
    sys.exit("logcheck: that offset ALREADY holds the corruption — this image is not clean")
f.seek(off)
f.write(b"\xAA\xBB\xCC\xDD")
f.flush()
f.seek(off)
now = f.read(4)
f.close()
if now != b"\xAA\xBB\xCC\xDD":
    sys.exit("logcheck: the image did not take the corruption")
print("logcheck: metadata pool magic %s -> aabbccdd" % was.hex())
EOF
    then
        echo "badpool: the image could not be corrupted where the pool should be"
        exit 1
    fi
}
badpool_off() {
    restore build/boxos.img
}

run_badpool() {
    echo "== badpool: the metadata pool holds something that is not a pool =="
    build
    badpool_on
    # boot() rebuilds nothing — it runs what is on disk now.
    make run-stop >/dev/null 2>&1
    make run-bg   >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3
    type_cmd "help"
    sleep 1
    key_press ret
    sleep 3
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.badpool.log"
    badpool_off
    build   # leave the tree with an image built from clean sources
    L="$SCRATCH/serial.badpool.log"

    grep -q "where a metadata pool should be" "$L"
    chk $? "the mount says what it found instead of a pool"

    ! grep -q "starting fresh" "$L"
    chk $? "and does not decide the volume is empty"

    ! grep -qE "AUTOSTART. Started" "$L"
    chk $? "nothing from the volume was started off it"

    grep -q "BoxOS Shell" "$L"
    chk $? "the machine still reaches a shell"

    typed_ok "$L"
    chk $? "and answers a keystroke"
}

run_novolume() {
    echo "== novolume: tagfs_recognise() forced false =="
    probe_on; mutate_on; build; boot novolume; mutate_off; probe_off
    L="$SCRATCH/serial.novolume.log"

    grep -q "no volume\|No autostart" "$L"; chk $? "boot proceeds with no volume"
    grep -q "BoxOS Shell" "$L"; chk $? "a machine with no medium still reaches a shell"
    typed_ok "$L"; chk $? "and answers a keystroke"

    # THE point of the whole change: the console tag survives having no medium
    grep -q "logbook 'keyboard'.*bare=0x8" "$L"; chk $? "'keyboard' resolves with NO volume"

    grep -q "logbook 'usb:arrived' full=0x8" "$L"; chk $? "'usb:arrived' resolves with NO volume"

    local dead=0
    for t in $DEAD_BEFORE; do
        grep -q "logbook '$t' full=0x8" "$L" || { bad "occurrence '$t' missing with no volume"; dead=1; }
    done
    [ $dead = 0 ] && ok "full occurrence vocabulary present with no volume"

    ! grep -q "logbook .* bare=0xffff" "$L"; chk $? "no occurrence resolved to INVALID"
}

run_uefi() {
    # q35 + OVMF is the only configuration here that exposes MCFG, so it is
    # the only one that produces the data-driven pci:vendor:* names at all.
    # That is exactly the traffic that used to be interned into the volume's
    # registry and flushed to the medium.
    echo "== uefi/q35: data-driven names =="
    probe_on; build
    make run-stop >/dev/null 2>&1
    make run-bg UEFI=on >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.uefi.log"
    probe_off
    L="$SCRATCH/serial.uefi.log"

    grep -q "BoxOS Shell" "$L"; chk $? "uefi boot reaches the shell"

    local pci_n
    pci_n=$(grep -c "logbook 'pci:" "$L")
    [ "$pci_n" -gt 0 ]; chk $? "pci:* hardware inventory named in the Logbook ($pci_n)"

    local pci_low
    pci_low=$(grep "logbook 'pci:" "$L" | grep -vc 'bare=0x8')
    [ "$pci_low" = 0 ]; chk $? "no pci:* name took a volume id"

    grep -q "AUTOSTART.*display.elf.*tags: display," "$L"; chk $? "uefi: volume tags unshifted"

    # A family the muster DECLARED must not be reported as a stranger. It was:
    # the warning judged the full name, which is new for every device that has
    # ever existed, so "pci" being in the muster bought nothing and eight lines
    # were printed on every boot of a machine with MCFG. Measured here, on this
    # scenario, before and after: 8 lines -> 0. Removing X("pci") from the
    # muster prints exactly one line naming the family, which is the whole
    # point of the warning and is what must survive.
    local strangers
    strangers=$(grep -c "not in its muster" "$L")
    [ "$strangers" = 0 ]
    chk $? "no name the muster declared was called a stranger ($strangers)"

    # ── what the loader hands over, and where it puts it ───────────────────
    #
    # TagBoot writes the E820 map at 0x500, boot_info at 0xA000 and the pass
    # at 0xA600 — three fixed addresses it had never asked the firmware for,
    # while calling that same firmware's allocator several more times before
    # ExitBootServices. Two of those calls are AllocateAnyPages, which lets
    # the firmware pick, and low conventional memory is exactly what it may
    # pick. Now the pages are claimed first, and the loader SAYS how many it
    # got — a board whose firmware refuses one names it instead of booting
    # strangely.
    grep -qE "TagBoot: set aside [0-9]+ of 3 handoff page\(s\)" "$L"
    chk $? "the loader asked for the pages it writes the handoff into"
    grep -q "TagBoot: set aside 3 of 3 handoff page(s)" "$L"
    chk $? "and this firmware gave it all three"
    # ‼ This one cannot go red here and is kept anyway: OVMF gives all three,
    # so there is no refusal to report and removing the claim entirely leaves
    # it green. Its value is on a board whose firmware says no — which is the
    # only place the question was ever open.
    ! grep -q "would not set aside" "$L"
    chk $? "no handoff page was written into unclaimed"

    # The UEFI half of the seal — a different loader, a different CRC, the
    # same block and the same kernel check.
    grep -q "sealed, and the seal agrees" "$L"
    chk $? "the pass TagBoot wrote adds up too"
}

# Does the medium actually carry this text? Read off the image rather than
# believed from what the command printed. Every needle carries a
# runtime-formatted digit or a typed prompt, so none of them can match a format
# string sitting in some binary's rodata — the trap a plain grep walks into.
img_carries() {
    python3 - "$1" "$2" <<'PY'
import sys, re
img = open(sys.argv[1], 'rb').read()
sys.exit(0 if re.search(sys.argv[2].encode(), img) else 1)
PY
}

# ── noexec: bit 63 is nobody's business until somebody takes it up ──────────
#
# This is the scenario for the failure that kept BoxOS off two real machines,
# and it exists because the emulator cannot show it by accident: OVMF leaves
# IA32_EFER.NXE set, so the UEFI path always found the bit already legal here
# while both boards found it clear.
#
# What went wrong there: the kernel wrote bit 63 into the page table entries
# for EFI runtime data, then called firmware through those pages — while NXE
# was still 0 and bit 63 was therefore a RESERVED bit (Intel SDM Vol 3A §4.5).
# Every access through such an entry faults, so the firmware's first read of
# its own data was #PF err=0x9 = P|RSVD, inside SetVirtualAddressMap. The BIOS
# path never showed it because stage2 sets NXE on its way into long mode.
#
# Two halves, and the second is the one the emulator could never give:
#
#   1. On an ordinary UEFI boot the kernel must take no-execute up, and must
#      do it BEFORE the two things that used to come first — the EFI runtime
#      mapping, and per_core_init_bsp, where the write used to live. Order in
#      the log IS the property; a check that only asked "did it happen" would
#      have passed on the broken kernel too.
#   2. `make NOEXEC=off` reproduces the boards' machine state — NXE clear —
#      and the machine must still BOOT. That is the whole claim of the fix:
#      not "we set the bit", but "the kernel knows whether the bit is legal
#      and never writes one that is not".
run_noexec() {
    echo "== noexec: bit 63 is taken up before anything writes it =="

    # ── half one: the ordinary UEFI boot, where the panic happened ──────────
    build
    make run-stop >/dev/null 2>&1
    make run-bg UEFI=on >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.noexec.log"
    L="$SCRATCH/serial.noexec.log"

    grep -q "BoxOS Shell" "$L"; chk $? "uefi boot reaches the shell"

    grep -q "\[CPU\] no-execute taken up" "$L"
    chk $? "the kernel says it took no-execute up"

    # The ordering, by line number, because that is the defect: both of these
    # were AFTER the mapping that used the bit.
    local nx_line rt_line pc_line
    nx_line=$(grep -n "\[CPU\] no-execute taken up" "$L" | head -1 | cut -d: -f1)
    rt_line=$(grep -n "\[EFI\] SetVirtualAddressMap" "$L" | head -1 | cut -d: -f1)
    pc_line=$(grep -n "\[PER_CORE\] Initializing BSP" "$L" | head -1 | cut -d: -f1)

    [ -n "$nx_line" ] && [ -n "$rt_line" ] && [ "$nx_line" -lt "$rt_line" ]
    chk $? "no-execute is taken up before EFI runtime is mapped (${nx_line:-?} < ${rt_line:-?})"

    [ -n "$nx_line" ] && [ -n "$pc_line" ] && [ "$nx_line" -lt "$pc_line" ]
    chk $? "and before per-core init, where the write used to live (${nx_line:-?} < ${pc_line:-?})"

    # The fault this whole change is about, named by the KIND of fault and not
    # by the error code one board happened to print.
    #
    # ‼ It was written the other way first — `err=0x9`, straight off the photo
    # — and that check stayed GREEN under a mutation that panicked the machine,
    # because the emulator reports the same reserved-bit fault as err=0x8
    # (P=0 where the boards had P=1). An oracle keyed to one machine's error
    # code is an oracle for that machine.
    ! grep -q "reserved bit set in a paging entry" "$L"
    chk $? "no reserved-bit page fault"
    ! grep -q "KERNEL PANIC" "$L"
    chk $? "the boot does not panic"

    # The memory held across SetVirtualAddressMap comes back. Held pages that
    # are never released are a leak the machine cannot report any other way.
    grep -q "\[PMM\] EFI boot-services memory returned to the machine" "$L"
    chk $? "boot-services memory is given back after the firmware relocates"
    ! grep -q "was only .* page(s) free" "$L"
    chk $? "every boot-services range was this allocator's to hold"

    # The E820 the kernel divides memory by is the one ExitBootServices
    # accepted, not the one built several allocations earlier.
    #
    # ‼ This one cannot go red here and is kept anyway. Measured with the
    # rebuild disabled: OVMF produces a BYTE-IDENTICAL table either way,
    # because it answers the EfiACPIMemoryNVS request out of a region that was
    # already NVS before the snapshot. The hazard is on firmware whose NVS
    # pool has to grow into conventional memory — there the staged map lands
    # in pages this allocator would hand out, and only this line would say so.
    ! grep -q "the loader's E820 is older than its own allocations" "$L"
    chk $? "the staged EFI map is not memory the allocator calls free"

    # The firmware's own declaration of how tightly its runtime regions may be
    # mapped (UEFI 2.10 §4.6.4), honoured whole. Applied only after SVAM,
    # because the firmware writes to its own code during that call.
    grep -q "\[EFI\] memory attributes: [0-9]* runtime region(s) tightened" "$L"
    chk $? "runtime regions are tightened to what the firmware declared"
    grep -q "tightened to what the firmware declared, 0 left as they were" "$L"
    chk $? "and every entry in the table checked out"

    # ‼ The only check here that is EVIDENCE rather than arrangement: a real
    # runtime service, dispatched through the rebased pointer, into the
    # relocated firmware code, off the pages just made read-only. Everything
    # above it describes what was set up; this is the machine using it.
    grep -q "\[EFI\] runtime services answer:" "$L"
    chk $? "and a runtime service still answers afterwards"
    grep -qE "firmware clock reads [0-9]{4}-[0-9]{2}-[0-9]{2}" "$L"
    chk $? "with a date off the firmware's own clock"

    # ── half two: the boards' state, reproduced ─────────────────────────────
    echo "-- and again with NXE clear, the way both boards booted --"
    make NOEXEC=off >"$SCRATCH/build.log" 2>&1
    if [ $? -ne 0 ]; then
        echo "BUILD FAILED (NOEXEC=off) — tail:"; tail -25 "$SCRATCH/build.log"
        bad "NOEXEC=off build"; return
    fi
    make run-stop >/dev/null 2>&1
    make run-bg UEFI=on NOEXEC=off >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.noexec_off.log"
    local N="$SCRATCH/serial.noexec_off.log"

    grep -q "no-execute refused by this build" "$N"
    chk $? "the kernel says bit 63 stays out of every entry"

    grep -q "BoxOS Shell" "$N"
    chk $? "a machine with NXE clear still boots to a shell"

    ! grep -q "reserved bit set in a paging entry" "$N"
    chk $? "and takes no reserved-bit fault doing it"

    ! grep -q "KERNEL PANIC" "$N"
    chk $? "and does not panic"

    # Restore the tree's ordinary kernel so a later scenario does not inherit
    # a NOEXEC=off build.
    make >"$SCRATCH/build.log" 2>&1
}

# ── earlyirq: an interrupt that arrives before the kernel opened the door ───
#
# The second thing that kept BoxOS off a real machine, and the emulator could
# not show it either: on an i5-9400F booting UEFI, RFLAGS.IF was set by
# something OUTSIDE this kernel — stage2.asm clears it ten times over on the
# BIOS path, TagBootJump clears it nowhere, and UEFI 2.10 §8.1 obliges
# firmware to restore it after a runtime call, which is the kind of obligation
# firmware breaks. The first HPET tick then landed inside cpu_calibrate_tsc,
# thirty-six lines of kernel_main before scheduler_init() had allocated
# anything, and irq_handler wrote through the NULL it got back: #PF at 0x18.
#
# Three things are asked here:
#   1. On an ordinary boot NOTHING arrives early — the kernel owns the flag
#      from _start and takes it back after every firmware call.
#   2. The state the loader handed over is SAID, both paths, so a machine that
#      inherits an open flag names it instead of behaving strangely.
#   3. `make EARLYIRQ=on` reproduces the board's window exactly — the flag is
#      opened for the length of TSC calibration and closed again — and the
#      machine must survive it, say so, and boot.
#
# ‼ The narrow window is deliberate. Measured: leaving interrupts on for the
# REST of early init deadlocks the kernel later in TouchLogbookResolve,
# because early init is not written to be re-entered from an interrupt. That
# is why the real fix is ownership of the flag, not surviving one tick.
run_earlyirq() {
    echo "== earlyirq: the kernel opens its own interrupt door =="

    build
    make run-stop >/dev/null 2>&1
    make run-bg UEFI=on >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.earlyirq.log"
    L="$SCRATCH/serial.earlyirq.log"

    grep -q "BoxOS Shell" "$L"; chk $? "uefi boot reaches the shell"

    grep -q "\[BOOT\] the loader handed over RFLAGS=" "$L"
    chk $? "the kernel says what interrupt state the loader handed it"

    ! grep -q "arrived before this kernel was ready to be interrupted" "$L"
    chk $? "and nothing was delivered before the kernel opened the door"

    ! grep -q "Unhandled kernel #PF at 0x18" "$L"
    chk $? "no fault at 0x18 — the address a NULL scheduler state faults at"

    # The BIOS half, because the two loaders disagreed and only one was right.
    make run-stop >/dev/null 2>&1
    make run-bg >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.earlyirq_bios.log"
    local B="$SCRATCH/serial.earlyirq_bios.log"

    grep -q "\[BOOT\] the loader handed over RFLAGS=" "$B"
    chk $? "the bios path says it too"
    ! grep -q "arrived before this kernel was ready to be interrupted" "$B"
    chk $? "and nothing arrives early there either"

    # ── the board'"'"'s window, reproduced ───────────────────────────────────────
    echo "-- and again with the flag opened where the board took its tick --"
    make EARLYIRQ=on >"$SCRATCH/build.log" 2>&1
    if [ $? -ne 0 ]; then
        echo "BUILD FAILED (EARLYIRQ=on) — tail:"; tail -25 "$SCRATCH/build.log"
        bad "EARLYIRQ=on build"; return
    fi
    make run-stop >/dev/null 2>&1
    make run-bg UEFI=on EARLYIRQ=on >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell\|System halted" "$WAIT_QUIET_S"
    sleep 3
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.earlyirq_on.log"
    local E="$SCRATCH/serial.earlyirq_on.log"

    grep -q "arrived before this kernel was ready to be interrupted" "$E"
    chk $? "an interrupt that slips in early is named, with its vector"

    ! grep -q "Unhandled kernel #PF at 0x18" "$E"
    chk $? "and does not write through a scheduler that does not exist yet"

    ! grep -q "KERNEL PANIC" "$E"
    chk $? "and does not panic"

    grep -q "BoxOS Shell" "$E"
    chk $? "and the machine boots through it to a shell"

    make >"$SCRATCH/build.log" 2>&1
}

# ── lastsaid: the account survives the reset that ended the run ─────────────
#
# `logsave` writes THIS boot's log to the volume, and that is the right tool
# right up to the moment it is needed most. A board wedged itself — a storming
# interrupt and a host controller out of slots — and the volume was exactly
# what had stopped answering. The only account of it was four photographs.
#
# So the kernel carries its log through a warm reset in a fixed window it does
# not clear at boot, and `lastsaid` reads it. Both tools exist; neither
# replaces the other.
#
# ‼ What is asked here is the WHOLE claim: a cold start must say nothing came
# through (so an empty window is never dressed up as a log), a warm reset must
# carry the bytes, and the count the kernel announces must be the count the
# tool pours. QEMU's system_reset is a warm reset — it resets devices and CPU
# and leaves guest RAM alone — which is the same thing the RESET button on a
# case does, and NOT what holding the power button does.
run_lastsaid() {
    echo "== lastsaid: what the machine said before the reset =="

    make PRINTTOFILE=on >"$SCRATCH/build.log" 2>&1
    if [ $? -ne 0 ]; then
        echo "BUILD FAILED (PRINTTOFILE=on) — tail:"; tail -25 "$SCRATCH/build.log"
        bad "PRINTTOFILE=on build"; return
    fi

    make run-stop >/dev/null 2>&1
    make run-bg PRINTTOFILE=on >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3

    grep -q "nothing came through the last reset" build/serial.log
    chk $? "a cold start says nothing came through"

    # Warm reset: devices and CPU reset, guest RAM untouched.
    mon_raw "system_reset"
    wait_until "$WAIT_QUIET_S" "a SECOND shell banner after the warm reset" \
        '[ "$(grep -c "BoxOS Shell" build/serial.log 2>/dev/null)" = "2" ]'
    sleep 4

    grep -qE "the previous run left [0-9]+ byte\(s\) behind" build/serial.log
    chk $? "and the run after a warm reset finds what the one before it said"

    local announced
    announced=$(grep -oE "the previous run left [0-9]+ byte" build/serial.log \
                | tail -1 | grep -oE "[0-9]+")
    [ -n "$announced" ] && [ "$announced" -gt 0 ]
    chk $? "and it is not an empty window dressed up as a log ($announced bytes)"

    type_cmd "lastsaid"
    key_press ret
    # Waited on the sentence each check is about, not on a number of seconds.
    # Every one of these commands reads a volume, and a volume read is as long
    # as the medium takes.
    wait_for_line -E build/serial.log "^-- [0-9]+ byte\(s\) from the previous run --" "$WAIT_QUIET_S"

    grep -qE "^-- [0-9]+ byte\(s\) from the previous run --" build/serial.log
    chk $? "lastsaid prints it, which is what a machine with no volume needs"

    local poured
    poured=$(grep -oE "^-- [0-9]+ byte" build/serial.log | tail -1 | grep -oE "[0-9]+")
    [ -n "$poured" ] && [ "$poured" = "$announced" ]
    chk $? "and pours exactly what the kernel announced ($poured of $announced)"

    # Both tools, still. The point was never to replace one with the other.
    type_cmd "lastsaid prev.log"
    key_press ret
    wait_for_line -E build/serial.log "byte\(s\) of the previous run written to prev.log" "$WAIT_QUIET_S"
    grep -qE "byte\(s\) of the previous run written to prev.log" build/serial.log
    chk $? "and writes a file when it is given a name"

    type_cmd "logsave now.log"
    key_press ret
    wait_for_line -E build/serial.log "byte\(s\) written to now.log" "$WAIT_QUIET_S"
    grep -qE "byte\(s\) written to now.log" build/serial.log
    chk $? "and logsave still writes this boot's log, untouched"

    # ── the copy that cannot be taken away ──────────────────────────────────
    #
    # Both tools above are ELF files ON the volume. The board failure being
    # chased is one where the volume mounts, reports its files, and the shell
    # can find none of them — so at the moment the log is worth having,
    # nothing that could write it can be started. `said` is compiled into
    # shell.bin and needs no lookup and no spawn.
    type_cmd "said"
    key_press ret
    wait_for_line -E build/serial.log "^-- [0-9]+ byte\(s\): what this boot has said --" "$WAIT_QUIET_S"
    grep -qE "^-- [0-9]+ byte\(s\): what this boot has said --" build/serial.log
    chk $? "the built-in prints this boot's log without loading anything"

    type_cmd "said before"
    key_press ret
    wait_for_line -E build/serial.log "^-- [0-9]+ byte\(s\): what the run before this one said --" "$WAIT_QUIET_S"
    grep -qE "^-- [0-9]+ byte\(s\): what the run before this one said --" build/serial.log
    chk $? "and the previous run's, through the same door"

    type_cmd "said kept.log"
    key_press ret
    wait_for_line -E build/serial.log "written to kept.log" "$WAIT_QUIET_S"
    grep -qE "written to kept.log" build/serial.log
    chk $? "and files it when given a name"

    grep -q "said \[before\]" build/serial.log || {
        type_cmd "help"
        key_press ret
        wait_for_line build/serial.log "said \[before\]" "$REACT_QUIET_S"
    }
    grep -q "said \[before\]" build/serial.log
    chk $? "and help names it, so it can be found without being known"

    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.lastsaid.log"
    make >"$SCRATCH/build.log" 2>&1
}

# ── mountfail: a mount that fails is a mount, not the end of the volume ─────
#
# The board failure this closes: the room seats the medium, the deed is read,
# "6009 data blocks, 410 free, 58 files" is printed — and the shell answers
# `Unknown command` to every one of those files. Two photographs of it, and
# the decisive fact was a line that WAS NOT THERE.
#
# tagfs_init brings up eleven things in order. A mount that failed partway used
# to leave the first five standing and nothing ever took them down, so every
# LATER mount died at TagFS_CowInit with ERR_ALREADY_INITIALIZED — reported by
# a debug_printf, which compiles to nothing in a shipped build. One transient
# read error cost the machine its filesystem for the rest of its boot, silently.
#
# Nothing in QEMU ever fails a mount, so this whole path was unreachable from
# the desk and the repair would have been green by construction. MOUNTFAIL=on
# fails the FIRST mount at the worst point there is, with all five of those
# subsystems up, and this scenario checks the whole story: the failure is SAID,
# the catch-up mount SUCCEEDS, and the shell can then find and run a program
# that lives on the volume.
#
# ‼ MEASURED AGAINST THE MUTATION IT EXISTS FOR. With the clearing taken out of
# mount_refused, the second mount answers "its copy-on-write layer would not
# start" and the machine falls back to its embedded shell with no files — the
# board, reproduced on the desk. Two other mutations were tried first and both
# stayed green, which is why this one is written against the line that does the
# work rather than the one that reads as though it does.
run_mountfail() {
    echo "== mountfail: one failed mount does not cost the volume for good =="

    make MOUNTFAIL=on >"$SCRATCH/build.log" 2>&1
    if [ $? -ne 0 ]; then
        echo "BUILD FAILED (MOUNTFAIL=on) — tail:"; tail -25 "$SCRATCH/build.log"
        bad "MOUNTFAIL=on build"; return
    fi

    make run-stop >/dev/null 2>&1
    # ‼ The key goes on run-bg TOO. It decides which mark file the tree carries,
    # and a bare `make run-bg` rebuilds the whole kernel without it — measured,
    # and it cost a run that looked like the reproduction had failed.
    make run-bg MOUNTFAIL=on >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3

    type_cmd "hw"
    sleep 1
    key_press ret
    sleep 4

    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.mountfail.log"
    local L="$SCRATCH/serial.mountfail.log"

    # ── the failure happened, and it SAID so ────────────────────────────────
    grep -q "\[TagFS\] this volume is NOT mounted: MOUNTFAIL=on asked this one to fail" "$L"
    chk $? "the first mount fails where it was told to, and says so out loud"

    grep -q "\[Storage Deck\] no volume yet" "$L"
    chk $? "and the deck carries the refusal up rather than swallowing it"

    # ‼ The line that was missing on the board. Everything printed during a
    # mount — the deed, the far copy, the file count — is printed by a mount
    # that then fails, so none of it answers "is there a filesystem". This does.
    local said mounted
    said=$(grep -c "\[TagFS\] this volume is NOT mounted" "$L")
    mounted=$(grep -c "\[TagFS\] the volume on seat .* is MOUNTED" "$L")
    [ "$said" = 1 ] && [ "$mounted" = 1 ]
    chk $? "exactly one mount refused and exactly one mounted (said $said, mounted $mounted)"

    # ── and the SECOND mount, on the same medium, works ─────────────────────
    grep -q "a medium arrived carrying a volume, and this machine had none" "$L"
    chk $? "the catch-up mount takes the volume up after the failure"

    # The one that goes red under the mutation: with the ground left uncleared,
    # this line reads "its copy-on-write layer would not start" instead.
    ! grep -q "copy-on-write layer would not start" "$L"
    chk $? "the second mount is not poisoned by the first one's remains"

    # ── and the machine can actually USE it, which is the whole point ────────
    #
    # Autostart reads its files off the volume. A machine whose volume did not
    # come back says "No autostart files found" and falls back to the embedded
    # shell — which is exactly what the board did, with a stick in the socket.
    grep -q "\[AUTOSTART\] Started 'shell.bin'" "$L"
    chk $? "shell.bin is found on the volume and started"
    ! grep -q "No autostart files found" "$L"
    chk $? "and the machine does not fall back to the shell built into it"

    grep -q "CPU features" "$L"
    chk $? "an external utility off the volume runs"
    external_ok "$L"
    chk $? "and the prompt comes back after it"

    make >"$SCRATCH/build.log" 2>&1
}

run_logsave() {
    echo "== logsave: what the kernel said, written down where it can be read =="

    # Two kernels, because the switch is a BUILD switch. Every kernel keeps the
    # ring of its own run (it is the serial line's source); PRINTTOFILE adds
    # the window that carries it through a reset. Both halves have to hold:
    # both kernels must produce a readable account of their run, and the one
    # without the window must SAY it carries nothing through a reset rather
    # than hand back an empty file that reads like a machine which never spoke.
    make PRINTTOFILE=on >"$SCRATCH/build.log" 2>&1
    if [ $? -ne 0 ]; then
        echo "BUILD FAILED (PRINTTOFILE=on) — tail:"; tail -25 "$SCRATCH/build.log"; exit 1
    fi

    make run-stop >/dev/null 2>&1
    make run-bg PRINTTOFILE=on >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3

    # A command FIRST, so the file is asked to carry both what a photograph of
    # the screen would have shown and what it would not: the command that was
    # typed. Userspace output reaches the ring through the Manifest VGA ops,
    # deliberately not through the serial mirror a board build turns off.
    type_cmd "hw"
    sleep 1; key_press ret; sleep 4
    type_cmd "logsave"
    sleep 1; key_press ret; sleep 8
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.logsave.log"
    cp build/boxos.img  "$SCRATCH/logsave.img"
    L="$SCRATCH/serial.logsave.log"
    I="$SCRATCH/logsave.img"

    grep -q "BoxOS Shell" "$L"; chk $? "boot reaches the shell with the log kept"

    grep -qE "[0-9]+ byte\(s\) written to watch.log" "$L"
    chk $? "logsave says how much it wrote, and where"

    ! grep -qE "^0 byte\(s\) written" "$L"
    chk $? "and it is not nothing"

    ! grep -q "byte(s) were said before this" "$L"
    chk $? "one boot does not overflow the ring"

    sed -n '/~ logsave/,$p' "$L" | grep -qE '^~ ?$|^~ '
    chk $? "the shell came back after it"

    img_carries "$I" '\[Logbook\] muster: \d+ name\(s\) seated'
    chk $? "the file carries a line from before the screen could scroll"

    img_carries "$I" '~ hw\n'
    chk $? "the file carries the command that was typed"

    # A RUNTIME value, not a label. The first form of this check looked for
    # "invariant TSC : " and passed with the ring deliberately broken — that
    # string is the format literal in hw.elf's rodata, sitting in the same
    # image, and the check was reading the binary rather than the log. The
    # number here is printed through %u and exists nowhere but in the account.
    img_carries "$I" 'TSC freq      : \d+ kHz'
    chk $? "and the answer that command produced"

    # Now the same on a kernel without the carry-over window.
    build
    make run-stop >/dev/null 2>&1
    make run-bg >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3
    type_cmd "logsave"
    sleep 1; key_press ret; sleep 8
    type_cmd "lastsaid"
    sleep 1; key_press ret; sleep 4
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.logsave-off.log"
    O="$SCRATCH/serial.logsave-off.log"

    grep -qE "[0-9]+ byte\(s\) written to watch.log" "$O"
    chk $? "a kernel without the window still writes down what it said"

    grep -q "carries nothing through a reset" "$O"
    chk $? "and says by name that it carries nothing through a reset"
}

# ---------------------------------------------------------------------------
# manyports — every port the controller says it has is a port this driver uses
#
# MaxPorts is an eight-bit field. This driver surveyed the first thirty-one
# root ports and kept a thirty-two-bit note of which it had done, so a device
# in any socket above that was invisible to the boot: not reported, not
# enumerated, not mentioned. A machine booting off a stick in one of them
# would not have booted, and nothing anywhere would have said why.
#
# ‼ WHAT THIS CANNOT DO. QEMU's xHCI allows fifteen ports per protocol and no
# more, so thirty is the widest controller that can stand on this desk and the
# cliff at thirty-one cannot be reached. What is checked instead is the
# INVARIANT the cliff broke — every port the controller reports is looked at
# and described — which is worth something at any width and which goes red the
# moment a bound of any size comes back. The bound is what the mutation below
# puts back, at fifteen instead of thirty-one, and the keyboard sitting on
# port 16 is what notices.
# ---------------------------------------------------------------------------
run_manyports() {
    echo "== manyports: thirty root ports, and none of them ignored =="
    build

    make run-stop >/dev/null 2>&1
    make run-bg USB=on XHCIPORTS=15 >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 2
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.manyports.log"
    L="$SCRATCH/serial.manyports.log"

    grep -q "BoxOS Shell" "$L"
    chk $? "boot reaches the shell with thirty root ports"

    # What the controller says it has, taken from its own summary line rather
    # than from the number this scenario asked for — the emulator is entitled
    # to give fewer than requested and a check against our own wish would then
    # be a check against nothing.
    local claimed described
    claimed=$(grep -oE "controller ready: [0-9]+ port" "$L" | head -1 |
              sed -nE 's/.*: ([0-9]+) port/\1/p')
    described=$(grep -cE "^\[xHCI [0-9a-f:.]+\] port [0-9]+: (powered|NOT powered)" "$L")

    [ -n "$claimed" ] && [ "$claimed" -gt 16 ]
    chk $? "the controller really has more than sixteen ports ($claimed)"

    [ "$claimed" = "$described" ]
    chk $? "every port it says it has is described ($described of $claimed)"

    # The survey, as opposed to the description: the keyboard sits on the first
    # USB 2 port, which with fifteen SuperSpeed ports in front of it is port 16.
    # A survey that stops short never finds it, and the machine boots without a
    # keyboard rather than saying anything.
    grep -qE "port ([2-9][0-9]|1[6-9]): device attached at boot" "$L"
    chk $? "the boot survey reached a port above the sixteenth"

    grep -qE "port ([2-9][0-9]|1[6-9]): .*keyboard .* is live" "$L"
    chk $? "and the device there came up"
}

run_usbrecover() {
    echo "== usbrecover: the deck is asked to put a controller back in service =="

    # Four cores, for the reason latearrival and replug already give above: the
    # deferred work of this kernel runs from the K-Core guide loop and from
    # cpu_idle, and on one core neither of them runs. Measured again here by
    # probe while this scenario was written — with CORES=1 the proof below is
    # never reached at all.
    #
    # The build key exists because this costs the machine every USB device it
    # has. A shipped build does not do that to itself.
    make USBRECOVER=on >"$SCRATCH/build.log" 2>&1
    if [ $? -ne 0 ]; then
        echo "BUILD FAILED — tail:"; tail -25 "$SCRATCH/build.log"; exit 1
    fi

    make run-stop >/dev/null 2>&1
    make run-bg USBRECOVER=on CORES=4 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3

    # Typed AFTER the controller has been taken down and brought back. The
    # keyboard is on that controller: this is the whole point of the scenario.
    type_cmd "help"
    sleep 1
    key_press ret
    sleep 3
    type_cmd "hw"
    sleep 1
    key_press ret
    sleep 4

    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.usbrecover.log"
    L="$SCRATCH/serial.usbrecover.log"

    # The surface first: the two opcodes that used to break the machine are
    # withdrawn, and their numbers answer to nothing rather than to something
    # else. hw.usb.reset is still there and still system-only.
    grep -q "the two withdrawn opcodes answer to nothing" "$L"
    chk $? "hw.usb.init and hw.usb.stop are gone and their numbers are spent"

    grep -q "hw.usb.reset is registered, system-only" "$L"
    chk $? "hw.usb.reset is still there, and still system-only"

    # Then the op itself, reached through the registry the way the dispatcher
    # reaches it — so what is proved is the surface and not the driver behind.
    grep -q "asked for the controller on .* to be put back in service" "$L"
    chk $? "the deck was asked, by name, through the op registry"

    # And what it did. A bare xhci_reset — which is what this op used to be —
    # prints the first of these and none of the rest, and leaves the machine
    # with no USB and no line saying so.
    grep -q "putting the controller back in service" "$L"
    chk $? "the driver began the whole repair, not a bare reset"

    grep -q "back in service — looking again at what is plugged in" "$L"
    chk $? "and finished it: the controller is running and the bus resurveyed"

    # THE check. The keyboard lives on the controller that was just reset, so
    # it went away with it. A repair that stops halfway leaves this line absent
    # and the machine unusable from that moment on.
    #
    # ‼ The anchor must EXIST, and this check said so only after a mutation
    # proved it did not. Written as `awk -v n="${began:-0}"`, a missing anchor
    # became line zero and the check then matched the keyboard's ORIGINAL boot
    # enumeration — it passed with the repair reverted to the bare reset it
    # replaced, which is the one case it exists to catch.
    local began came_back
    began=$(grep -n "putting the controller back in service" "$L" | head -1 | cut -d: -f1)
    came_back=""
    if [ -n "$began" ]; then
        came_back=$(awk -v n="$began" 'NR>n && /keyboard .* is live/ {print NR; exit}' "$L")
    fi
    [ -n "$came_back" ]
    chk $? "the keyboard came back after the controller did"

    grep -q "USB RECOVER TEST. PASSED" "$L"
    chk $? "the op answered success"

    ! grep -q "USB RECOVER TEST. FAIL" "$L"
    chk $? "nothing in the proof failed"

    # And the machine, afterwards. Typing is the only question the board
    # failure was ever really about, and here it is asked of a keyboard that
    # has been through a controller reset.
    typed_ok "$L";    chk $? "and the machine answers a keystroke after all that"
    external_ok "$L"; chk $? "and the prompt came back after an external utility"

    # The volume was not on that controller in this configuration, and must not
    # have noticed anything at all.
    ! grep -q "the medium the volume lives on has left" "$L"
    chk $? "the volume on the other seat was untouched by any of it"
}

run_yank() {
    echo "== yank: the stick is pulled while the volume is being read =="

    # replug pulls the stick when the machine is idle, so nothing is in flight
    # and the recovery path is never entered. The window this driver used to
    # spend tens of seconds in is the other one: a transfer outstanding at the
    # moment the device leaves. Two things are needed to reach it.
    #
    # The stick must BE the volume, or nothing reads from it — the boot image
    # is on ATA. Same mutation late-arrival uses, and it stays installed for
    # the whole run, for the reason written there.
    latearrival_on; build
    cp build/boxos.img "$SCRATCH/stick.img"

    make run-stop >/dev/null 2>&1
    make run-bg USB=on CORES=4 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 2
    local MARK
    MARK=$(wc -l < build/serial.log)

    mon_raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw"
    sleep 1
    mon_raw "device_add usb-storage,drive=stick,id=usbstick"

    # And the MEDIUM must be slow. QEMU answers a read the instant it is asked,
    # so the whole mount burst is over before a poll of the log can see it
    # begin — there is no window to aim at, and a fixed delay lands either side
    # of it. Held to 64 KiB/s the same burst takes seconds, which is also what
    # a flash drive on a real bus looks like. Nothing in the guest is touched:
    # this is the emulator being told to behave less like a RAM disk.
    mon_raw "block_set_io_throttle stick 0 65536 0 0 0 0"

    # Fired off the first sign of the volume being read, not off a clock.
    #
    # ‼ THE ONE POLL IN THIS FILE THAT KEEPS ITS CEILING, AND ON PURPOSE.
    # It is not waiting for the machine to finish something — it is aiming at a
    # WINDOW: the stick has to be pulled while the read is in flight, so the
    # poll runs at twenty milliseconds and the ceiling is eight seconds of that
    # window. It also does not fail anything when it expires; the pull happens
    # either way and the checks below judge what the machine did. A silence
    # watchdog is the wrong instrument for a window: the machine is at its most
    # talkative exactly when this is waiting.
    i=0
    while [ $i -lt 400 ]; do
        tail -n +$((MARK+1)) build/serial.log 2>/dev/null | grep -q "Deed] seat" && break
        python3 -c "import time; time.sleep(0.02)"
        i=$((i+1))
    done
    sleep 1
    mon_raw "device_del usbstick"

    # Waited out past XHCI_RETIRE_PATIENCE_MS (15 s), not merely past the
    # departure. The last check below is on the slot service saying it has been
    # trying to take a slot down and cannot — and a window shorter than the
    # patience makes that check green on a machine where it is false, which is
    # decoration rather than a guard. Measured: at fourteen seconds it passed
    # against the very code it exists to catch.
    sleep 20
    make run-stop >/dev/null 2>&1
    tail -n +$((MARK+1)) build/serial.log > "$SCRATCH/serial.yank.log"
    latearrival_off
    L="$SCRATCH/serial.yank.log"

    grep -q "mounted from seat" "$L"
    chk $? "the stick became this machine's volume"

    # The point of the whole scenario. Measured against the code before this
    # change, same script, same throttle: 4 transport resets, 2 endpoint
    # clears, no departure and NO EMPTY SEAT within fourteen seconds.
    grep -q "the device has left, so it is not being asked for anything more" "$L"
    chk $? "the driver stopped because the device left, not because a clock ran out"

    ! grep -q "resetting the transport" "$L"
    chk $? "nothing was spent resetting a transport that is not there"

    ! grep -q "halted — clearing it" "$L"
    chk $? "no endpoint of a departed device was reset"

    ! grep -q "no answer in .* ms at stage" "$L"
    chk $? "no budget was waited out for an answer nobody could give"

    # ‼ THE SOCKET, NOT THE PORT.
    #
    # A USB 3 socket is two root ports, and a device whose SuperSpeed link does
    # not train leaves one of them and appears on the other — which on the port
    # it left is bit-for-bit what a hand pulling it out looks like. Here the
    # emulator really does take the device away, so BOTH halves are empty, and
    # the driver has to say so rather than say something that would be equally
    # true of a link falling back.
    grep -q "unplugged — the socket is empty" "$L"
    chk $? "the departure says the SOCKET is empty, not just the port"

    ! grep -q "the other half of the same socket — has something in it" "$L"
    chk $? "and does not claim the device moved to the socket's other half"

    grep -q "is gone" "$L"
    chk $? "the unit was released"

    grep -q "seat .* is empty" "$L"
    chk $? "and its chair was emptied"

    # The slot service says this when a slot has been leaving for longer than
    # its patience because somebody is still inside it. That somebody was the
    # recovery grind.
    ! grep -q "has been leaving for" "$L"
    chk $? "no caller was still inside the slot when it wanted to leave"
}

# ── holdground: the volume comes back while somebody is still inside it ────
#
# replug proves a volume that leaves and comes home is re-read. It proves it on
# an IDLE machine, which is the one case where the re-read is free: nobody is
# in the filesystem, so the memory it hands back — the tag registry, the tag
# index, the block bitmap, the free list, the file table, the metadata pool —
# is memory nobody is holding. That is not the machine this has to survive.
#
# `make HOLDGROUND=on` puts somebody there. At the moment the medium is noticed
# gone, one caller steps into the volume and does not step out until the volume
# is back; and a file is held OPEN across the whole departure. Three separate
# lines of the repair are then falsifiable, and each was measured against its
# own mutation:
#
#   take the "somebody is inside" refusal out of tagfs_abandon      -> the
#      take-down says nothing and frees the volume under the caller.
#      MEASURED: 6 passed, 7 failed;
#   take the mounting stamp off the handle (handle_belongs_here)    -> the old
#      handle reads block numbers that now belong to another file, and says
#      nothing. MEASURED: 11 passed, 2 failed — and exactly the two;
#   put memset(g_open_files, ...) back into tagfs_init              -> the
#      entry a live handle owns is no longer the one the table hands out, so
#      two writers to one file take two different locks.
#      MEASURED: 11 passed, 2 failed — and exactly the other two.
run_holdground() {
    echo "== holdground: the volume comes back while somebody is still in it =="

    # The stick must BE the volume — the same mutation replug and yank use, and
    # it stays installed for the whole run for the reason written there.
    latearrival_on
    make HOLDGROUND=on >"$SCRATCH/build.log" 2>&1
    if [ $? -ne 0 ]; then
        echo "BUILD FAILED (HOLDGROUND=on) — tail:"; tail -25 "$SCRATCH/build.log"
        latearrival_off; bad "HOLDGROUND=on build"; return
    fi
    cp build/boxos.img "$SCRATCH/stick.img"

    make run-stop >/dev/null 2>&1
    # ‼ The key goes on run-bg TOO: it decides which mark file the tree carries,
    # and a bare `make run-bg` rebuilds the kernel without it. Same trap that
    # cost mountfail a whole run.
    make run-bg HOLDGROUND=on USB=on CORES=4 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"

    # First arrival: the stick becomes this machine's volume, and the probe
    # opens a file on it.
    mon_raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw"
    sleep 1
    mon_raw "device_add usb-storage,drive=stick,id=usbstick"
    wait_for_line build/serial.log "hands over" "$WAIT_QUIET_S"
    sleep 2

    local FIRST_LINES
    FIRST_LINES=$(wc -l < build/serial.log)

    # Pulled out — and waited on the PROBE's own line, not on a clock: it is
    # the fact that there is somebody inside the volume to protect.
    mon_raw "device_del usbstick"
    wait_for_line build/serial.log "HOLDGROUND. a caller was inside the volume" "$WAIT_QUIET_S"
    sleep 2

    # And pushed back in. drive_add first: an HMP drive is auto-delete and went
    # with the device.
    mon_raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw"
    sleep 1
    mon_raw "device_add usb-storage,drive=stick,id=usbstick"
    wait_for_line build/serial.log "HOLDGROUND. the open-file entry" "$WAIT_QUIET_S" $((FIRST_LINES + 1))
    sleep 3

    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.holdground.log"
    latearrival_off
    L="$SCRATCH/serial.holdground.log"
    tail -n +$((FIRST_LINES + 1)) "$L" > "$SCRATCH/serial.holdground.second.log"
    local S="$SCRATCH/serial.holdground.second.log"

    # ── the situation was actually reached ──────────────────────────────────
    grep -q "a medium arrived carrying a volume, and this machine had none" "$L"
    chk $? "the stick was mounted on its first arrival"
    grep -q "HOLDGROUND. holding file .* open across whatever happens" "$L"
    chk $? "and a file was held open on it"
    grep -q "HOLDGROUND. a caller was inside the volume when its medium left" "$S"
    chk $? "somebody was inside the volume when the medium left"
    grep -q "the volume is back, in seat" "$S"
    chk $? "and the volume came back"

    # ── ‼ THE BARRIER. Nothing is let go of while somebody is on it ─────────
    grep -qE "the volume is on its way out and [1-9][0-9]* caller\(s\) are still inside it" "$S"
    chk $? "the take-down found a caller inside and let go of nothing"

    grep -q "the last caller is out and the old volume has been let go of" "$S"
    chk $? "and finished only once that caller was out"

    # In that ORDER. A "let go of" line that precedes the "still inside" line
    # would mean the memory went back first and the sentence came after.
    local WAIT_AT FREE_AT MOUNT_AT
    WAIT_AT=$(grep -nE "the volume is on its way out and [1-9]" "$S" | head -1 | cut -d: -f1)
    FREE_AT=$(grep -n  "the last caller is out and the old volume" "$S" | head -1 | cut -d: -f1)
    MOUNT_AT=$(grep -n "the volume on seat .* is MOUNTED" "$S" | tail -1 | cut -d: -f1)
    [ -n "$WAIT_AT" ] && [ -n "$FREE_AT" ] && [ -n "$MOUNT_AT" ] && \
        [ "$WAIT_AT" -lt "$FREE_AT" ] && [ "$FREE_AT" -lt "$MOUNT_AT" ]
    chk $? "waited, then let go, then mounted — in that order (${WAIT_AT:-?}, ${FREE_AT:-?}, ${MOUNT_AT:-?})"

    # ── ‼ THE HANDLE FROM BEFORE IS REFUSED, NOT FOLLOWED ──────────────────
    grep -q "was opened on an earlier mounting of this volume" "$S"
    chk $? "a read through the handle from the previous mounting is refused"

    grep -q "HOLDGROUND. a read through the handle from before returned -1" "$S"
    chk $? "and the refusal reaches the caller as a failure"

    # ── ‼ THE OPEN-FILE TABLE BELONGS TO THE MACHINE ───────────────────────
    grep -q "HOLDGROUND. the open-file entry that handle owns is still in the table" "$S"
    chk $? "the entry a live handle owns survived the re-mount"

    grep -q "HOLDGROUND. opening the same file again found the same entry" "$S"
    chk $? "and one file still has exactly one entry, so its writers share one lock"

    # ── and the machine is usable afterwards, which is the point ───────────
    grep -q "the volume on seat .* is MOUNTED" "$S"
    chk $? "the volume was taken back up"
    grep -q "BoxOS Shell" "$L"
    chk $? "the machine still has a shell"
}

# ── returnfail: a mount that did not finish is asked again by the machine ──
#
# Mounting is driven by ARRIVALS. A medium that is already seated does not
# arrive twice, so a mount that begins on a volume's RETURN and fails used to
# leave the machine with no filesystem and nothing that would ever ask again —
# the stick in its socket, answering, ignored for the rest of the boot.
#
# ‼ MEASURED ON THE OWNER'S BOARD, 2026-08-30, BEFORE THIS EXISTED: three
# returns out of fifty-five failed their mount, and all three recovered ONLY
# because the hand at the machine kept replugging. A read that fails on a
# medium which STAYS PUT had no way back at all.
#
# `make RETURNFAIL=on` fails the first re-mount once, with the medium left
# exactly where it is. The stick is plugged in ONCE here — that single arrival
# is spent on the attempt that fails — so every line after it is the machine
# asking again by itself.
#
# ‼ THE MUTATION IT EXISTS FOR: take mount_owed() out of the return road in
# TagFSVolumeReturned. The "asking again" line never appears, no second
# `is MOUNTED` follows, and the machine sits volumeless with its stick in.
run_returnfail() {
    echo "== returnfail: a mount that did not finish is asked again =="

    latearrival_on
    make RETURNFAIL=on >"$SCRATCH/build.log" 2>&1
    if [ $? -ne 0 ]; then
        echo "BUILD FAILED (RETURNFAIL=on) — tail:"; tail -25 "$SCRATCH/build.log"
        latearrival_off; bad "RETURNFAIL=on build"; return
    fi
    cp build/boxos.img "$SCRATCH/stick.img"

    make run-stop >/dev/null 2>&1
    make run-bg RETURNFAIL=on USB=on CORES=4 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"

    # First arrival — the mount that succeeds, and the one the stamp counts.
    mon_raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw"
    sleep 1
    mon_raw "device_add usb-storage,drive=stick,id=usbstick"
    wait_for_line build/serial.log "hands over" "$WAIT_QUIET_S"
    sleep 2

    local FIRST_LINES
    FIRST_LINES=$(wc -l < build/serial.log)

    # Pulled out.
    mon_raw "device_del usbstick"
    wait_for_line build/serial.log "the medium the volume lives on has left" "$WAIT_QUIET_S"
    sleep 2

    # And pushed back in ONCE. That single arrival is spent on the re-mount
    # this build is told to fail; nothing is plugged in after it.
    mon_raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw"
    sleep 1
    mon_raw "device_add usb-storage,drive=stick,id=usbstick"

    # Waited on the machine's own words, not a clock: the retry is spaced by
    # TAGFS_MOUNT_RETRY_MS and gets eight attempts, so a pass costs a second.
    wait_for_line build/serial.log "asking again for the volume nobody has mounted" "$WAIT_QUIET_S" $((FIRST_LINES + 1))
    sleep 4

    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.returnfail.log"
    latearrival_off
    L="$SCRATCH/serial.returnfail.log"
    tail -n +$((FIRST_LINES + 1)) "$L" > "$SCRATCH/serial.returnfail.second.log"
    local S="$SCRATCH/serial.returnfail.second.log"

    # ── the situation was reached ───────────────────────────────────────────
    grep -q "a medium arrived carrying a volume, and this machine had none" "$L"
    chk $? "the stick was mounted on its first arrival"
    grep -q "the medium the volume lives on has left" "$S"
    chk $? "and the medium then left"
    grep -q "this volume is NOT mounted: RETURNFAIL=on asked this re-mount to fail" "$S"
    chk $? "the re-mount failed where it was told to, and said so out loud"
    grep -q "the volume came back and would not mount" "$S"
    chk $? "and the return road carried the refusal up"

    # ‼ ONE arrival, and it was spent. Everything after is the machine itself.
    local arrivals
    arrivals=$(grep -c "arrived after the room was called to order" "$S")
    [ "$arrivals" = 1 ]
    chk $? "the stick was plugged in exactly once after the pull ($arrivals)"

    # ── ‼ THE MACHINE ASKED AGAIN BY ITSELF ────────────────────────────────
    grep -q "asking again for the volume nobody has mounted" "$S"
    chk $? "the machine asked again for the volume nobody had mounted"

    grep -q "\[TagFS\] the volume on seat .* is MOUNTED" "$S"
    chk $? "and the volume came up"

    # In that order: the refusal, then the asking, then the mount.
    local FAIL_AT ASK_AT UP_AT
    FAIL_AT=$(grep -n "came back and would not mount" "$S" | head -1 | cut -d: -f1)
    ASK_AT=$(grep -n  "asking again for the volume" "$S" | head -1 | cut -d: -f1)
    UP_AT=$(grep -n   "is MOUNTED" "$S" | tail -1 | cut -d: -f1)
    [ -n "$FAIL_AT" ] && [ -n "$ASK_AT" ] && [ -n "$UP_AT" ] && \
        [ "$FAIL_AT" -lt "$ASK_AT" ] && [ "$ASK_AT" -lt "$UP_AT" ]
    chk $? "refused, then asked again, then mounted — in that order (${FAIL_AT:-?}, ${ASK_AT:-?}, ${UP_AT:-?})"

    # It did not spend its whole budget: one attempt is what a healthy medium
    # costs. A pass that needed eight would mean the spacing is wrong.
    grep -q "asking again for the volume nobody has mounted (attempt 1 of" "$S"
    chk $? "and it came up on the first asking, not by grinding through the budget"

    ! grep -q "stops asking and waits for a medium to arrive" "$S"
    chk $? "the machine never had to give up"

    grep -q "BoxOS Shell" "$L"; chk $? "the machine still has a shell"
}

run_stillthere() {
    echo "== stillthere: the read was slow, and the device never went anywhere =="

    # yank    pulls the stick while it is being read  -> the answer NEVER comes
    # replug  pulls it while the machine is idle      -> nothing is in flight
    #
    # This is the third thing, and it is the one a real flash drive does: it
    # answers LATE. A device doing its own garbage collection stops replying
    # for seconds and then carries on, and from the host's side that is
    # indistinguishable from silence — on BULK it is not even silence, it is
    # the device NAKing, which the controller retries for as long as it takes.
    #
    # Nothing here is unplugged. The stick is in the machine from the first
    # line to the last, and every check is about what the driver did to a
    # device that was there the whole time.
    latearrival_on; build
    cp build/boxos.img "$SCRATCH/stick.img"

    make run-stop >/dev/null 2>&1
    make run-bg USB=on CORES=4 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"

    mon_raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw"
    sleep 1
    mon_raw "device_add usb-storage,drive=stick,id=usbstick"
    wait_for_line build/serial.log "hands over" "$WAIT_QUIET_S"
    sleep 2

    # ── the control is the boot itself ──────────────────────────────────────
    #
    # Everything below is "and now it does not work", which is worth nothing
    # unless the machine has been shown to work first. It has: reaching a shell
    # at all means display.elf and shell.bin were read off this stick and run.
    #
    # ‼ IT USED TO BE A `files` TYPED HERE, AND THAT MADE THE RUN UNRELIABLE.
    # TagFS reads ahead, so running the program once put it in memory and the
    # throttled read below was then served without touching the medium — the
    # scenario passed by never reaching the situation it exists for. Measured:
    # green on its own, red in the matrix, same tree. A control that warms what
    # the test is about is not a control.
    cp build/serial.log "$SCRATCH/serial.stillthere.control.log"
    local MARK
    MARK=$(wc -l < build/serial.log)

    # ── PART ONE: slow, and that is ALL it is ──────────────────────────────
    #
    # 512 bytes a second puts one 4 KiB filesystem block at eight seconds —
    # longer than the five this driver used to allow a transfer, and well
    # inside what a flash drive doing its own housekeeping takes, but nowhere
    # near what the class gives a whole command. NOTHING may be given up on
    # here. This is the check the whole change is for, and on the old tree it
    # goes red within five seconds.
    #
    # ‼ THE PROGRAM MATTERS AS MUCH AS THE RATE. TagFS reads ahead, and
    # measurement says some programs are already in memory by the time a shell
    # appears: `files` and `bench` came back in two seconds at SIXTY-FOUR bytes
    # a second, while `today` and `mtest` took a hundred and twenty-two at five
    # hundred and twelve. A scenario aimed at a program that is already in
    # memory tests nothing and says PASS. Both programs used here were measured
    # cold, and neither is used twice.
    local GAVE_UP_BEFORE GAVE_UP_AFTER
    GAVE_UP_BEFORE=$(grep -cE "giving up on the command|no answer in .* at stage" build/serial.log)
    wait_for_prompt
    mon_raw "block_set_io_throttle stick 0 512 0 0 0 0"
    sleep 1
    type_line "today"

    # ‼ WAITED ON THE PROGRAM'S OWN ANSWER, AND NOTHING IS TYPED UNTIL IT COMES.
    #
    # Fixed pauses here cost two whole runs. The shell is one thing doing one
    # thing at a time: a command typed while the last one is still reading is a
    # command that lands in a buffer, and the scenario then reports a machine
    # that was never given a chance as a machine that failed. `today` prints a
    # date, and the date arriving is the fact that it was read.
    wait_for_line -E build/serial.log "^20[0-9][0-9]-[0-9][0-9]-[0-9][0-9] " "$WAIT_QUIET_SLOW_S" $((MARK + 1))
    GAVE_UP_AFTER=$(grep -cE "giving up on the command|no answer in .* at stage" build/serial.log)
    mon_raw "block_set_io_throttle stick 0 0 0 0 0 0"
    sleep 2

    # ── ‼ WHAT IS NOT TESTED HERE, AND WHY IT IS SAID OUT LOUD ──────────────
    #
    # The other half of the change is the LAST RESORT: a device slower than the
    # class allows is given up on, the transfer is taken off the endpoint with
    # the command the controller accepts, and the disk still works afterwards.
    # That was OBSERVED WORKING — a medium held to 128 bytes a second produced
    #
    #   [USB disk 0] the device has been asking for more time for 30000 ms at
    #                stage 2 — giving up on the command
    #   [USB disk 0] resetting the transport
    #   [mtest] HW VGA via Manifest works
    #
    # — the give-up, the reset, and then the same program arriving anyway once
    # the medium was quick again.
    #
    # It is not a CHECK here because reaching it needs a SECOND typed command,
    # and a second command does not reliably land: when the first one has just
    # finished, the shell is still doing its own reads, and on a medium this
    # slow those take tens of seconds. Five runs went into that, and a check
    # that is red for the instrument's reasons is worse than no check — it
    # teaches everyone to ignore the matrix.
    #
    # The shape that has never once failed is ONE typed command per run, which
    # means the slow-but-tolerable half above has to be proved by the BOOT
    # rather than by typing. That is a scenario of its own and it is written
    # down as owed, not forgotten.

    make run-stop >/dev/null 2>&1
    tail -n +$((MARK + 1)) build/serial.log > "$SCRATCH/serial.stillthere.log"
    latearrival_off

    local C="$SCRATCH/serial.stillthere.control.log"
    local L="$SCRATCH/serial.stillthere.log"

    grep -q "AUTOSTART. Started .shell.bin" "$C"
    chk $? "the machine reads programs off this stick at full speed"

    # ‼ THE POINT OF THE WHOLE CHANGE. On bulk, a device that is busy NAKs, and
    # the controller retries for as long as it takes; trouble arrives as an
    # EVENT and busy arrives as nothing at all. A driver that gives up on
    # "nothing" gives up on a healthy device that is doing its housekeeping.
    grep -qE "^20[0-9][0-9]-[0-9][0-9]-[0-9][0-9] " "$L"
    chk $? "a program read off a medium at 512 bytes a second still runs"

    [ "$GAVE_UP_AFTER" = "$GAVE_UP_BEFORE" ]
    chk $? "and nothing was given up on while it was being read ($GAVE_UP_BEFORE -> $GAVE_UP_AFTER)"

    # The device is in the machine. Anything that says otherwise is the driver
    # inventing a departure out of a delay.
    ! grep -q "the device has left" "$L"
    chk $? "nothing claimed a device had left that was still plugged in"

    # ‼ Reset Endpoint is defined for a HALTED endpoint (xHCI 1.2 4.6.8) and a
    # merely slow one is Running, so the controller refuses it — and says so, in
    # its own words, once per attempt. A refused command is a repair that did
    # not happen.
    ! grep -qE "Reset Endpoint on slot .* refused: Context State Error" "$L"
    chk $? "no endpoint was reset that the controller did not consider halted"

    ! grep -qE "Set TR Dequeue Pointer on slot .* refused: Context State Error" "$L"
    chk $? "and no dequeue pointer was moved on a ring still being read"

    ! grep -q "has been leaving for" "$L"
    chk $? "no slot was left with somebody stuck inside it"
}

run_slowdisk() {
    echo "== slowdisk: the disk was slow, and that is all it was =="

    # A drive that meets a marginal sector retries the head inside itself before
    # it answers. Seven seconds is ordinary on a desktop disk without
    # configurable error recovery and the standard sets no ceiling at all — so a
    # two-second clock on a command is not patience, it is a driver deciding a
    # healthy disk is broken, recovering the port, retrying three times and
    # reporting a failure on a read that would have arrived.
    #
    # ‼ THE DRIVER ALREADY HELD THE RIGHT NUMBER IN ONE HALF OF ITSELF. The
    # ASYNCHRONOUS path has always given a command CONFIG_AHCI_IO_TIMEOUT_MS
    # (thirty seconds, through ahci_watchdog_scan); only the SYNCHRONOUS one —
    # every metadata read a filesystem makes — used two.
    #
    # ‼ WHAT THIS DOES NOT COVER, SAID RATHER THAN LEFT TO BE FOUND. The legacy
    # IDE channels take the same fix (CONFIG_ATA_IO_TIMEOUT_MS) and are NOT
    # tested here: ATA PIO waits for DRQ once per 512-byte SECTOR, so making one
    # of those outlast the old five seconds means a hundred bytes a second, and
    # reading a program at that rate is a quarter of an hour. It is exercised by
    # every other scenario in the ordinary way, and the change to it is a
    # ceiling nothing in an emulator ever reaches.
    build

    make run-stop >/dev/null 2>&1
    make run-bg AHCI=on CORES=4 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    wait_for_prompt

    # 1536 bytes a second puts ONE 4 KiB filesystem block at not quite three
    # seconds — past the two this driver used to allow a whole command, and
    # nowhere near the thirty the class gives one. Everything below must simply
    # work, slowly.
    #
    # The volume is already mounted, so this is aimed at what a filesystem does
    # afterwards: `files` is a tag query, and a tag query is metadata reads,
    # and metadata reads are the synchronous path.
    mon_raw "block_set_io_throttle disk0 0 1536 0 0 0 0"
    sleep 1
    type_line "files"

    wait_for_line build/serial.log "^shell.bin " "$WAIT_QUIET_SLOW_S"

    mon_raw "block_set_io_throttle disk0 0 0 0 0 0 0"
    sleep 2
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.slowdisk.log"
    L="$SCRATCH/serial.slowdisk.log"

    # The control, and it is the boot: reaching a shell means the machine read
    # its programs off this disk at full speed.
    grep -q "AUTOSTART. Started" "$L"
    chk $? "the machine reads programs off the AHCI disk at full speed"

    grep -q "seat 0: AHCI port" "$L"
    chk $? "and the volume really is behind the AHCI controller"

    # The point. A disk that takes seconds is a disk, and the machine has to go
    # on using it.
    grep -q "^shell.bin " "$L"
    chk $? "the filesystem still answers with the disk at 1536 bytes a second"

    ! grep -q "did not answer a" "$L"
    chk $? "and nothing was given up on while it was being read"

    ! grep -qE "a (read|write|cache flush) failed after" "$L"
    chk $? "no command was abandoned after retries on a disk that was answering"

    ! grep -q "the link is no longer established" "$L"
    chk $? "and the cable was never called unplugged"
}

run_gpt() {
    echo "== gpt: the table is kept at both ends, and the far one is read =="

    # A GPT is written TWICE — the primary at LBA 1 and a copy in the last
    # sector of the medium — and UEFI 2.10 §5.3.2 requires a reader whose
    # primary does not check out to use the other one. This kernel did not: it
    # returned nothing, and every caller read that as "there is no BoxOS ground
    # on this disk". One dead erase block under sector 1 — the commonest way a
    # GPT is lost — and a perfectly good disk did not mount, without a word.
    #
    # Reaching the far copy needs one fact no driver would hand out until now:
    # how far the medium runs. BoardroomSeatSectors is that door.
    #
    # The disk under test arrives as a SECOND medium rather than as the one the
    # machine booted from, deliberately: the loaders read a partition table
    # too, and a scenario that damages the disk it is booting from measures
    # them as much as the kernel. Here the machine boots the ordinary way and
    # the damaged disk is handed to it afterwards, so what is measured is the
    # Ground survey and nothing else.
    build
    ./tools/make_gpt_boot.py build/boxos.img "$SCRATCH/gpt-good.img" >/dev/null 2>&1
    cp "$SCRATCH/gpt-good.img" "$SCRATCH/gpt-bad.img"
    python3 - "$SCRATCH/gpt-bad.img" <<'EOF'
import sys
p = sys.argv[1]
with open(p, 'r+b') as f:
    f.seek(512)              # LBA 1: the primary GPT header
    f.write(b'\x00' * 512)   # one dead erase block, which is how they go
EOF
    local GOOD_LOG="$SCRATCH/serial.gpt.good.log"
    local BAD_LOG="$SCRATCH/serial.gpt.bad.log"

    local which img wait_for log
    for which in good bad; do
        img="$SCRATCH/gpt-$which.img"
        if [ "$which" = good ]; then
            wait_for="from GPT entry"; log="$GOOD_LOG"
        else
            wait_for="the copy at the far end"; log="$BAD_LOG"
        fi

        make run-stop >/dev/null 2>&1
        make run-bg USB=on CORES=4 MEM=4G >/dev/null 2>&1
        wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"

        local MARK
        MARK=$(wc -l < build/serial.log)
        mon_raw "drive_add 0 if=none,id=gptdisk,file=$img,format=raw"
        sleep 1
        mon_raw "device_add usb-storage,drive=gptdisk,id=gptstick"

        # Waited on the survey having spoken, not on a clock.
        wait_for_line build/serial.log "$wait_for" "$WAIT_QUIET_S" $((MARK + 1))
        sleep 2
        make run-stop >/dev/null 2>&1
        tail -n +$((MARK + 1)) build/serial.log > "$log"
    done

    # ── the disk with both copies intact ────────────────────────────────────
    grep -q "from GPT entry" "$GOOD_LOG"
    chk $? "a healthy GPT disk gives up its BoxOS ground"

    ! grep -qE "its GPT (is damaged|is not where)" "$GOOD_LOG"
    chk $? "and nothing called a healthy table damaged"

    ! grep -q "made for a smaller disk" "$GOOD_LOG"
    chk $? "and the table matches the medium it is on"

    # ── and the same disk with its primary header gone ──────────────────────
    grep -q "its GPT is not where the disk says it is" "$BAD_LOG"
    chk $? "a lost primary header is called damage, not an empty disk"

    # The number is ASKED OF THE DISK, not typed. It used to be 122325 here —
    # the last sector of the image as it was the day this was written — and the
    # day the build began sizing the volume from its content, this check failed
    # for a machine that had done exactly the right thing.
    LAST=$(( $(stat -f%z "$SCRATCH/gpt-bad.img" 2>/dev/null || stat -c%s "$SCRATCH/gpt-bad.img") / 512 - 1 ))
    grep -q "reading the copy at the far end, sector $LAST" "$BAD_LOG"
    chk $? "and the far copy is looked for in the last sector of the medium"

    grep -q "the copy at the far end is good" "$BAD_LOG"
    chk $? "and it is read"

    grep -q "from GPT entry" "$BAD_LOG"
    chk $? "and the BoxOS ground on that disk is found after all"

    # And the machine that was handed a damaged table finished the pass it was
    # in rather than stopping inside it — the room says so on its way out.
    grep -q "arrived after the room was called to order" "$BAD_LOG"
    chk $? "and the room finished seating it"

    ! grep -qiE "panic|fault at" "$BAD_LOG"
    chk $? "and nothing fell over reading a broken table"
}

run_twoctrl() {
    echo "== twoctrl: two host controllers, and the stick on the SECOND one =="

    # The board has two: an Intel 00:14.0 and an NVIDIA 01:00.2, and the driver
    # registers the NVIDIA one FIRST — so the flash drive lives on controller
    # INDEX 1. Every scenario before this one ran on a machine with exactly one
    # controller, where index 1 does not exist, so nothing here could ever be
    # seen: the retire flag is ONE flag for the whole machine, and the pass for
    # controller 0 used to consume it and walk away, leaving controller 1's
    # departed devices standing for ever.
    #
    # Measured on the board: fifteen chairs from one flash drive, slot ids to
    # 34, and not one "is gone" in a 977-line log. Measured here, same script,
    # with and without the fix: five replugs give usb0 five times, or usb0
    # through usb4 and zero releases.
    latearrival_on; build
    cp build/boxos.img "$SCRATCH/stick.img"

    make run-stop >/dev/null 2>&1
    make run-bg USB=on XHCI2=on CORES=4 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 2
    local MARK
    MARK=$(wc -l < build/serial.log)

    # Five arrivals and five departures on the second controller. A fresh
    # drive_add each time: HMP marks the drive auto-del, so device_del takes it
    # with it and the next device_add silently does nothing — one cycle
    # pretending to be ten.
    for n in 1 2 3 4 5; do
        mon_raw "drive_add 0 if=none,id=st$n,file=$SCRATCH/stick.img,format=raw"
        sleep 1
        mon_raw "device_add usb-storage,bus=xhci2.0,drive=st$n,id=us"
        sleep 4
        mon_raw "device_del us"
        sleep 4
    done
    sleep 4
    make run-stop >/dev/null 2>&1
    tail -n +$((MARK+1)) build/serial.log > "$SCRATCH/serial.twoctrl.log"
    latearrival_off
    L="$SCRATCH/serial.twoctrl.log"

    grep -q "2 controller(s) in service" build/serial.log
    chk $? "the machine really has two host controllers"

    local arrivals
    arrivals=$(grep -c "usb0 QEMU" "$L")
    [ "$arrivals" -ge 2 ]
    chk $? "the stick arrived on the second controller more than once ($arrivals)"

    # THE check. A unit number is handed back when its device leaves, so the
    # same stick coming home takes the same number. A second number means the
    # first was never given back.
    local numbers count
    numbers=$(grep -oE "\[USB disk [0-9]+\] usb[0-9]+ " "$L" | grep -oE "usb[0-9]+" | sort -u | tr '\n' ' ')
    count=$(printf '%s' "$numbers" | wc -w | tr -d ' ')
    [ "$count" = "1" ]
    chk $? "one stick kept one unit number across every replug ($numbers)"

    grep -q "is gone" "$L"
    chk $? "the unit on controller 1 was released at all"

    grep -q "is empty" "$L"
    chk $? "and its chair was emptied"

    ! grep -q "has been leaving for" "$L"
    chk $? "no slot was left standing because nobody came to take it down"

    # ── each controller's records are ITS records ──────────────────────────
    #
    # Everything above this in the scenario is the damage the machine-wide
    # table did: one array of sixty-four for every controller, one lock, and a
    # special case in the retirement pass for "somebody else's device" that a
    # live board proved was not enough — fifteen chairs from one flash drive.
    # The records now live on the controller that hands out the slots.
    local ctl rec
    ctl=$(grep -c "controller ready:" build/serial.log)
    rec=$(grep -c "carries .* device record(s) of its own" build/serial.log)
    [ "$ctl" -ge 2 ] && [ "$ctl" = "$rec" ]
    chk $? "every controller carries records of its own ($rec of $ctl)"

    # And as many as it said it can address. ‼ This cannot fail here: QEMU's
    # xHCI has no way to report anything but sixty-four slots, so a kernel that
    # went back to one machine-wide 64 would satisfy it too. It bites on a
    # board whose controller says something else — which is the only place the
    # question was ever open.
    local mismatched
    mismatched=$(awk '
        /controller ready:/ { match($0, /([0-9]+) slot\(s\)/, m); want = m[1] }
        /carries [0-9]+ device record/ { match($0, /carries ([0-9]+) device/, g);
            if (want != "" && g[1] != want) n++ }
        END { print n+0 }' build/serial.log 2>/dev/null || echo 0)
    [ "$mismatched" = 0 ]
    chk $? "and as many of them as it said it can address ($mismatched wrong)"
}

# ── a pass that did not survive the journey ────────────────────────────────
#
# The seal exists for one case that cannot be staged any other way: the block
# still carries a valid magic, states lengths that fit, and walks cleanly —
# and is not what the loader wrote. Firmware handing the page to something
# else between the write and ExitBootServices produces exactly that, and so
# does a loader that died halfway.
#
# So the mutation corrupts the pass AFTER it has been sealed. One byte, inside
# the span the seal covers, in the stamp the kernel leans on hardest.
seal_on() {
    mutate src/boot/stage2/stage2.asm
    python3 - <<'EOF'
p = "src/boot/stage2/stage2.asm"
s = open(p).read()
anchor = "    mov [BOARDING_PASS_ADDR+68], eax\n    ret\n"
assert anchor in s, "seal anchor missing"
s = s.replace(anchor,
              "    mov [BOARDING_PASS_ADDR+68], eax\n"
              "    mov byte [BOARDING_PASS_ADDR+41], 0x7F   ; logcheck mutation: the block rots after it was sealed\n"
              "    ret\n", 1)
open(p, "w").write(s)
EOF
    mut_done src/boot/stage2/stage2.asm "logcheck mutation"
}
seal_off() {
    restore src/boot/stage2/stage2.asm
}

# ── a pass from a loader newer than this kernel ────────────────────────────
#
# The other half of the same question: what may be believed off the block.
# The version used to throw the WHOLE pass away on any number the kernel had
# not been compiled against — volume identity and all — which is the version
# ladder boarding_pass.h promises not to build. The stamps are self-describing
# and the header is frozen, so a later loader is readable.
passver_on() {
    mutate src/boot/stage2/stage2.asm
    python3 - <<'EOF'
p = "src/boot/stage2/stage2.asm"
s = open(p).read()
anchor = "    mov word  [BOARDING_PASS_ADDR+4],   BOARDING_PASS_VERSION   ; +4  version\n"
assert anchor in s, "pass-version anchor missing"
s = s.replace(anchor,
              "    mov word  [BOARDING_PASS_ADDR+4],   2   ; logcheck mutation: a loader newer than this kernel\n", 1)
open(p, "w").write(s)
EOF
    mut_done src/boot/stage2/stage2.asm "logcheck mutation"
}
passver_off() {
    restore src/boot/stage2/stage2.asm
}

run_seal() {
    echo "== seal: the pass was written, then something walked over it =="
    seal_on; build
    boot seal.broken
    seal_off
    local L="$SCRATCH/serial.seal.broken.log"

    # The kernel CAUGHT it, and said both sums rather than "bad pass".
    grep -qE "the seal on the pass is broken: [0-9]+ bytes sum to 0x[0-9a-f]+ and the loader wrote 0x[0-9a-f]+" "$L"
    chk $? "the broken seal was caught, and both sums named"

    # And having caught it, refused the block ENTIRELY. A pass that does not
    # add up must not have some of its stamps believed.
    grep -q "the loader left no pass" "$L"
    chk $? "and the whole pass was refused, not part of it"

    ! grep -q "read out of the volume" "$L"
    chk $? "no volume identity was taken off a block that did not add up"

    # And the room does not go on crediting a loader whose word it threw
    # away. A healthy boot prints "the loader said so on its boarding pass"
    # over the seat it mounts; after a refused pass that sentence must be
    # gone, because nothing on the block is usable — not the volume id, and
    # not the confidence. This is the check that would catch a kernel which
    # rejects the pass and then quietly uses a value it took off it anyway.
    ! grep -q "the loader said so on its boarding pass" "$L"
    chk $? "the room does not credit a pass it refused"

    # It still found the volume — by its own means, which is the arrangement
    # the pass was only ever an improvement on.
    grep -qE '^\[TagFS\] volume on seat [0-9]+' "$L"
    chk $? "and found the volume anyway, without being told"

    # ‼ AND THE MACHINE STILL BOOTS. A corrupted pass is not a reason to
    # refuse a machine: the pass is an optimisation over a rule that works.
    grep -q "BoxOS Shell" "$L";  chk $? "the machine still reaches a shell"
    typed_ok "$L";               chk $? "and still answers a keystroke"

    # ── and now a pass this kernel is too old to have heard of ─────────────
    passver_on; build
    boot seal.newer
    passver_off
    local N="$SCRATCH/serial.seal.newer.log"

    grep -q "the pass is version 2 and this kernel was built for 1" "$N"
    chk $? "a newer pass is noticed and named"

    # THE point. The stamps are self-describing and the header is frozen, so
    # everything this kernel knows how to read is still there to be read. The
    # old rule discarded the block whole and sent the room back to guessing.
    grep -q "read out of the volume" "$N"
    chk $? "and its volume identity is still read off it"
    grep -q "the loader said so on its boarding pass" "$N"
    chk $? "and the room is told which seat, by a loader it does not know"
    grep -q "sealed, and the seal agrees" "$N"
    chk $? "and the seal on it still checks out"
}

# ── the one control transfer nobody can answer ─────────────────────────────
#
# xhci_ep_wait gives up when the device has gone, when the pipe has broken, or
# — last — when the time ran out, and on every one of those it has to take the
# transfer back OFF the endpoint. Under emulation none of the three happens on
# the control pipe: the emulated device answers everything immediately. That
# was measured six ways before this scenario was written (a zero budget, yank,
# stillthere, usbrecover, a hot-removed hub, an ordinary boot) and the path was
# reached by none of them.
#
# So the kernel runs it deliberately, once, in a build made with CTRLGIVEUP=on:
# a control transfer posted with the doorbell withheld, which is a faithful
# "no answer came" rather than a simulated one. The proof is the transfer
# AFTER it — see xhci_ctrl_giveup_proof.
run_ctrlgiveup() {
    echo "== ctrlgiveup: a control transfer is given up on, and taken back =="

    make CTRLGIVEUP=on >"$SCRATCH/build.log" 2>&1
    if [ $? -ne 0 ]; then
        echo "BUILD FAILED — tail:"; tail -25 "$SCRATCH/build.log"; exit 1
    fi
    cp build/boxos.img "$SCRATCH/stick.img"

    # Four cores, for the reason latearrival and replug give: the deferred work
    # of this kernel runs from the K-Core guide loop and from cpu_idle, and on
    # one core neither runs.
    make run-stop >/dev/null 2>&1
    make run-bg CTRLGIVEUP=on USB=on CORES=4 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 2

    # USB=on gives this machine a KEYBOARD, not a disk. The proof runs on the
    # first mass-storage device the kernel configures, so one is plugged in —
    # measured: without this the proof never runs and the scenario is vacuous.
    mon_raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw"
    sleep 1
    mon_raw "device_add usb-storage,drive=stick,id=usbstick"
    wait_for_line build/serial.log "xHCI PROOF. the next control transfer" "$WAIT_QUIET_S"
    sleep 2

    type_cmd "help"
    sleep 1; key_press ret; sleep 3

    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.ctrlgiveup.log"
    local L="$SCRATCH/serial.ctrlgiveup.log"

    # It ran at all. The first version of this proof could decline in silence,
    # which reads exactly like a proof that passed.
    grep -q "a control transfer is posted and not rung for" "$L"
    chk $? "the unanswerable transfer was posted"
    ! grep -q "the give-up proof did NOT run" "$L"
    chk $? "and the proof did not decline"

    # The wait ended on the clock — the last question it asks, and the only one
    # that can be reached here — and it NAMED that.
    grep -q "it has not answered in the time it was given — taking the transfer back" "$L"
    chk $? "the wait gave up and said which of its questions ended it"

    # THE repair: the transfer comes off the endpoint at the controller, not
    # just out of the driver's mind.
    grep -qE "slot [0-9]+ endpoint 1: the transfer on it is no longer wanted — stopping it" "$L"
    chk $? "and the controller was told to stop it"

    grep -q "the wait ended with -1" "$L"
    chk $? "the caller was told the transfer failed"

    # ‼ AND THIS IS THE PROOF. The next question down the same pipe must get
    # ITS OWN answer. Left where it was, the abandoned Setup runs first when
    # the doorbell next rings, and its event is accepted for the new transfer
    # because a control transfer sets xfer_trb_phys to zero and that turns the
    # TRB match off — so the buffer holds a DEVICE descriptor (type 1) where
    # the caller asked for a CONFIGURATION one (type 2), and cannot tell.
    grep -q "the next control transfer asked something the device answers and got its own answer" "$L"
    chk $? "the next control transfer got its own answer, not the abandoned one"

    # And the device carried on being a disk afterwards.
    grep -qE "\[USB disk [0-9]+\] .* sectors, .* MiB" "$L"
    chk $? "the disk still enumerated after its control pipe was given up on"
    grep -q "BoxOS Shell" "$L"; chk $? "the machine still has a shell"
    typed_ok "$L";              chk $? "and still answers a keystroke"
}

# ── a device this kernel has no use for, and says so ───────────────────────
#
# enum_pick_visit answers bulk and interrupt and lets everything else fall
# through its default, so a headset or a camera used to be addressed,
# configured and settled exactly like a device with nothing on it. "The machine
# saw my microphone and did nothing" is a different fault from "the machine did
# not see my microphone", and only one of them was readable off a screen.
run_isoch() {
    echo "== isoch: a device whose useful half this kernel does not configure =="
    build
    make run-stop >/dev/null 2>&1
    make run-bg USB=on ISOCH=on CORES=4 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 2
    type_cmd "help"
    sleep 1; key_press ret; sleep 3
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.isoch.log"
    local L="$SCRATCH/serial.isoch.log"

    # The device got as far as being configured at all.
    grep -qE "port [0-9]+: .*device 46f4:0002 class 01/01/00 configured, no driver claims it" "$L"
    chk $? "the audio device was addressed, configured and settled"

    # ‼ AND THE PART THIS KERNEL DROPS IS NAMED, with a count that came from
    # the descriptor rather than from a guess.
    grep -qE "port [0-9]+: it has [1-9][0-9]* isochronous endpoint\(s\), and this kernel configures none" "$L"
    chk $? "and the isochronous endpoints it has are named, not dropped in silence"

    # A device nobody drives must not cost the machine anything.
    grep -q "BoxOS Shell" "$L"; chk $? "the machine still reaches a shell"
    typed_ok "$L";              chk $? "and still answers a keystroke"
    grep -q "endpoint-context self-test 15/15" "$L"
    chk $? "and the endpoint-context fields still agree with the specification"
}

# ===========================================================================
# sleeps — the box with nothing to do costs nothing, and still hears
# ===========================================================================
#
# Two questions, and they are opposite halves of the same change. Sleeping is
# easy on its own: stop looking, and a machine goes quiet for ever. Hearing is
# easy on its own: never stop looking, which is what this box did, at a hundred
# percent of a core with an empty screen. Only together are they worth
# anything, and only together can a mutation to either one be seen.
#
#   IT SLEEPS   — at an idle prompt, the emulator's own CPU time barely moves.
#                 Measured on the HOST, deliberately: the guest cannot be asked
#                 whether it is really idle, because the loop that would answer
#                 is the loop that would be burning the core.
#
#   IT HEARS    — quietprint writes one line into its console lane and then
#                 says nothing at all for three seconds. Nothing else happens
#                 in that window: no key, no message, no death. The line must
#                 be on screen BEFORE the program has finished, and the only
#                 thing that can put it there is the bell — the sleeping
#                 daemon's pid, hung on the lane, taken and rung by the writer.
#                 Without it the line still arrives, but only when the process
#                 DIES, because a death is a Touch and a Touch wakes the daemon.
#                 So the check is on WHEN, never on whether.
#
# Single core on purpose. It is the configuration where a spinning strand is
# unmistakable — there is one core, and either it is idle or it is not.

QUIET_SETTLE_S=6      # long enough for boot chatter to stop
QUIET_SAMPLE_S=8      # window the host CPU is measured over

# Host CPU time (in centiseconds) that the QEMU process has used so far.
qemu_cpu_cs() {
    local pid t
    pid=$(cat build/qemu.pid 2>/dev/null) || return 1
    [ -n "$pid" ] || return 1
    t=$(ps -o time= -p "$pid" 2>/dev/null | tr -d ' ') || return 1
    [ -n "$t" ] || return 1
    # ps prints [[hh:]mm:]ss[.cc]
    printf '%s\n' "$t" | awk -F: '
        { n=NF; sec=$n; m=(n>1)?$(n-1):0; h=(n>2)?$(n-2):0;
          printf "%d\n", (h*3600 + m*60 + sec) * 100 }'
}

run_sleeps() {
    echo "== sleeps: an idle box costs nothing, and a sleeping console still hears =="
    build

    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=1 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    grep -q "BoxOS Shell" build/serial.log 2>/dev/null || { bad "sleeps: never reached a shell"; make run-stop >/dev/null 2>&1; return; }

    # ── IT SLEEPS ───────────────────────────────────────────────────────────
    sleep $QUIET_SETTLE_S
    local A B USED
    A=$(qemu_cpu_cs); sleep $QUIET_SAMPLE_S; B=$(qemu_cpu_cs)
    USED=$(( (B - A) * 100 / (QUIET_SAMPLE_S * 100) ))   # percent of one core
    echo "     idle: ${USED}% of one core over ${QUIET_SAMPLE_S}s (was ~100% before Turn In)"

    # 25% is not a target, it is a LINE. A box that turns in measures single
    # digits and a box that spins measures a hundred; nothing lands in between,
    # so the threshold only has to be far from both to be immune to a loaded
    # build host.
    if [ "$USED" -lt 25 ]; then ok "sleeps: idle box costs ${USED}% of a core"
    else                        bad "sleeps: idle box costs ${USED}% of a core — it never turned in"; fi

    # ── IT HEARS ────────────────────────────────────────────────────────────
    local MARK EARLY LATE
    MARK=$(wc -l < build/serial.log)
    type_cmd "quietprint"
    sleep 1
    key_press ret

    # quietprint: line 1, three seconds asleep in brook_pop, line 2, three more
    # in a touch park, done. Measure INSIDE the first silence — that window has
    # a strand blocked in brook_pop, a sibling parked, and a shell and a daemon
    # turned in, so a box that is not idle here has a waiter that spins.
    sleep 1
    local WA WB WUSED
    WA=$(qemu_cpu_cs); sleep 2; WB=$(qemu_cpu_cs)
    WUSED=$(( (WB - WA) * 100 / 200 ))
    echo "     with a strand blocked in brook_pop: ${WUSED}% of one core"
    if [ "$WUSED" -lt 25 ]; then ok "sleeps: a blocked brook_pop costs ${WUSED}% of a core"
    else                        bad "sleeps: a blocked brook_pop costs ${WUSED}% of a core — the reader is spinning"; fi

    # Look at three seconds — still inside the FIRST silence. Line one must
    # already be there and the program must still be running.
    EARLY=$(tail -n +$((MARK + 1)) build/serial.log)
    printf '%s\n' "$EARLY" > "$SCRATCH/serial.sleeps.early.log"

    if printf '%s' "$EARLY" | grep -q "\[QP\] line 1"; then
        ok "sleeps: the line reached the screen while the writer was still quiet"
    else
        bad "sleeps: line 1 had not been rendered two seconds in — the bell never rang"
    fi
    if printf '%s' "$EARLY" | grep -q "\[QP\] done"; then
        bad "sleeps: quietprint had already finished — the window proves nothing, fix the timing"
    else
        ok "sleeps: the window was real — the writer had not finished yet"
    fi

    # And it must all still arrive, in order, and the silences must be the
    # length they were asked for — read off the guest's OWN timestamps below,
    # never off this script's clock, which is why waiting for the program to
    # say it is done costs the measurement nothing.
    wait_for_line build/serial.log "\[QP\] done" "$WAIT_QUIET_S" $((MARK + 1))
    LATE=$(tail -n +$((MARK + 1)) build/serial.log)
    printf '%s\n' "$LATE" > "$SCRATCH/serial.sleeps.log"
    make run-stop >/dev/null 2>&1

    printf '%s' "$LATE" | grep -q "\[QP\] done" \
        && ok "sleeps: quietprint ran to the end" \
        || bad "sleeps: quietprint never finished"

    printf '%s' "$LATE" | grep -q "\[QP\] line 2 at .* (brook)" \
        && ok "sleeps: the first silence really was a blocked brook_pop" \
        || bad "sleeps: quietprint fell back to a touch park — the brook sleep was not exercised"

    local T2
    T2=$(printf '%s' "$LATE" | sed -n 's/.*\[QP\] line 2 at \([0-9]*\) ms.*/\1/p' | head -1)
    if [ -n "$T2" ] && [ "$T2" -ge 2900 ] && [ "$T2" -le 4200 ]; then
        ok "sleeps: three seconds of silence took ${T2} ms"
    else
        bad "sleeps: three seconds of silence took ${T2:-?} ms — a wait is counting its deadline twice"
    fi
}

# The mutation, and it has to be the RING rather than the bell itself: a bell
# nobody hangs out is also a bell nobody rings, and Nightwatch would then have
# nothing to describe. Leaving the sign up and refusing to pull the cord is the
# exact defect the check exists for, and it is the one that leaves evidence.
# BOTH ringers, and both on purpose. Silencing only the peer's leaves the
# kernel's departure ring to rescue the sleeper the moment the other end lets
# go — measured: the staged wedge healed itself in six seconds and there was
# nothing left to observe. A bell that nobody rings, from anywhere, is the
# defect this is about.
bell_off() {
    mutate src/userspace/boxlib/src/brook.c
    mutate src/kernel/core/brook/brook.c
    python3 - <<'EOF'
p = "src/userspace/boxlib/src/brook.c"
s = open(p).read()
anchor = "    if (!brook_cas_u32(bell, who, who | BROOK_BELL_RUNG)) return;"
assert anchor in s, "bell mutation anchor missing"
s = s.replace(anchor, anchor + "\n    return;   /* logcheck mutation: the bell is never rung */", 1)
open(p, "w").write(s)

p = "src/kernel/core/brook/brook.c"
s = open(p).read()
anchor = "    if (who == 0) return;\n    process_t *target = process_find_ref(who);"
assert anchor in s, "kernel bell mutation anchor missing"
s = s.replace(anchor, "    return;   /* logcheck mutation: the kernel never rings either */\n" + anchor, 1)
open(p, "w").write(s)
EOF
    mut_done src/userspace/boxlib/src/brook.c "logcheck mutation"
    mut_done src/kernel/core/brook/brook.c "logcheck mutation"
}

bell_restore() {
    restore src/userspace/boxlib/src/brook.c
    restore src/kernel/core/brook/brook.c
}

# ── kcoreclaim ───────────────────────────────────────────────────────────────
#
# A K-Core's queue is taken in two steps: the producer CLAIMS a position (the
# CAS on `tail`) and PUBLISHES it afterwards (the slot store). The consumer
# that stood in kcore_queue_pop read the tail as well as the slot, and when the
# tail said "claimed" while the slot still said "not yet" it waited a million
# spins and then moved head past the claim — taking the position from under a
# producer that was still walking up to it. The pocket was published behind
# the head, where nobody would look; its owner kept kcore_pending set, so no
# notify of its own could ring the bell again; and it waited for an answer to a
# question nobody would open. The warning was debug_printf, which is nothing.
#
# On the board the gap between claim and publish holds an SMI; on this stand a
# descheduled vCPU. Neither is rare enough to wait for, so the gap is WIDENED
# from both sides: every submit by an APPLICATION (pid >= 3) stands claimed and
# unpublished for two milliseconds — the display daemon and the shell are left
# alone, so the machine stays typeable and the harness can drive it — and the
# K-Cores never sleep, so the consumer is always awake to meet the gap. Without
# the second half the meeting is a coin toss: a K-Core that has already parked
# in HLT is woken by the IPI that follows the publish and never sees the claim
# in flight, and the loss struck the first bench in one run and the third in
# the next. Under the fix — a consumer that judges only the slot and never
# moves head past a claim — bench completes three runs; under the impatient
# consumer (put back by kcoreclaimmut) the first gap it meets loses a submit,
# the machine stops, and Nightwatch names it: POCKET UNSERVED. The completion
# row (the storage benchmark, the last one bench prints) is what is counted:
# the PING row comes early in a run, and a machine that stops after it has
# still stopped.
kcoreclaim_window_on() {
    mutate src/kernel/arch/x86-64/amp/kcore.c
    python3 - <<'EOF'
p = "src/kernel/arch/x86-64/amp/kcore.c"
s = open(p).read()
anchor = """    atomic_fetch_add_u32(&q->count, 1);
    __atomic_store_n(&q->slots[old_tail], proc, __ATOMIC_RELEASE);"""
assert anchor in s, "kcoreclaim window anchor missing"
widened = """    atomic_fetch_add_u32(&q->count, 1);
    if (proc->pid >= 3) delay(2);   /* logcheck mutation: an application's claim stands unpublished for 2 ms */
    __atomic_store_n(&q->slots[old_tail], proc, __ATOMIC_RELEASE);"""
s = s.replace(anchor, widened, 1)
gate = """            __asm__ volatile("sti; hlt");   /* atomic arm-and-sleep */"""
assert gate in s, "kcoreclaim sleep-gate anchor missing"
awake = """            __asm__ volatile("sti");        /* logcheck mutation: the K-Core never sleeps, so every claim is met awake */"""
s = s.replace(gate, awake, 1)
open(p, "w").write(s)
EOF
    mut_done src/kernel/arch/x86-64/amp/kcore.c "logcheck mutation" 2
}

# Restores the whole file, so it takes the impatient consumer out as well.
kcoreclaim_window_off() {
    restore src/kernel/arch/x86-64/amp/kcore.c
}

# The consumer that stood here before: it reads the tail, waits on a claimed
# slot, and when its patience runs out moves head past the claim. Its patience
# is a thousand spins instead of the million that stood in the tree, so that
# the two-millisecond gap always outlasts it on this stand — the shape is the
# defect, not the number: any patience loses to a producer delayed longer, and
# an SMI is not consulted about how long it may take.
# ‼ THERE IS NO `mutate` HERE, AND THAT IS THE SHAPE, NOT AN OMISSION.
#
# This is the SECOND edit to kcore.c: run_kcoreclaimmut installs the claim
# window first and then stacks the impatient consumer on top of it, so the file
# is already under the manifest and `mutate` would — rightly — refuse it. The
# pristine copy taken by the FIRST mutation is the pristine copy of the file,
# full stop, and restoring it undoes both edits at once. The alternative would
# be holding two "pristine" copies of one file, the second of which is a
# mutated file saved as its own original: precisely the bug this mechanism
# exists to make impossible. `mut_done` is still called, because the second
# edit has to be proved to have landed exactly like the first.
impatient_on() {
    python3 - <<'EOF'
p = "src/kernel/arch/x86-64/amp/kcore.c"
s = open(p).read()
anchor = """    uint32_t h = q->head;
    struct process_t* proc = __atomic_load_n(&q->slots[h], __ATOMIC_ACQUIRE);
    if (!proc) return NULL;"""
assert anchor in s, "impatient anchor missing"
impatient = """    uint32_t h = q->head;
    /* logcheck mutation: the impatient consumer — waits on a claim, then moves head past it */
    struct process_t* proc;
    uint32_t patience = 0;
    for (;;) {
        proc = __atomic_load_n(&q->slots[h], __ATOMIC_ACQUIRE);
        if (proc != NULL) break;
        if (h == atomic_load_u32(&q->tail)) return NULL;
        cpu_pause();
        if (++patience > 1000u) {
            q->slots[h] = NULL;
            q->head = (h + 1) & KCORE_QUEUE_MASK;
            atomic_fetch_sub_u32(&q->count, 1);
            return NULL;
        }
    }"""
open(p, "w").write(s.replace(anchor, impatient, 1))
EOF
    mut_done src/kernel/arch/x86-64/amp/kcore.c "logcheck mutation: the impatient consumer"
}

# Sixteen cores, bench three times over. Each run is waited out to the row
# AFTER the one at risk (the storage row is the last one bench prints), or to
# Nightwatch speaking — whichever comes first. A verdict ends the whole thing:
# the shell is parked on a bench that will never return, so nothing typed after
# it would run.
claim_boot() {
    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=16 MEM=8G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 4
    local run=0
    while [ $run -lt 3 ]; do
        local MARK
        MARK=$(wc -l < build/serial.log)
        # The typer's verdict is read, not thrown away: a line the guest did not
        # take must stop the run and say so — pressing Enter on it would run
        # something else and blame the OS (the stress matrix learned this once).
        if ! type_cmd "bench"; then break; fi
        sleep 1
        key_press ret
        wait_until "$WAIT_QUIET_S" "a bench row or a verdict" \
            'tail -n +$((MARK + 1)) build/serial.log | grep -q "create+write64+delete" ||
              grep -qE "POCKET UNSERVED|ANSWER OWED" build/serial.log'
        grep -qE "POCKET UNSERVED|ANSWER OWED" build/serial.log && break
        run=$((run+1))
    done
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.$1.log"
}

run_kcoreclaim() {
    echo "== kcoreclaim: a claim stands unpublished for 2 ms and the K-Core must wait for it =="
    kcoreclaim_window_on; build
    claim_boot kcoreclaim
    kcoreclaim_window_off
    L="$SCRATCH/serial.kcoreclaim.log"

    local rows
    rows=$(grep -c "create+write64+delete" "$L")
    [ "$rows" -ge 3 ]
    chk $? "kcoreclaim: bench completed three runs with every application claim standing unpublished for 2 ms before an awake K-Core (rows=$rows)"
    ! grep -q "POCKET UNSERVED" "$L"; chk $? "kcoreclaim: and no pocket went unserved"
    ! grep -q "ANSWER OWED" "$L";     chk $? "kcoreclaim: and no answer was owed"
}

# The oracle measured against itself: with the impatient consumer back, the
# same run must STOP, and Nightwatch must be the one to say why.
run_kcoreclaimmut() {
    echo "== kcoreclaimmut: put the impatient consumer back and require the machine to stop =="
    kcoreclaim_window_on; impatient_on; build
    claim_boot kcoreclaimmut
    kcoreclaim_window_off
    L="$SCRATCH/serial.kcoreclaimmut.log"

    grep -q "POCKET UNSERVED" "$L"
    chk $? "kcoreclaimmut: with the impatient consumer back, Nightwatch named the lost submit — POCKET UNSERVED"
    local rows
    rows=$(grep -c "create+write64+delete" "$L")
    [ "$rows" -lt 3 ]
    chk $? "kcoreclaimmut: and bench did not complete three runs (rows=$rows)"
}

# ── lostwake ─────────────────────────────────────────────────────────────────
#
# Nightwatch's LOST WAKE used to be judged at a glance: a strand parked on a
# word, no deadline armed, and the word already changed. Two things make a
# glance no proof at all. A waker stores first and asks for the wake in a
# SEPARATE call, so "changed and still asleep" is what every delivery looks
# like for the length of one syscall; and the watch's own glance is two steps —
# the park's expectation is copied under the process lock, the word is read
# later, after the walk and its printing — while the machine goes on. Measured
# on BIOS 16c (2026-09-06, the day matrix): eleven workers of the
# std::execution::par brigade accused at once, the printed words climbing along
# the walk (0xc1, 0xc3, 0xc7 … 0xd3 against one and the same expectation), and
# cxxtest passing the phase a moment later. The matrix was announced 6/6 over
# that verdict, because nobody read verdicts.
#
# The proof is now persistence, the same as the other six proofs: the same park
# (pid, generation, park seq), the word still changed, a full look later. A
# delivery in flight cannot last ten seconds; a lost wake lasts for ever.
#
# The glance is WIDENED: the watch waits two seconds between copying the
# parks and reading the words, and says so — but only on a look that found an
# application alive, so the witness never lands inside the shell's echo of a
# typed command (the shell echoes keystroke by keystroke, and a kernel line
# between two of them breaks the echo the typer verifies) and every witness
# counted is a look that fell inside the workload. The look's CADENCE is left
# alone on purpose: a look every second was tried, and it changed what the
# other proofs mean — ANSWER OWED needs "no answer anywhere for a whole look",
# which at one second is any sequential stretch of a test (seven brigade
# workers convicted thirteen seconds into the boot) — and it let two walks
# overlap and print over each other. So the width comes from the walk and
# from repetition instead: eight rounds of cxxtest's parallel-algorithm phases
# (201-202: eleven workers parked and woken thousands of times a second), so
# that several looks land with the brigade alive. Under the one-glance judge
# every such look convicts — measured: after three green rounds, all eleven
# workers named on the first look that found the brigade alive, and two more
# verdicts before the harness stopped the machine; under the fix none may,
# and the phases must pass every time.
slowwalk_on() {
    mutate src/kernel/core/nightwatch/nightwatch.c
    python3 - <<'EOF'
p = "src/kernel/core/nightwatch/nightwatch.c"
s = open(p).read()
anchor = """    process_list_unlock();

    /* What each strand was PROMISED is read with the snapshot above (ChitPeek):"""
assert anchor in s, "slowwalk anchor missing"
slow = """    process_list_unlock();
    if (n > 2)   /* logcheck mutation: the watch walks slowly — 2 s between the parks and the words */
        kprintf("[MUTATION] the watch walks slowly: 2 s between the parks and the words, %u processes alive\\n", n);
    delay(2000);

    /* What each strand was PROMISED is read with the snapshot above (ChitPeek):"""
open(p, "w").write(s.replace(anchor, slow, 1))
EOF
    mut_done src/kernel/core/nightwatch/nightwatch.c "logcheck mutation" 1
}
slowwalk_off() {
    restore src/kernel/core/nightwatch/nightwatch.c
}

# The wake that was never rung. The first application wake (pid >= 3 — the
# console daemon and the shell are left alone, so the harness can still drive
# the machine) that would have reached a sleeper is dropped: the bucket is
# walked, a parked strand is found on exactly that word, and the call returns
# OK having claimed nobody. The word has already moved — the caller stored
# before it asked — so this is precisely a lost wake, once, and it says so on
# serial. The brigade's region never completes: the worker sleeps on a ticket
# that has moved, the caller sleeps on a done-count that never will, and
# Nightwatch has to be the one to say which strand and why.
wakedrop_on() {
    mutate src/kernel/core/decks/system/sync_ops.c
    python3 - <<'EOF'
p = "src/kernel/core/decks/system/sync_ops.c"
s = open(p).read()
anchor = """    spin_lock(&bucket->lock);
    AddrWaitEntry *e = bucket->head;
    while (e && (count == 0 || wake_count < count) && wake_count < ADDR_WAKE_MAX_BATCH)"""
assert anchor in s, "wakedrop anchor missing"
dropped = """    {   /* logcheck mutation: the wake that was never rung */
        static uint32_t s_rung_short;
        if (ctx->proc->pid >= 3 && __atomic_load_n(&s_rung_short, __ATOMIC_ACQUIRE) == 0) {
            bool sleeper = false;
            spin_lock(&bucket->lock);
            for (AddrWaitEntry *w = bucket->head; w; w = w->next)
                if (!w->done && w->phys_addr == phys) { sleeper = true; break; }
            spin_unlock(&bucket->lock);
            if (sleeper && __atomic_exchange_n(&s_rung_short, 1u, __ATOMIC_ACQ_REL) == 0) {
                kprintf("[MUTATION] wake dropped: pid %u asked for 0x%lx and the sleeper was not told\\n",
                        ctx->proc->pid, (unsigned long)phys);
                return OK;
            }
        }
    }
    spin_lock(&bucket->lock);
    AddrWaitEntry *e = bucket->head;
    while (e && (count == 0 || wake_count < count) && wake_count < ADDR_WAKE_MAX_BATCH)"""
open(p, "w").write(s.replace(anchor, dropped, 1))
EOF
    mut_done src/kernel/core/decks/system/sync_ops.c "logcheck mutation: the wake that was never rung"
}
wakedrop_off() {
    restore src/kernel/core/decks/system/sync_ops.c
}

# Sixteen cores. cxxtest's two parallel-algorithm phases are run `rounds`
# times, each waited out to its SUBSET PASS (or an announced failure), or to
# Nightwatch naming a lost wake — whichever comes first. A verdict ends the
# whole thing: the shell is parked on a cxxtest that will never return, so
# nothing typed after it would run.
#
# Between rounds the shell must be back at its prompt, and that is read from
# the LINES, not from the last two bytes of the file: the watch's witness line
# lands after a prompt that has no newline yet, so the file ends in the
# witness and wait_for_prompt would wait its whole ceiling for a shell that
# was ready all along (measured: the first run of this scenario). A second
# line beginning "~ " since the round was typed is the shell's next prompt,
# whatever the kernel appended to it.
lostwake_prompt_back() {
    local mark=$1
    wait_until "$WAIT_QUIET_S" "the shell's next prompt" \
        '[ "$(tail -n +$((mark + 1)) build/serial.log | grep -c "^~ ")" -ge 2 ]' \
        || { echo "  (the shell never came back to its prompt)"; return 1; }
    sleep 2
    return 0
}
lostwake_boot() {
    local rounds=$2
    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=16 MEM=8G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 4
    local run=0
    while [ $run -lt "$rounds" ]; do
        local MARK
        MARK=$(wc -l < build/serial.log)
        if ! type_cmd "cxxtest 201-202"; then break; fi
        sleep 1
        key_press ret
        wait_until "$WAIT_QUIET_S" "a cxxtest verdict or a LOST WAKE" \
            'tail -n +$((MARK + 1)) build/serial.log | grep -qE "\[CXX\] (SUBSET PASS|TOTAL FAILURES)" ||
              grep -q "LOST WAKE" build/serial.log'
        grep -q "LOST WAKE" build/serial.log && break
        lostwake_prompt_back "$MARK" || break
        run=$((run+1))
    done
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.$1.log"
}

run_lostwake() {
    echo "== lostwake: the watch walks slowly through a brigade, and must not cry wolf =="
    slowwalk_on; build
    lostwake_boot lostwake 8
    slowwalk_off
    L="$SCRATCH/serial.lostwake.log"

    local passes looks
    passes=$(grep -c "\[CXX\] SUBSET PASS" "$L")
    [ "$passes" -ge 8 ]
    chk $? "lostwake: cxxtest 201-202 passed eight times over with the watch reading every parked word 2 s late (passes=$passes)"
    # The witness is printed only on a look that found an application alive,
    # so its count is the count of looks that fell inside the workload. Fewer
    # than three and the run proved nothing — the watch hardly looked while
    # the brigade was running.
    looks=$(grep -c "the watch walks slowly" "$L")
    [ "$looks" -ge 3 ]
    chk $? "lostwake: and the watch looked at least three times while a program was alive (looks=$looks)"
    ! grep -q "LOST WAKE" "$L";                 chk $? "lostwake: and no wake was called lost"
    ! grep -q "a defect, not a slow test" "$L"; chk $? "lostwake: and no verdict was spoken"

    build   # leave the tree built from clean sources
}

# The oracle measured against itself: with one wake dropped for real, the same
# run must STOP, and Nightwatch must be the one to say on whom.
run_lostwakemut() {
    echo "== lostwakemut: one wake is never rung, and the watch must name the sleeper =="
    wakedrop_on; build
    lostwake_boot lostwakemut 1
    wakedrop_off
    L="$SCRATCH/serial.lostwakemut.log"

    grep -q "\[MUTATION\] wake dropped" "$L"
    chk $? "lostwakemut: a wake to a sleeping strand was dropped (the mutation bit)"
    grep -q "LOST WAKE" "$L"
    chk $? "lostwakemut: Nightwatch named it — LOST WAKE"
    # The accused must be a worker of the brigade: a strand of cxxtest, pid 4
    # or above. The line two above the verdict names it.
    local accused
    accused=$(grep -B2 "LOST WAKE" "$L" | grep -oE "pid [0-9]+ gen [0-9]+ waiting" | head -1 | awk '{print $2}')
    [ -n "$accused" ] && [ "$accused" -ge 4 ]
    chk $? "lostwakemut: and the accused is a brigade worker (pid ${accused:-?})"
    ! grep -q "\[CXX\] SUBSET PASS" "$L"
    chk $? "lostwakemut: and cxxtest 201-202 did not pass"

    build   # leave the tree built from clean sources
}

# ── chit ─────────────────────────────────────────────────────────────────────
#
# ANSWER OWED used to rest on two witnesses about the MACHINE — every K-Core
# asleep, and no answer published anywhere since the last look — rather than
# on facts about the strand it accused. A brigade of workers parked on their
# leader while the leader merely computes satisfies both (measured 2026-09-06:
# seven workers convicted while the phase passed). The kernel now keeps its own
# half of the cloakroom token, the chit (chit.h): the handler that puts an
# answer off leaves one naming itself, the event that determines the answer
# marks it due, the push that delivers it keeps it. ANSWER OWED convicts only
# a token nobody left a chit for, or one whose chit fell due and was never
# kept — the same a full look later — and the guide says at once, out of band,
# when a deferred answer to a waited-for token left no chit at all.
#
# Three scenarios, on sixteen cores. The healthy one runs every kind of
# deferral this machine has (a brigade parked and woken on addr_park, strands
# parked and joined, files written and read back, children waited out by the
# shell) and then stands at the prompt: nothing may be convicted and neither
# guard may speak. The two mutations each break one promise the way a real bug
# would — the death of a child determined and never delivered; a handler that
# raised the async flag without leaving its chit — and the watch must name the
# strand and the holder, and the guide must name the omission at once.
chit_boot_and_prompt() {
    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=16 MEM=8G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 4
}
# Type one command and wait for either its marker or a Nightwatch verdict.
# Returns 0 only when the MARKER came. The ceiling this used to carry is gone:
# the wait now ends on the machine going quiet, so a slower host no longer
# turns a command that was merely long into a command that never answered.
chit_command() {
    local cmd=$1 marker=$2
    CHIT_MARK=$(wc -l < build/serial.log)
    type_cmd "$cmd" || return 1
    sleep 1
    key_press ret
    # Either the command answered, or the watch convicted somebody. The second
    # is an answer too — just not the one that was asked for — so both end the
    # wait and the two are told apart afterwards.
    wait_until "$WAIT_QUIET_S" "'$marker' or a verdict" \
        'tail -n +$((CHIT_MARK + 1)) build/serial.log | grep -qE -- "$marker" ||
          grep -q "ANSWER OWED" build/serial.log' || return 1
    tail -n +$((CHIT_MARK + 1)) build/serial.log | grep -qE -- "$marker"
}

run_chit() {
    echo "== chit: every deferral leaves its chit, and a healthy machine is convicted of nothing =="
    build
    chit_boot_and_prompt
    local rounds=0 r
    for r in 1 2; do
        chit_command "cxxtest 201-202" "\[CXX\] (SUBSET PASS|TOTAL FAILURES)" || break
        lostwake_prompt_back "$CHIT_MARK" || break
        rounds=$((rounds+1))
    done
    local strands=0 benches=0
    if chit_command "strandtest" "STRAND|strandtest"; then
        lostwake_prompt_back "$CHIT_MARK" && strands=1
    fi
    if chit_command "bench" "create\+write64\+delete"; then
        lostwake_prompt_back "$CHIT_MARK" && benches=1
    fi
    sleep 30   # at the prompt: three looks with nothing to accuse
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.chit.log"
    L="$SCRATCH/serial.chit.log"

    [ "$rounds" -eq 2 ];                        chk $? "chit: cxxtest 201-202 (the brigade) passed twice (rounds=$rounds)"
    [ "$strands" -eq 1 ];                       chk $? "chit: strandtest ran and the shell came back"
    [ "$benches" -eq 1 ];                       chk $? "chit: bench wrote and read the volume and the shell came back"
    ! grep -q "ANSWER OWED" "$L";               chk $? "chit: no answer was called owed"
    ! grep -q "a defect, not a slow test" "$L"; chk $? "chit: and no verdict was spoken"
    ! grep -q "\[GUIDE\] DEFECT" "$L";          chk $? "chit: and every deferred answer to a waited-for token left a chit (the guide stayed silent)"
    ! grep -q "\[CHIT\] DEFECT" "$L";           chk $? "chit: and no strand moved on from an answer the kernel still owed"
    ! grep -qE "PANIC|\[EXCEPTION\]" "$L";      chk $? "chit: and no panic"
}

# The death is determined and the answer never delivered. The first delivery
# of an application's death (pid >= 3) to a waiter marks the chit due — exactly
# as the real path does — and then throws the Result away instead of pushing
# it. The shell waits for a child that has already died, for ever; nothing is
# queued, nobody is serving it, and the only trace is a chit DUE that stays DUE.
chitwithhold_on() {
    mutate src/kernel/core/decks/system/sync_ops.c
    python3 - <<'EOF'
p = "src/kernel/core/decks/system/sync_ops.c"
s = open(p).read()
anchor = """        ChitDue(list->waiter, list->submit_cookie);
        KResultPush(list->waiter, &r);
"""
assert anchor in s, "chitwithhold anchor missing"
held = """        ChitDue(list->waiter, list->submit_cookie);
        {   /* logcheck mutation: the answer is determined, and never delivered */
            static uint32_t s_withheld;
            if (proc->pid >= 3 && list->waiter->pid >= 2 &&
                __atomic_exchange_n(&s_withheld, 1u, __ATOMIC_ACQ_REL) == 0) {
                kprintf("[MUTATION] answer withheld: pid %u owed token 0x%06x for the death of pid %u\\n",
                        list->waiter->pid, list->submit_cookie, proc->pid);
                process_ref_dec(list->waiter);
                process_ref_dec(proc);
                kfree(list);
                list = next;
                continue;
            }
        }
        KResultPush(list->waiter, &r);
"""
open(p, "w").write(s.replace(anchor, held, 1))
EOF
    mut_done src/kernel/core/decks/system/sync_ops.c "logcheck mutation: the answer is determined"
}
chit_mutation_off() {
    restore src/kernel/core/decks/system/sync_ops.c
}

# The handler that forgot. SysProcessGone raises the async flag the old way,
# by hand, and leaves no chit — the shape of every future async handler that
# does not go through ChitGive. The guide must say so the moment the shell's
# wait for its child is deferred, and Nightwatch must convict the shell two
# looks later: waiting, nothing queued, nobody serving, no chit.
chitforgot_on() {
    mutate src/kernel/core/decks/system/sync_ops.c
    python3 - <<'EOF'
p = "src/kernel/core/decks/system/sync_ops.c"
s = open(p).read()
anchor = """    ChitGive(ctx, "system.process.gone", ((uint64_t)want_gen << 32) | pid);
"""
assert anchor in s, "chitforgot anchor missing"
forgot = """    if (ctx->async_owns_crates) *ctx->async_owns_crates = true;   /* logcheck mutation: the handler forgot its chit */
"""
open(p, "w").write(s.replace(anchor, forgot, 1))
EOF
    mut_done src/kernel/core/decks/system/sync_ops.c "logcheck mutation: the handler forgot its chit"
}

# Wait for Nightwatch to speak: a verdict takes two looks (>= 20 s) after the
# stall begins; sixty seconds is three looks and a margin, not a deadline on
# the machine — a healthy one would simply never speak.
#
# ‼ THIS ONE KEEPS ITS DEADLINE, AND IT IS THE RIGHT INSTRUMENT HERE.
#
# Everything else in this file gave its ceiling up for a silence watchdog,
# because silence is evidence of being wedged. Here it is the opposite: the
# machine under test IS stalled — that is the whole scenario — and what is
# being waited for is the WATCH noticing. A silence watchdog would give up the
# instant the stall it is measuring took hold. The number is not a guess about
# speed either; it is counted in Nightwatch's own looks, which are a machine-
# side interval. If the watch ever starts printing per-look, this becomes a
# wait on the look count and the number goes with it.
chit_wait_for_verdict() {
    local t=0
    while [ $t -lt 60 ]; do
        grep -q "ANSWER OWED" build/serial.log && return 0
        sleep 2; t=$((t+1))
    done
    return 1
}

run_chitmut() {
    echo "== chitmut: a child's death is determined and its answer withheld — the watch must name the shell and the holder =="
    chitwithhold_on; build
    chit_boot_and_prompt
    chit_command "say hi" "^hi"
    chit_wait_for_verdict
    make run-stop >/dev/null 2>&1
    chit_mutation_off
    cp build/serial.log "$SCRATCH/serial.chitmut.log"
    L="$SCRATCH/serial.chitmut.log"

    grep -q "\[MUTATION\] answer withheld" "$L"
    chk $? "chitmut: a determined answer was thrown away (the mutation bit)"
    grep -q "ANSWER OWED: system.process.gone took the event" "$L"
    chk $? "chitmut: Nightwatch named it — ANSWER OWED, held by system.process.gone"
    local accused
    accused=$(grep "ANSWER OWED" "$L" | grep -oE "pid [0-9]+ gen" | head -1 | awk '{print $2}')
    [ "${accused:-0}" = 2 ]
    chk $? "chitmut: and the accused is the shell (pid ${accused:-?})"
    ! grep -q "\[GUIDE\] DEFECT" "$L"
    chk $? "chitmut: and the guide had nothing to say — the chit WAS left, it was the delivery that failed"

    build   # leave the tree built from clean sources
}

run_chitmissmut() {
    echo "== chitmissmut: a handler raises the async flag and leaves no chit — the guide must say so at once, the watch two looks later =="
    chitforgot_on; build
    chit_boot_and_prompt
    chit_command "cxxtest" "\[GUIDE\] DEFECT"     # the child runs for minutes; the omission is said as it is deferred
    chit_wait_for_verdict
    make run-stop >/dev/null 2>&1
    chit_mutation_off
    cp build/serial.log "$SCRATCH/serial.chitmissmut.log"
    L="$SCRATCH/serial.chitmissmut.log"

    grep -q "\[GUIDE\] DEFECT: pid 2 deferred the answer to token" "$L"
    chk $? "chitmissmut: the guide named the omission the moment the shell's wait was deferred"
    grep -q "ANSWER OWED: waiting on token .* no handler left a chit" "$L"
    chk $? "chitmissmut: Nightwatch named it — ANSWER OWED, nobody promised the answer"
    local accused
    accused=$(grep "ANSWER OWED" "$L" | grep -oE "pid [0-9]+ gen" | head -1 | awk '{print $2}')
    [ "${accused:-0}" = 2 ]
    chk $? "chitmissmut: and the accused is the shell (pid ${accused:-?})"

    build   # leave the tree built from clean sources
}

# ── turnin ───────────────────────────────────────────────────────────────────
#
# A producer moves a reply ring's `tail` when it claims a slot and releases the
# slot's seq later. A strand that takes its Turn In mark after the claim and
# looks before the release finds nothing to pop, asks to sleep with a mark that
# already covers the arrival, and is put to bed by a kernel that compared only
# cursors. The release lands a moment later; nobody releases a slot twice; a
# wake that then re-checks only the mark re-arms the sleep on top of a message
# already delivered. Measured on BIOS 16c (2026-09-06): the console daemon
# parked at result 36 with pos 35 (a lane request) released and unread, and the
# child that asked for the lane waited for the rest of the run.
#
# The window is microseconds wide on silicon, so it is WIDENED: every IPC push
# into the console daemon's ring waits, after its claim and before its release,
# until the daemon has actually gone to sleep (bounded, so a daemon that never
# sleeps costs the push a few hundred milliseconds and nothing else). That
# forces exactly the interleaving above on every lane request. Under the fix —
# a sleep judged on the slot at the head as well as on the cursor, on both
# sides — phase 58 of cxxtest, which spawns a printing child per step, runs
# three times over; under the cursor-only sleep (put back by turninmut) the
# first lane request that meets the window stops the machine.
turnin_window_on() {
    mutate src/kernel/core/ipc/kring.c
    python3 - <<'EOF'
p = "src/kernel/core/ipc/kring.c"
s = open(p).read()
anchor = """    slot->r = *r;

    /* Publish — the RELEASE store makes the payload above visible to the
"""
assert anchor in s, "turnin window anchor missing"
widened = """    slot->r = *r;
    /* logcheck mutation: an IPC push into pid 1 holds its released-later slot
     * until the daemon has gone to sleep (or 300 ms), so the claim lands
     * before the daemon's mark and the release after its look — every time. */
    if (r->sender_pid != 0 && target->pid == 1) {
        for (uint32_t w = 0; w < 300 && process_get_state(target) != PROC_WAITING; w++)
            delay(1);
    }

    /* Publish — the RELEASE store makes the payload above visible to the
"""
open(p, "w").write(s.replace(anchor, widened, 1))
EOF
    mut_done src/kernel/core/ipc/kring.c "logcheck mutation"
}

turnin_window_off() {
    restore src/kernel/core/ipc/kring.c
}

# The sleep that stood here before: cursors only, on both sides — exactly the
# old SysTurnIn re-check and the old box_turn_in exit — and the writer that
# stood with it, which waited for its lane grant without ever asking again.
# The re-ask (print.c) would otherwise end the mutation's wedge by itself: a
# second request is a second push, and a push wakes the sleeper.
cursoronly_on() {
    mutate src/kernel/core/decks/system/turnin_ops.c
    mutate src/userspace/boxlib/src/turnin.c
    mutate src/userspace/boxlib/src/print.c
    python3 - <<'EOF'
p = "src/userspace/boxlib/src/print.c"
s = open(p).read()
anchor = """            asked = false;
            continue;"""
assert anchor in s, "cursoronly writer anchor missing"
blind = """            continue;   /* logcheck mutation: the writer that never asked again */"""
open(p, "w").write(s.replace(anchor, blind, 1))
p = "src/kernel/core/decks/system/turnin_ops.c"
s = open(p).read()
anchor = """    return turnin_touch_tail(proc)  != touch_seen  ||
           turnin_result_tail(proc) != result_seen ||
           KResultRingHasUnreadAtHead(proc)        ||
           KTouchRingHasUnreadAtHead(proc);"""
assert anchor in s, "cursoronly kernel anchor missing"
blind = """    /* logcheck mutation: cursors only, the sleep that stood here before */
    return turnin_touch_tail(proc)  != touch_seen  ||
           turnin_result_tail(proc) != result_seen;"""
open(p, "w").write(s.replace(anchor, blind, 1))
p = "src/userspace/boxlib/src/turnin.c"
s = open(p).read()
anchor = """    return box_mark_moved(seen) ||
           result_published_at_head() ||
           touch_ring_published_at_head();"""
assert anchor in s, "cursoronly boxlib anchor missing"
blind = """    return box_mark_moved(seen);   /* logcheck mutation: the mark alone */"""
open(p, "w").write(s.replace(anchor, blind, 1))
EOF
    mut_done src/kernel/core/decks/system/turnin_ops.c "logcheck mutation"
    mut_done src/userspace/boxlib/src/print.c "logcheck mutation"
    mut_done src/userspace/boxlib/src/turnin.c "logcheck mutation: the mark alone"
}

cursoronly_off() {
    restore src/kernel/core/decks/system/turnin_ops.c
    restore src/userspace/boxlib/src/turnin.c
    restore src/userspace/boxlib/src/print.c
}

# Sixteen cores, `cxxtest 58` three times over. Each run is waited out to its
# SUBSET PASS or to a frozen serial — a child waiting for a lane it will never
# be granted prints nothing, and the shell is parked on a cxxtest that will
# never return, so nothing typed after it would run.
turnin_boot() {
    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=16 MEM=8G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 4
    local run=0
    while [ $run -lt 3 ]; do
        local before
        before=$(grep -c "SUBSET PASS" build/serial.log)
        if ! type_cmd "cxxtest 58"; then break; fi
        sleep 1
        key_press ret
        wait_until "$WAIT_QUIET_S" "cxxtest 58 to pass its subset" \
            '[ "$(grep -c "SUBSET PASS" build/serial.log)" -gt "$before" ]'
        [ "$(grep -c "SUBSET PASS" build/serial.log)" -gt "$before" ] || break
        run=$((run+1))
    done
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.$1.log"
}

run_turnin() {
    echo "== turnin: a slot claimed before the sleeper's mark and released after its look =="
    turnin_window_on; build
    turnin_boot turnin
    turnin_window_off
    L="$SCRATCH/serial.turnin.log"

    local passes
    passes=$(grep -c "SUBSET PASS" "$L")
    [ "$passes" -ge 3 ]
    chk $? "turnin: cxxtest 58 passed three runs running with every lane request released only after the daemon slept (passes=$passes)"
}

# The oracle measured against itself: with the cursor-only sleep back, the
# same run must STOP on its first lane request.
run_turninmut() {
    echo "== turninmut: put the cursor-only sleep back and require the machine to stop =="
    turnin_window_on; cursoronly_on; build
    turnin_boot turninmut
    cursoronly_off; turnin_window_off
    L="$SCRATCH/serial.turninmut.log"

    local passes
    passes=$(grep -c "SUBSET PASS" "$L")
    [ "$passes" -lt 3 ]
    chk $? "turninmut: with the cursor-only sleep back, cxxtest 58 did not get through three runs (passes=$passes)"
}

# And the other half: a box that never turns in. Removing the submit leaves the
# loop exactly as it was before this work — look, find nothing, look again.
turnin_off() {
    mutate src/userspace/boxlib/src/turnin.c
    python3 - <<'EOF'
p = "src/userspace/boxlib/src/turnin.c"
s = open(p).read()
anchor = "        if (pocket_ring_is_empty(pocket_ring())) turn_in_submit(seen);"
assert anchor in s, "turnin mutation anchor missing"
s = s.replace(anchor, "        /* logcheck mutation: never ask to be put down */", 1)
open(p, "w").write(s)
EOF
    mut_done src/userspace/boxlib/src/turnin.c "logcheck mutation"
}

turnin_restore() {
    restore src/userspace/boxlib/src/turnin.c
}

# And the reader's own half: a brook_pop that spins instead of sleeping. This
# is the narrowest of the three — turn-in stays, the bell stays, only the wait
# inside Brook goes back to watching a cacheline — so it is the one that says
# whether the window measurement is about brook_pop or merely about the box.
brookpop_off() {
    mutate src/userspace/boxlib/src/brook.c
    python3 - <<'EOF'
p = "src/userspace/boxlib/src/brook.c"
s = open(p).read()
anchor = "        brook_sleep_until(b, brook_pop_ready);"
assert anchor in s, "brookpop mutation anchor missing"
s = s.replace(anchor, "        brook_wait_cycle(&h->tail, &spin, 0);   /* logcheck mutation: spin, do not sleep */", 1)
open(p, "w").write(s)
EOF
    mut_done src/userspace/boxlib/src/brook.c "logcheck mutation"
}

brookpop_restore() {
    restore src/userspace/boxlib/src/brook.c
}

# sleepsmut — the oracle measured against itself.
#
# A check nobody has ever seen go red is a check nobody should believe. This
# breaks each half in turn, on purpose, and requires the corresponding half of
# `sleeps` to notice. Neither mutation is subtle and neither is a crash: both
# leave a machine that boots, runs, and prints everything — which is precisely
# why an oracle is needed to tell them apart from a healthy one.
run_sleepsmut() {
    echo "== sleepsmut: break the bell, then the sleep, and require the oracle to say so =="

    local before_fail
    before_fail=$FAIL

    bell_off; build
    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=1 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 4
    local MARK EARLY
    MARK=$(wc -l < build/serial.log)
    type_cmd "quietprint"
    sleep 1
    key_press ret
    sleep 2
    EARLY=$(tail -n +$((MARK + 1)) build/serial.log)
    # Long enough for TWO Nightwatch looks. The watch convicts on a delivery
    # that is still unread a whole look later, never on one instant — a
    # delivery in flight looks exactly like a lost one for the microseconds
    # between the record and the wake, and sleeping strands go through that gap
    # thousands of times a second under a print storm.
    sleep 28
    cp build/serial.log "$SCRATCH/serial.sleepsmut.bell.log"
    make run-stop >/dev/null 2>&1
    bell_restore

    if printf '%s' "$EARLY" | grep -q "\[QP\] line 1"; then
        bad "sleepsmut: bell disabled and the line STILL arrived on time — the check proves nothing"
    else
        ok "sleepsmut: bell disabled — the line was not on screen while the writer lived"
    fi

    # lane_push used to announce its own jam after sixty seconds. It does not
    # any more, because it sleeps instead of spinning and a sleeper cannot
    # narrate. This is what took that job: the watch names the jam from facts,
    # with no clock in it. If it ever goes quiet, the jam goes silent with it.
    if grep -q "BELL UNRUNG" "$SCRATCH/serial.sleepsmut.bell.log"; then
        ok "sleepsmut: Nightwatch named the jam — BELL UNRUNG, on facts, with no deadline"
    else
        bad "sleepsmut: nobody rang and NOBODY SAID SO — a stream can now jam in silence"
    fi

    turnin_off; build
    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=1 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep $QUIET_SETTLE_S
    local A B USED
    A=$(qemu_cpu_cs); sleep $QUIET_SAMPLE_S; B=$(qemu_cpu_cs)
    USED=$(( (B - A) * 100 / (QUIET_SAMPLE_S * 100) ))
    make run-stop >/dev/null 2>&1
    turnin_restore

    echo "     idle without Turn In: ${USED}% of one core"
    if [ "$USED" -lt 25 ]; then
        bad "sleepsmut: Turn In removed and the box was STILL idle at ${USED}% — the check proves nothing"
    else
        ok "sleepsmut: Turn In removed — the box went back to ${USED}% of a core"
    fi

    brookpop_off; build
    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=1 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3
    type_cmd "quietprint"
    sleep 1
    key_press ret
    sleep 1
    local WA WB WUSED
    WA=$(qemu_cpu_cs); sleep 2; WB=$(qemu_cpu_cs)
    WUSED=$(( (WB - WA) * 100 / 200 ))
    wait_for_line build/serial.log "\[QP\] done" "$WAIT_QUIET_S"
    cp build/serial.log "$SCRATCH/serial.sleepsmut.brookpop.log"
    make run-stop >/dev/null 2>&1
    brookpop_restore

    echo "     blocked brook_pop without the sleep: ${WUSED}% of one core"
    if [ "$WUSED" -lt 25 ]; then
        bad "sleepsmut: brook_pop set back to spinning and the box was STILL idle at ${WUSED}% — the window measurement proves nothing"
    else
        ok "sleepsmut: brook_pop set back to spinning — the window went to ${WUSED}% of a core"
    fi

    build   # leave the tree built from clean sources either way
    [ $FAIL -eq $before_fail ] || echo "     (a red line above means the oracle cannot see its own defect)"
}

# ===========================================================================
# lines — one strand's line is never cut in half by another's
# ===========================================================================
#
# The console is a fan-in: every printing strand has its own lane, and the
# daemon merges them in global push order. The unit it interleaves is a FRAME,
# so where a frame ends decides whether concurrent output is readable. It used
# to end wherever the 108-byte run filled up, which lands mid-line, and two
# strands printing at once then spliced into each other:
#
#     [PS-11] 00000[PS-04] 00000000
#     [PS-04787
#
# Nothing was ever lost — the character count came out exact on every run, on
# both sides of the fix — so no test could see it by counting. What it cost was
# grep: every marker in this project is one, and a torn "[STRAND] PASS" reads
# as a machine that hung. That is what made sixteen-core runs a lottery.
#
# print_stress is the instrument: sixteen strands, two thousand numbered lines
# each, all of one exact shape. Anything that does not match that shape is a
# line something else got into. MEASURED: ~300-700 spliced lines per run before
# the fix, 0 of 32000 after.
run_lines() {
    echo "== lines: sixteen strands printing at once, and not one line spliced =="
    build

    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=16 MEM=8G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    grep -q "BoxOS Shell" build/serial.log 2>/dev/null || { bad "lines: never reached a shell"; make run-stop >/dev/null 2>&1; return; }

    local MARK
    MARK=$(wc -l < build/serial.log)
    type_cmd "print_stress"
    sleep 1
    key_press ret
    wait_for_line build/serial.log "PS SUMMARY" "$WAIT_QUIET_S" $((MARK + 1))
    # The serial line discipline is CRLF, and a trailing \r defeats a `$`
    # anchor on BSD grep — strip it once here rather than have every check
    # below carry the same footnote.
    tail -n +$((MARK + 1)) build/serial.log | tr -d '\r' > "$SCRATCH/serial.lines.log"
    make run-stop >/dev/null 2>&1

    grep -q "PS SUMMARY.*PASS" "$SCRATCH/serial.lines.log" \
        && ok "lines: print_stress ran to the end" \
        || bad "lines: print_stress never finished"

    local WHOLE DIGITS
    WHOLE=$(grep -cE '^\[PS-[0-9]{2}\] [0-9]{8}$' "$SCRATCH/serial.lines.log" | tr -d ' ')
    DIGITS=$(tr -cd '0-9' < "$SCRATCH/serial.lines.log" | wc -c | tr -d ' ')
    echo "     $WHOLE of 32000 lines whole; $DIGITS digits on the wire"

    # Exactly 32000, not "nearly". A single splice is a marker somebody's grep
    # will miss one run in ten, which is the whole failure this closes.
    if [ "$WHOLE" -eq 32000 ]; then ok "lines: all 32000 lines came out whole"
    else                            bad "lines: $WHOLE of 32000 whole — $((32000 - WHOLE)) line(s) were spliced"; fi

    # And the older, weaker fact, kept because it is the one that says the
    # difference between a splice and a LOSS: the characters are all there.
    if [ "$DIGITS" -ge 256000 ]; then ok "lines: every character reached the log ($DIGITS digits)"
    else                              bad "lines: only $DIGITS digits reached the log — characters were LOST, not merely spliced"; fi
}

# The mutation: cut a frame wherever the buffer happens to fill, as it did
# before. Nothing breaks, nothing is lost, and the log becomes unmatchable —
# which is exactly why this needs an oracle rather than an eye.
linecut_off() {
    mutate src/userspace/boxlib/src/print.c
    python3 - <<'EOF'
p = "src/userspace/boxlib/src/print.c"
s = open(p).read()
anchor = "                lane_flush_lines(ps);"
assert anchor in s, "linecut mutation anchor missing"
s = s.replace(anchor, "                lane_flush_run(ps);   /* logcheck mutation: cut anywhere */", 1)
open(p, "w").write(s)
EOF
    mut_done src/userspace/boxlib/src/print.c "logcheck mutation"
}

linecut_restore() {
    restore src/userspace/boxlib/src/print.c
}

run_linesmut() {
    echo "== linesmut: cut frames anywhere again, and require the oracle to notice =="
    linecut_off; build

    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=16 MEM=8G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    local MARK
    MARK=$(wc -l < build/serial.log)
    type_cmd "print_stress"
    sleep 1
    key_press ret
    wait_for_line build/serial.log "PS SUMMARY" "$WAIT_QUIET_S" $((MARK + 1))
    tail -n +$((MARK + 1)) build/serial.log | tr -d '\r' > "$SCRATCH/serial.linesmut.log"
    make run-stop >/dev/null 2>&1
    linecut_restore

    local WHOLE
    WHOLE=$(grep -cE '^\[PS-[0-9]{2}\] [0-9]{8}$' "$SCRATCH/serial.linesmut.log" | tr -d ' ')
    echo "     cutting anywhere: $WHOLE of 32000 lines whole"
    if [ "$WHOLE" -eq 32000 ]; then
        bad "linesmut: frames cut anywhere and nothing spliced — the check proves nothing"
    else
        ok "linesmut: frames cut anywhere — $((32000 - WHOLE)) line(s) spliced, and the oracle sees them"
    fi

    build   # leave the tree built from clean sources
}

# ===========================================================================
# rollcall — every line said answers with its number, whole, on the wire
# ===========================================================================
#
# Two paths carry a line into the serial account, and each had a way of
# losing or tearing it that no count of characters could see.
#
# printf stages a strand's text in the strand's own frame and pushes it when
# the frame fills, the colour changes or somebody flushes — so the line a
# strand said just before returning died with it (MEASURED on BIOS 16c: the
# last of 400 numbered lines, every printf strand, every run; print_stress
# hid it with an io_flush of its own). And the daemon drew a line's text in
# one VGA operation and its newline in the next, with the console lock held
# per operation, so a kprintf from another core landed between the two:
#
#     [ROLLCALL] s=1 n=0 via=printf[6] [ROLLCALL] s=2 n=2 via=kdbg
#
# (MEASURED: 184 and 71 such lines of 1600 in two runs.) `lines` can see
# neither: print_stress flushed by hand, and nothing prints from the kernel
# while it runs.
#
# rollcall is the instrument: four strands, four hundred numbered lines each,
# two through printf and two through kdbg_print (the kernel's kprintf), all
# at once. Every number must answer exactly once, and no line may carry
# another's.
rollcall_boot() {
    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=16 MEM=8G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    if ! grep -q "BoxOS Shell" build/serial.log 2>/dev/null; then
        make run-stop >/dev/null 2>&1
        return 1
    fi
    sleep 3
    local MARK
    MARK=$(wc -l < build/serial.log)
    type_cmd "rollcall"
    sleep 1
    key_press ret
    wait_for_line build/serial.log "\[ROLLCALL\] done" "$WAIT_QUIET_S" $((MARK + 1))
    sleep 2
    # CRLF on the wire, and a kdbg line wears the kernel's "[pid] " prefix:
    # strip both once so the shape below is the line's own.
    tail -n +$((MARK + 1)) build/serial.log | tr -d '\r' \
        | sed -E 's/^\[[0-9]+\] //' > "$SCRATCH/serial.$1.log"
    make run-stop >/dev/null 2>&1
    return 0
}

# Prints "<answered> <carrying>": numbers that answered as a whole line of
# their own, and lines that carry more than one record.
rollcall_count() {
    local answered=0 s
    for s in 0 1 2 3; do
        answered=$(( answered + $(grep -E "^\[ROLLCALL\] s=$s n=[0-9]+ via=(printf|kdbg)$" "$1" | sort -u | wc -l | tr -d ' ') ))
    done
    local carrying
    carrying=$(grep -c "ROLLCALL.*ROLLCALL" "$1" | tr -d ' ')
    echo "$answered $carrying"
}

run_rollcall() {
    echo "== rollcall: 1600 numbered lines from four strands, every one answering, none carrying another =="
    build
    if ! rollcall_boot rollcall; then bad "rollcall: never reached a shell"; return; fi
    L="$SCRATCH/serial.rollcall.log"

    grep -q "\[ROLLCALL\] done: 4 strands x 400 lines" "$L" \
        && ok "rollcall: all four strands spoke and the utility finished" \
        || bad "rollcall: the utility did not finish with four strands"

    set -- $(rollcall_count "$L")
    echo "     $1 of 1600 numbers answered whole; $2 line(s) carrying another record"
    if [ "$1" -eq 1600 ]; then ok "rollcall: every number answered, whole"
    else                       bad "rollcall: $((1600 - $1)) number(s) never answered whole"; fi
    if [ "$2" -eq 0 ]; then ok "rollcall: no line carried another's"
    else                    bad "rollcall: $2 line(s) carried another record"; fi
}

# The oracle measured against itself: put both defects back — the strand
# leaves without handing in its frame, the daemon draws text and newline as
# two operations — and each symptom above must come back.
rollcall_defects_on() {
    mutate src/userspace/boxlib/src/strand.c
    mutate src/userspace/display/display.c
    python3 - <<'EOF'
p = "src/userspace/boxlib/src/strand.c"
s = open(p).read()
anchor = "    io_flush();\n"
assert s.count(anchor) == 1, "rollcall strand anchor missing"
s = s.replace(anchor, "    /* logcheck mutation: the strand leaves without handing in its frame */\n", 1)
open(p, "w").write(s)
p = "src/userspace/display/display.c"
s = open(p).read()
anchor = """    for (uint32_t i = 0; i < len; i++) {
        char c = f->text[i];
        if (c == '\\n' || (unsigned char)c >= 0x20) seg[pos++] = c;
    }
    if (pos > 0) {
        seg[pos] = '\\0';
        vga_puts(seg);
    }
"""
assert s.count(anchor) == 1, "rollcall display anchor missing"
mutated = """    for (uint32_t i = 0; i <= len; i++) {   /* logcheck mutation: text and newline as two operations */
        int at_end = (i == len);
        char c     = at_end ? '\\0' : f->text[i];
        if (!at_end && c != '\\n' && (unsigned char)c >= 0x20) { seg[pos++] = c; continue; }
        if (pos > 0) { seg[pos] = '\\0'; vga_puts(seg); pos = 0; }
        if (!at_end && c == '\\n') vga_newline();
    }
"""
open(p, "w").write(s.replace(anchor, mutated, 1))
EOF
    mut_done src/userspace/boxlib/src/strand.c "logcheck mutation"
    mut_done src/userspace/display/display.c "logcheck mutation"
}

rollcall_defects_off() {
    restore src/userspace/boxlib/src/strand.c
    restore src/userspace/display/display.c
}

run_rollcallmut() {
    echo "== rollcallmut: both defects put back, and the roll call must see each =="
    rollcall_defects_on; build
    rollcall_boot rollcallmut; local booted=$?
    rollcall_defects_off
    if [ $booted -ne 0 ]; then bad "rollcallmut: never reached a shell"; build; return; fi
    L="$SCRATCH/serial.rollcallmut.log"

    set -- $(rollcall_count "$L")
    echo "     $1 of 1600 numbers answered whole; $2 line(s) carrying another record"
    if [ "$1" -lt 1600 ]; then ok "rollcallmut: a strand that leaves without handing in loses lines — $((1600 - $1)) missing, and the oracle sees them"
    else                       bad "rollcallmut: the flush removed and every number STILL answered — the oracle cannot see that defect"; fi
    if [ "$2" -gt 0 ]; then ok "rollcallmut: text and newline as two operations let $2 line(s) carry another — and the oracle sees them"
    else                    bad "rollcallmut: two operations per line and NO line carried another — the oracle cannot see that defect"; fi

    build   # leave the tree built from clean sources
}


# ===========================================================================
# handset — who holds the handset hears the keys
# ===========================================================================
#
# The console's ear: the display daemon consumes keyboard Touches only while
# some lane listens, and republishes each key as a Touch on the tag of the
# lane at the top of the listening stack. A program that asks for input takes
# the top; a lane that closes — its owner died or left — is dropped, and the
# one beneath hears again. Nothing is asked of a clock, and nothing typed is
# lost: keys pressed before anybody listens stay banked in the daemon's ring
# until somebody does.
#
# Three facts, one boot:
#   1. a child (handset) hears the line typed AFTER it asked;
#   2. the shell hears again once the child is gone (it runs `hw`);
#   3. a child hears the line typed BEFORE it asked (type-ahead banked).
handset_boot() {
    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=16 MEM=8G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    if ! grep -q "BoxOS Shell" build/serial.log 2>/dev/null; then
        make run-stop >/dev/null 2>&1
        return 1
    fi
    sleep 3

    # 1 + 2: ask, then the line; then `hw` for the shell.
    type_cmd "handset"; sleep 1
    key_press ret
    wait_for_line build/serial.log "\[HANDSET\] ask" "$WAIT_QUIET_S"
    sleep 1
    type_cmd "after the ask"; sleep 1
    key_press ret
    wait_for_line build/serial.log "\[HANDSET\] got: after the ask" "$WAIT_QUIET_S"
    sleep 2
    type_cmd "hw"; sleep 1
    key_press ret
    wait_for_line build/serial.log "TSC freq" "$WAIT_QUIET_S"
    sleep 2

    # 3: the line typed before the ask — `handset`, Enter, then the words at
    # once, before "[HANDSET] ask" can possibly have been printed. The words go
    # as RAW keystrokes: `type` waits for each character's echo and retypes
    # what was not echoed, and a key that nobody echoes because nobody listens
    # is exactly the key this case is about — a witness would type it again
    # once the child listens and hide the loss.
    local MARK
    MARK=$(wc -l < build/serial.log)
    type_cmd "handset"
    key_press ret
    local k
    for k in b e f o r e spc t h e spc a s k ret; do
        mon_raw "sendkey $k 30"
        sleep 0.03
    done
    wait_for_line build/serial.log "\[HANDSET\] got:" "$REACT_QUIET_S" $((MARK + 1))
    sleep 2

    tr -d '\r' < build/serial.log > "$SCRATCH/serial.$1.log"
    make run-stop >/dev/null 2>&1
    return 0
}

run_handset() {
    echo "== handset: the child hears, the shell hears again, nothing typed early is lost =="
    build
    if ! handset_boot handset; then bad "handset: never reached a shell"; return; fi
    L="$SCRATCH/serial.handset.log"

    grep -q "\[HANDSET\] got: after the ask" "$L" \
        && ok "handset: the child heard the line typed after it asked" \
        || bad "handset: the child never got the line typed after it asked"

    # The shell heard again: `hw` ran after the first handset was gone.
    sed -n '/\[HANDSET\] got: after the ask/,$p' "$L" | grep -q "TSC freq" \
        && ok "handset: the shell heard again once the child was gone (hw ran)" \
        || bad "handset: after the child died the shell never heard hw"

    grep -q "\[HANDSET\] got: before the ask" "$L" \
        && ok "handset: the line typed before the ask was banked and heard" \
        || bad "handset: the line typed before the ask was lost"
}

# The oracle measured against itself: the daemon consumes keys while nobody
# listens — the very thing "keys wait at the daemon for the next reader"
# forbids — so the line typed before the child asked is gone.
handset_deaf_on() {
    mutate src/userspace/display/display.c
    python3 - <<'EOF'
p = "src/userspace/display/display.c"
s = open(p).read()
anchor = "    while (g_ear && touch_try_pop_tag(g_kb, &t)) {\n"
assert s.count(anchor) == 1, "handset mutation anchor missing"
s = s.replace(anchor, "    while (touch_try_pop_tag(g_kb, &t)) {   /* logcheck mutation: consumed while nobody listens */\n", 1)
anchor2 = "        touch_send(g_ear->lane->ear, t.payload, t.payload_len, 0);\n"
assert s.count(anchor2) == 1, "handset mutation anchor 2 missing"
s = s.replace(anchor2, "        if (g_ear) touch_send(g_ear->lane->ear, t.payload, t.payload_len, 0);\n", 1)
open(p, "w").write(s)
EOF
    mut_done src/userspace/display/display.c "logcheck mutation"
}

handset_deaf_off() {
    restore src/userspace/display/display.c
}

run_handsetmut() {
    echo "== handsetmut: keys consumed while nobody listens, and the early line must be lost =="
    handset_deaf_on; build
    handset_boot handsetmut; local booted=$?
    handset_deaf_off
    if [ $booted -ne 0 ]; then bad "handsetmut: never reached a shell"; build; return; fi
    L="$SCRATCH/serial.handsetmut.log"

    if grep -q "\[HANDSET\] got: before the ask" "$L"; then
        bad "handsetmut: keys consumed while nobody listened and the early line STILL arrived — the oracle cannot see that defect"
    else
        ok "handsetmut: keys consumed while nobody listened lost the early line — and the oracle sees it"
    fi

    build   # leave the tree built from clean sources
}


# ===========================================================================
# brigade — one wake for more sleepers than any tray
# ===========================================================================
#
# SysAddrWake used to claim its waiters into a stack array of 256 and stop
# when it was full: a notify_all with more sleepers than that woke 256 and
# left the rest asleep for ever, and nothing said so. The claimed entries now
# ride to their delivery on their own link, so there is nothing to size.
#
# strandpark part 3 is the instrument: 300 strands park on one word with no
# deadline, main learns of every park from the kernel's own strand:parked
# Touch, then changes the word and wakes all parked on it — once. Every
# sleeper must come back.
# ===========================================================================
# draft — what was typed is what the volume holds, and a shorter save leaves
#         no tail of the longer one behind it
# ===========================================================================
#
# TagFS writes only ever GREW a file: rewriting one shorter left the old tail
# readable behind the new content, and every shorter rewrite in the system was
# silently wrong until file_truncate existed. An editor is where that shows
# first and worst — a line deleted and saved would come back on the next read.
#
# draft is the instrument, and the whole path is under test: a key becomes a
# Touch, the Touch becomes a byte in the book, ^S writes the book, truncates
# to its length and anchors it, and `show` reads the volume back. Ten bytes
# are typed and saved; five are taken back and saved again; the volume must
# hold five.
draft_keys() {
    # draft_keys KEY...  — straight to the monitor, no echo to wait for: a
    # painted frame never enters the log ring, so qemu-input's `type` (which
    # verifies the guest echo) has nothing to see and would give up.
    local k
    for k in "$@"; do
        mon_raw "sendkey $k 30"
        sleep 0.06
    done
}

draft_session() {
    # draft_session NAME "RUN-BG ARGS" — boots, drives one editing session,
    # then reads the file back with `show`. Leaves the transcript in
    # $SCRATCH/serial.NAME.log and the machine stopped.
    local name=$1 args=$2
    make run-stop >/dev/null 2>&1
    # shellcheck disable=SC2086
    make run-bg $args >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    if ! grep -q "BoxOS Shell" build/serial.log 2>/dev/null; then
        make run-stop >/dev/null 2>&1
        return 1
    fi
    sleep 3
    local MARK
    MARK=$(wc -l < build/serial.log)

    type_cmd "draft oracle.txt"
    sleep 1
    key_press ret
    sleep 3

    draft_keys a b c d e f g h i j
    draft_keys ctrl-s
    sleep 2
    draft_keys backspace backspace backspace backspace backspace
    draft_keys ctrl-s
    sleep 2
    draft_keys ctrl-q
    sleep 3

    type_cmd "show oracle.txt"
    sleep 1
    key_press ret
    sleep 3

    tail -n +$((MARK + 1)) build/serial.log | tr -d '\r' > "$SCRATCH/serial.$name.log"
    make run-stop >/dev/null 2>&1
    return 0
}

run_draft() {
    echo "== draft: ten bytes typed and saved, five taken back and saved again =="
    build
    if ! draft_session draft "STRICT=on CORES=4 MEM=4G"; then
        bad "draft: never reached a shell"; return
    fi
    L="$SCRATCH/serial.draft.log"
    grep -q "\[DRAFT\] oracle.txt - 1 lines, 5 bytes, saved 2 times" "$L" \
        && ok "draft: the editor says five bytes over two saves" \
        || bad "draft: $(grep -m1 '\[DRAFT\]' "$L" || echo 'the editor said nothing at all')"
    if grep -q "^abcde$" "$L"; then
        ok "draft: the volume holds exactly what was left on the screen"
    elif grep -q "abcdefghij" "$L"; then
        bad "draft: the old tail is still on the volume - the shorter save did not cut it"
    else
        bad "draft: the volume did not answer with the draft at all"
    fi
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "draft: the kernel spoke of a stall or fault" \
        || ok "draft: no verdict, no panic"
}

draft_notail_on() {
    mutate src/userspace/utils/draft_file.c
    python3 - <<'EOF2'
p = "src/userspace/utils/draft_file.c"
s = open(p).read()
anchor = "    int rc = file_truncate(g_fid, len);"
assert s.count(anchor) == 1, "draft mutation anchor missing"
s = s.replace(anchor, "    int rc = 0;   /* logcheck mutation: the tail is left where it was */\n    (void)len;", 1)
open(p, "w").write(s)
EOF2
    mut_done src/userspace/utils/draft_file.c "logcheck mutation"
}

draft_notail_off() {
    restore src/userspace/utils/draft_file.c
}

run_draftmut() {
    echo "== draftmut: the truncate taken out, and the old tail must come back =="
    draft_notail_on; build
    draft_session draftmut "STRICT=on CORES=4 MEM=4G"; local booted=$?
    draft_notail_off
    if [ $booted -ne 0 ]; then bad "draftmut: never reached a shell"; build; return; fi
    L="$SCRATCH/serial.draftmut.log"
    if grep -q "abcdefghij" "$L"; then
        ok "draftmut: without the cut the old tail is read back - and the oracle sees it"
    else
        bad "draftmut: the truncate was taken out and the file was STILL five bytes - the oracle cannot see that defect ($(grep -m1 'abcde' "$L" || echo 'no content at all'))"
    fi
    build   # leave the tree built from clean sources
}

# ===========================================================================
# paint — a whole frame of colour reaches the glass as ONE op, and says
#         nothing into the log ring on the way
# ===========================================================================
#
# Before HW_VGA_PAINT the only way to colour a cell was HW_VGA_PUTCHAR, which
# is one Manifest op per cell AND one LogRingPut per cell — a global spinlock
# and a wire kick for every one of sixteen thousand cells, twenty-five times a
# second, burying everything the machine actually says under a screenful of
# spaces. PAINT lays a rectangle down in one op, one Canvas commit, one blit,
# and deliberately does not mirror: a picture is not speech.
#
# playtime is the instrument: it paints every cell of the screen every frame
# and stops on a key. The screendump must show a painted screen (a shell
# prompt is black almost everywhere), and the run must have said nothing but
# its own one line.
paint_shot_verdict() {
    # paint_shot_verdict PPM — prints "painted N" or "bare N", N = the
    # percentage of sampled pixels that are not the console's black.
    python3 - "$1" <<'EOF2'
import sys
d = open(sys.argv[1], 'rb').read()
parts = d.split(b'\n', 3)
w, h = map(int, parts[1].split())
px = parts[3]
lit = total = 0
for y in range(0, h, 4):
    for x in range(0, w, 4):
        i = (y * w + x) * 3
        total += 1
        if px[i:i+3] != b'\x00\x00\x00':
            lit += 1
pct = (100 * lit) // max(total, 1)
print(("painted " if pct >= 80 else "bare ") + str(pct))
EOF2
}

paint_boot() {
    local name=$1 args=$2
    make run-stop >/dev/null 2>&1
    # shellcheck disable=SC2086
    make run-bg $args >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    if ! grep -q "BoxOS Shell" build/serial.log 2>/dev/null; then
        make run-stop >/dev/null 2>&1
        return 1
    fi
    sleep 3
    local MARK
    MARK=$(wc -l < build/serial.log)
    type_cmd "playtime"
    sleep 1
    key_press ret
    # ‼ THERE IS NOTHING IN THE LOG TO WAIT FOR HERE, AND THAT IS THE SCENARIO.
    # A painted frame says nothing into the ring — proving that is half of what
    # this measures — so the fact has to be read off the GLASS instead of the
    # log: take the screendump, and if it is not a painted screen yet, take
    # another. `playtime` keeps pouring until a key stops it, so a shot can
    # only ever be too EARLY, which is the one way a flat six seconds could
    # fail and did not announce itself.
    local shot=0
    while [ $shot -lt "$PAINT_SHOT_TRIES" ]; do
        mon_shot "$SCRATCH/$name.ppm"
        case "$(paint_shot_verdict "$SCRATCH/$name.ppm" 2>/dev/null)" in
            painted*) break ;;
        esac
        sleep 1; shot=$((shot+1))
    done
    sleep 1
    key_press spc
    sleep 3
    tail -n +$((MARK + 1)) build/serial.log | tr -d '\r' > "$SCRATCH/serial.$name.log"
    make run-stop >/dev/null 2>&1
    return 0
}

run_paint() {
    echo "== paint: one op lays a whole screen of colour, and the log stays quiet =="
    build
    if ! paint_boot paint "UEFI=on STRICT=on CORES=4 MEM=4G"; then
        bad "paint: never reached a shell"; return
    fi
    L="$SCRATCH/serial.paint.log"
    V=$(paint_shot_verdict "$SCRATCH/paint.ppm")
    case "$V" in
        painted*) ok "paint: the screendump is a painted screen ($V per cent lit)" ;;
        *)        bad "paint: the screen was not painted ($V per cent lit)" ;;
    esac
    FR=$(grep -oE "\[PLAYTIME\] [0-9]+ frames" "$L" | grep -oE "[0-9]+" | head -1)
    [ -n "$FR" ] && [ "$FR" -gt 10 ] \
        && ok "paint: $FR frames were laid down and a key stopped them" \
        || bad "paint: the pour said '$(grep -m1 '\[PLAYTIME\]' "$L" || echo nothing)'"
    # A painted cell must not enter the log ring. The whole session is allowed
    # its own lines; a paint that mirrored would put thousands of spaces here.
    SP=$(tr -d '\n' < "$L" | tr -dc ' ' | wc -c | tr -d ' ')
    [ "$SP" -lt 4000 ] \
        && ok "paint: the frames said nothing into the log ring ($SP spaces in the whole session)" \
        || bad "paint: the log ring carries $SP spaces - the frames are being mirrored into it"
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "paint: the kernel spoke of a stall or fault" \
        || ok "paint: no verdict, no panic"
}

paint_blind_on() {
    mutate src/kernel/core/decks/hardware/hardware_ops.c
    python3 - <<'EOF2'
p = "src/kernel/core/decks/hardware/hardware_ops.c"
s = open(p).read()
anchor = "    console_lock_acquire();\n    bool fit = VideoPaintCells(row, col, height, width, cells);\n    console_lock_release();"
assert s.count(anchor) == 1, "paint mutation anchor missing"
s = s.replace(anchor, "    bool fit = true;   /* logcheck mutation: the op takes the frame and drops it */", 1)
open(p, "w").write(s)
EOF2
    mut_done src/kernel/core/decks/hardware/hardware_ops.c "logcheck mutation"
}

paint_blind_off() {
    restore src/kernel/core/decks/hardware/hardware_ops.c
}

run_paintmut() {
    echo "== paintmut: the op answers OK and paints nothing, and the screen must stay bare =="
    paint_blind_on; build
    paint_boot paintmut "UEFI=on STRICT=on CORES=4 MEM=4G"; local booted=$?
    paint_blind_off
    if [ $booted -ne 0 ]; then bad "paintmut: never reached a shell"; build; return; fi
    V=$(paint_shot_verdict "$SCRATCH/paintmut.ppm")
    case "$V" in
        bare*) ok "paintmut: nothing was painted and the oracle sees it ($V per cent lit)" ;;
        *)     bad "paintmut: the op dropped every frame and the screen was STILL painted ($V) - the oracle cannot see that defect" ;;
    esac
    build   # leave the tree built from clean sources
}

# ===========================================================================
# finish — the crew of a tag is named, the machine's own cabins are held back,
#          and the word `anyway` really ends one
# ===========================================================================
#
# `broadcast` has always walked the carriers of a tag to speak to them; until
# system.proc.crew there was no way to ASK who they are, and killing meant
# knowing a pid — a seat, which is re-let. finish ends the crew of a tag by
# (pid, generation) and refuses, without the word `anyway`, to end a cabin
# wearing the bare `system` tag: the shell, the display daemon and itself.
#
# The display daemon is the instrument, because ending it is survivable — the
# lane dies, boxlib says so and falls back to writing the glass directly, and
# the shell keeps answering. So all three answers are provable on one boot.
finish_boot() {
    local name=$1 args=$2
    make run-stop >/dev/null 2>&1
    # shellcheck disable=SC2086
    make run-bg $args >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    if ! grep -q "BoxOS Shell" build/serial.log 2>/dev/null; then
        make run-stop >/dev/null 2>&1
        return 1
    fi
    sleep 3
    local MARK
    MARK=$(wc -l < build/serial.log)
    local cmd
    for cmd in "finish nobody" "finish display" "finish display anyway" "me"; do
        type_cmd "$cmd"
        sleep 1
        key_press ret
        sleep 4
    done
    tail -n +$((MARK + 1)) build/serial.log | tr -d '\r' > "$SCRATCH/serial.$name.log"
    make run-stop >/dev/null 2>&1
    return 0
}

run_finish() {
    echo "== finish: nobody wears it, the daemon is held back, and anyway ends it =="
    build
    if ! finish_boot finish "STRICT=on CORES=4 MEM=4G"; then
        bad "finish: never reached a shell"; return
    fi
    L="$SCRATCH/serial.finish.log"
    grep -q "nobody: nothing wears that tag" "$L" \
        && ok "finish: a tag nobody wears is said plainly" \
        || bad "finish: an unworn tag was not reported"
    grep -q "wears system - this machine runs on it" "$L" \
        && ok "finish: the daemon is named and held back" \
        || bad "finish: the guard over a system cabin did not speak"
    grep -q "if that is what you want: finish display anyway" "$L" \
        && ok "finish: the way past the guard is spelled out" \
        || bad "finish: the guard did not say how to override it"
    grep -q "^nothing ended$" "$L" \
        && ok "finish: nothing was ended while the guard stood" \
        || bad "finish: something was ended although the guard spoke"
    grep -q "Processes: 2 live" "$L" \
        && ok "finish: after the word anyway the daemon is gone and the shell still answers" \
        || bad "finish: the daemon survived the word anyway, or the shell stopped answering ($(grep -m1 'Processes:' "$L" || echo 'no answer at all'))"
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "finish: the kernel spoke of a stall or fault" \
        || ok "finish: no verdict, no panic"
}

finish_unguarded_on() {
    mutate src/userspace/utils/finish.c
    python3 - <<'EOF2'
p = "src/userspace/utils/finish.c"
s = open(p).read()
anchor = "        if (!anyway && wears(crew->mates[i].tags, SYSTEM_TAG)) {"
assert s.count(anchor) == 1, "finish mutation anchor missing"
s = s.replace(anchor, "        if (false && wears(crew->mates[i].tags, SYSTEM_TAG)) {   /* logcheck mutation: the guard is gone */", 1)
open(p, "w").write(s)
EOF2
    mut_done src/userspace/utils/finish.c "logcheck mutation"
}

finish_unguarded_off() {
    restore src/userspace/utils/finish.c
}

run_finishmut() {
    echo "== finishmut: the guard taken out, and the daemon must fall to a bare finish =="
    finish_unguarded_on; build
    finish_boot finishmut "STRICT=on CORES=4 MEM=4G"; local booted=$?
    finish_unguarded_off
    if [ $booted -ne 0 ]; then bad "finishmut: never reached a shell"; build; return; fi
    L="$SCRATCH/serial.finishmut.log"
    if grep -q "wears system - this machine runs on it" "$L"; then
        bad "finishmut: the guard was taken out and STILL spoke - the oracle cannot see that defect"
    else
        ok "finishmut: without the guard nothing is held back - and the oracle sees the silence"
    fi
    build   # leave the tree built from clean sources
}

# ===========================================================================
# overtake — a finished program's last words are not overtaken by the prompt
# ===========================================================================
#
# The console daemon merges the lanes by the stamp each frame carries, and the
# stamp is cpu_rdtsc read on whatever core the writer was running on. Cores
# agree about that counter only to within what the boot-time sync left them
# (per_core.c tolerates four microseconds), and a program that lists a volume
# pushes its whole output in less time than that. So the stamps of one burst
# and the stamp of a line printed afterwards ON ANOTHER CORE are not reliably
# ordered — and the shell's next prompt landed INSIDE the listing of the
# command that had just finished.
#
# MEASURED on the board: four listings out of twelve cut open at exactly the
# same byte, 891 of 4235. The same byte every time is what a FIXED offset
# between two cores looks like; a race would have scattered. Never once in an
# emulator, where the cores agree exactly — which is why this scenario has to
# MAKE them disagree.
#
# The injection is one writer's clock running ahead from its eleventh word on.
# The prompt, stamped normally, is then older than the rest of that writer's
# burst and the merge puts it in the middle — unless the order comes from the
# death instead of from the clock, which is the fix under test.

# ‼ TWO CONDITIONS ARE NEEDED TO SEE THIS, AND THE BOARD HAS BOTH: the cores
# disagree about the counter, AND the daemon is behind the writer. An emulator
# has neither — the first attempt at this scenario injected only the skew and
# the listings came out whole, because the daemon had drained every frame long
# before the shell woke up. So the second condition is injected too, by making
# one render cost what a render costs on a real framebuffer.
overtake_slow_on() {
    mutate src/userspace/display/display.c
    python3 - <<'EOF2'
p = "src/userspace/display/display.c"
s = open(p).read()
a = "    if (f->kind != CONSOLE_RUN_TEXT) return;"
assert s.count(a) == 1, "overtake slow anchor missing"
inject = "\n    /* logcheck injection: a render costs what one costs on glass. */"
inject += "\n    for (volatile uint32_t z = 0; z < 400000u; z++) { }"
s = s.replace(a, a + inject, 1)
open(p, "w").write(s)
EOF2
    mut_done src/userspace/display/display.c "logcheck injection"
}

overtake_slow_off() {
    restore src/userspace/display/display.c
}

overtake_clockonly_on() {
    mutate src/userspace/boxlib/src/print.c
    python3 - <<'EOF2'
p = "src/userspace/boxlib/src/print.c"
s = open(p).read()
a = "    return __atomic_add_fetch(ps->order, 1u, __ATOMIC_SEQ_CST);"
assert s.count(a) == 1, "overtake mutation anchor missing"
m  = "    /* logcheck mutation: the clock decides again, and two cabins' clocks"
m += "\n     * differ by a fixed amount — which is exactly what an imperfect TSC"
m += "\n     * sync leaves behind. The shell is pid 2; what it spawns has a higher"
m += "\n     * one, so this is two processes whose counters disagree. */"
m += "\n    { static uint32_t n;"
m += "\n      uint64_t t = cpu_rdtsc();"
m += "\n      if (cabin_info()->pid > 2u && ++n > 10u) t += 4000000000ull;"
m += "\n      return t; }"
s = s.replace(a, m, 1)
if "box/core/cabin.h" not in s:
    s = s.replace('#include "box/bay.h"', '#include "box/bay.h"\n#include "box/core/cabin.h"', 1)
if "box/cpu.h" not in s:
    s = s.replace('#include "box/bay.h"', '#include "box/cpu.h"\n#include "box/bay.h"', 1)
open(p, "w").write(s)
EOF2
    mut_done src/userspace/boxlib/src/print.c "logcheck mutation"
}

overtake_clockonly_off() {
    restore src/userspace/boxlib/src/print.c
}

overtake_listing() {
    # overtake_listing NAME — boot, list the volume three times, and report the
    # shortest run of listing that reached the log before a prompt did.
    make run-stop >/dev/null 2>&1
    make run-bg UEFI=on STRICT=on CORES=4 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    if ! grep -q "BoxOS Shell" build/serial.log 2>/dev/null; then
        make run-stop >/dev/null 2>&1; return 1
    fi
    sleep 3
    # Three listings, each driven off two facts and no clock. A listing has
    # STARTED when the utility prints its own heading; it is OVER when the log
    # stops growing. The gap between "the prompt is back" and "the burst has
    # drained" is the whole subject of this scenario, so the second fact has to
    # be the drain and not the prompt.
    local n MARK
    for n in 1 2 3; do
        MARK=$(wc -l < build/serial.log)
        type_cmd "files"
        key_press ret
        wait_for_line build/serial.log "Files:" "$WAIT_QUIET_S" $((MARK + 1))
        wait_for_quiet "$OVERTAKE_DRAIN_QUIET_S"
    done
    cp build/serial.log "$SCRATCH/serial.$1.log"
    make run-stop >/dev/null 2>&1
    return 0
}

# ‼ 4000 WAS A NUMBER TYPED IN ON A DAY THIS VOLUME HELD THE FILES IT HELD.
#
# The old verdict was "the shortest listing is at least 4000 bytes long", which
# is a statement about how many programs happen to be on the medium, not about
# whether a prompt cut one open. Add a file and the whole listing grows; remove
# two and a WHOLE listing drops under the line and the oracle reddens against a
# kernel with nothing wrong with it.
#
# The exact fact is in the run itself and needs no constant at all. A listing
# the prompt did NOT cut has that prompt at its end: nothing of the listing
# follows it. A listing the prompt landed inside has the rest of its ROWS
# stranded after it — which is precisely what was measured on the board, 891
# bytes of 4235 and the other 3344 arriving after the prompt.
#
# So: one line per listing, "<bytes before the prompt> <listing rows after it>".
# A second number of 0 is a whole listing, whatever the volume holds today; a
# positive one is a cut and says how much was stranded; -1 means no prompt ever
# followed the listing at all, which is a third thing and not a pass.
overtake_cuts() {
    python3 - "$1" <<'EOF2'
import re, sys
t = open(sys.argv[1], "rb").read().replace(b"\r", b"")
row = re.compile(rb"(?m)^[!-~]+ +\[")
starts = [m.end() for m in re.finditer(rb"Files:\n", t)]
for k, s in enumerate(starts):
    end = starts[k + 1] if k + 1 < len(starts) else len(t)
    body = t[s:end]
    n = body.find(b"\n~ ")
    if n < 0:
        print("%d -1" % len(body))
        continue
    # The prompt is two bytes with no newline after it, so whatever the merge
    # put next lands on the prompt's OWN line. Drop the "~ " and the rest of
    # that line is read like any other, or a listing stranded by exactly one
    # row would be counted as a listing that was never cut.
    after = body[n + 1:]
    if after.startswith(b"~ "):
        after = after[2:]
    print("%d %d" % (n, len(row.findall(after))))
EOF2
}

# The three numbers every caller of overtake_cuts wants, read once.
overtake_tally() {
    OT_ROWS=$(overtake_cuts "$1")
    OT_COUNT=$(printf '%s\n' "$OT_ROWS" | grep -c '[0-9]')
    OT_CUT=$(printf '%s\n' "$OT_ROWS"   | awk '$2 > 0 { n++ } END { print n+0 }')
    OT_NOPROMPT=$(printf '%s\n' "$OT_ROWS" | awk '$2 < 0 { n++ } END { print n+0 }')
    OT_SHORTEST=$(printf '%s\n' "$OT_ROWS" | awk 'NF { if (m == "" || $1 < m) m = $1 } END { print (m == "" ? 0 : m) }')
}

run_overtake() {
    echo "== overtake: a finished program's last words come before the next prompt =="
    overtake_slow_on; build
    local booted=1
    overtake_listing overtake && booted=0
    overtake_slow_off
    if [ $booted -ne 0 ]; then bad "overtake: never reached a shell"; build; return; fi

    overtake_tally "$SCRATCH/serial.overtake.log"
    if [ "$OT_COUNT" -lt 3 ]; then
        bad "overtake: only $OT_COUNT of 3 listings reached the log — the machine was not asked what this scenario means to ask"
    elif [ "$OT_NOPROMPT" -gt 0 ]; then
        bad "overtake: $OT_NOPROMPT listing(s) were never followed by a prompt at all"
    elif [ "$OT_CUT" -gt 0 ]; then
        bad "overtake: a prompt cut $OT_CUT of $OT_COUNT listings open (the shortest ran $OT_SHORTEST bytes before it) — the clock is still deciding the order"
    else
        ok "overtake: all $OT_COUNT listings reached the log whole ($OT_SHORTEST bytes) before their prompt, with the daemon left far behind the writer"
    fi
    build
}

run_overtakemut() {
    echo "== overtakemut: with the clock deciding again, the prompt must cut in =="
    overtake_clockonly_on; overtake_slow_on; build
    local booted=1
    overtake_listing overtakemut && booted=0
    overtake_clockonly_off; overtake_slow_off
    if [ $booted -ne 0 ]; then bad "overtakemut: never reached a shell"; build; return; fi

    overtake_tally "$SCRATCH/serial.overtakemut.log"
    if [ "$OT_COUNT" -lt 3 ]; then
        bad "overtakemut: only $OT_COUNT of 3 listings reached the log — the scenario proved nothing either way"
    elif [ "$OT_CUT" -gt 0 ]; then
        ok "overtakemut: a prompt cut $OT_CUT of $OT_COUNT listings open after $OT_SHORTEST bytes, stranding rows behind it, and the oracle sees it"
    else
        bad "overtakemut: the death was taken out of the order and every listing was STILL whole - the oracle cannot see that defect"
    fi
    build
}

# ===========================================================================
# orderlife — a Bay window outlives the strand that opened it
# ===========================================================================
#
# The console's order is a counter in a Bay, and every printing strand maps it.
# The question this asks is not about the console at all: it is whether a Bay
# window survives the STRAND that opened it, because the window lives in the
# CABIN's address space and every sibling runs in that same space.
#
# It used to not survive. A Bay claim belonged to the strand, and the cleanup
# on strand death unmapped it — out from under siblings still reading through
# it. The console found it: one shared order pointer, opened by whichever
# strand printed first, vanished for all of them when that strand finished.
#
# ‼ AND THE ORDINARY SHAPE HIDES IT, which is why this scenario exists rather
# than trusting `lines`. print_stress prints its banner from the MAIN strand
# before it spawns anybody, so the main strand opens the order and outlives
# every worker, and nothing ever faults. Remove that one line and the first
# strand to print is a worker — one that runs its two thousand lines and then
# exits while fifteen siblings are still printing. That is the shape, and it
# is one line of difference from a program that has passed for months.
#
# The scenario keeps that line removed for BOTH halves, so the two runs differ
# only by the defect under test and not by what the program does.
orderlife_shape_on() {
    mutate src/userspace/apps/print_stress.c
    python3 - <<'EOF2'
p = "src/userspace/apps/print_stress.c"
s = open(p).read()
a = """    printf("[PS] print_stress starting: %u strands x %u iters each\\n",
           STRAND_COUNT, ITERS);
    io_flush();
"""
assert s.count(a) == 1, "orderlife shape anchor missing"
s = s.replace(a, """    /* logcheck mutation: the banner is gone, so the first strand to print is
     * a spawned worker and not main. */
""", 1)
open(p, "w").write(s)
EOF2
    mut_done src/userspace/apps/print_stress.c "logcheck mutation"
}

orderlife_shape_off() { restore src/userspace/apps/print_stress.c; }

# The defect, put back in one line: let ANY strand's death take the whole
# cabin's Bay windows, which is what strand-scoped claims did to a sibling's
# window and is the class this is guarding. The real rule is that only the
# cabin's own teardown may do this (cabin.c, cabin_destroy).
orderlife_strandkill_on() {
    mutate src/kernel/core/process/process.c
    python3 - <<'EOF2'
p = "src/kernel/core/process/process.c"
s = open(p).read()
a = "    BrookCleanupProcess(proc);"
assert s.count(a) == 1, "orderlife strandkill anchor missing"
m  = "    /* logcheck mutation: a strand's death takes the cabin's Bay windows,"
m += "\n     * which is what strand-scoped claims did to a sibling's. */"
m += "\n    if (proc->cabin) BayCleanupCabin(proc->cabin);"
m += "\n" + a
s = s.replace(a, m, 1)
open(p, "w").write(s)
EOF2
    mut_done src/kernel/core/process/process.c "logcheck mutation"
}

orderlife_strandkill_off() { restore src/kernel/core/process/process.c; }

# orderlife_run NAME — boot sixteen cores, run print_stress, and report what
# reached the log. Returns 1 if the machine never reached a shell at all.
orderlife_run() {
    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=16 MEM=8G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    if ! grep -q "BoxOS Shell" build/serial.log 2>/dev/null; then
        make run-stop >/dev/null 2>&1; return 1
    fi
    local MARK
    MARK=$(wc -l < build/serial.log)
    type_cmd "print_stress"
    key_press ret
    wait_for_line build/serial.log "PS SUMMARY" "$WAIT_QUIET_S" $((MARK + 1))
    tail -n +$((MARK + 1)) build/serial.log | tr -d '\r' > "$SCRATCH/serial.$1.log"
    make run-stop >/dev/null 2>&1
    return 0
}

run_orderlife() {
    echo "== orderlife: the strand that opened the console's order finishes first, and the rest keep printing =="
    orderlife_shape_on; build
    local booted=1
    orderlife_run orderlife && booted=0
    orderlife_shape_off
    if [ $booted -ne 0 ]; then bad "orderlife: never reached a shell"; build; return; fi

    local WHOLE
    WHOLE=$(grep -cE '^\[PS-[0-9]{2}\] [0-9]{8}$' "$SCRATCH/serial.orderlife.log" | tr -d ' ')
    echo "     $WHOLE of 32000 lines whole"
    if grep -q "PS SUMMARY.*PASS" "$SCRATCH/serial.orderlife.log"; then
        ok "orderlife: every strand printed to the end though the first one to print had long finished"
    else
        bad "orderlife: print_stress never finished — a worker printed first and the survivors lost something"
    fi
    if grep -qE "PAGE FAULT|#PF|PANIC" "$SCRATCH/serial.orderlife.log"; then
        bad "orderlife: the run faulted"
    else
        ok "orderlife: and nothing faulted"
    fi
    build
}

run_orderlifemut() {
    echo "== orderlifemut: let a strand's death take the cabin's windows, and require the oracle to notice =="
    orderlife_shape_on; orderlife_strandkill_on; build
    local booted=1
    orderlife_run orderlifemut && booted=0
    orderlife_shape_off; orderlife_strandkill_off
    if [ $booted -ne 0 ]; then
        ok "orderlifemut: the machine could not even reach a shell with windows pulled from under it, and the oracle sees it"
        build; return
    fi

    if grep -q "PS SUMMARY.*PASS" "$SCRATCH/serial.orderlifemut.log" \
       && ! grep -qE "PAGE FAULT|#PF|PANIC" "$SCRATCH/serial.orderlifemut.log"; then
        bad "orderlifemut: a strand's death took the cabin's Bay windows and print_stress finished anyway - the oracle cannot see that defect"
    else
        ok "orderlifemut: the survivors lost the window under them, and the oracle sees it"
    fi
    build
}

# ===========================================================================
# batchfail — one refused op costs its own cell, and nothing behind it
# ===========================================================================
#
# A Manifest stops at its first refusal: manifest_exec.c breaks out of the
# dispatch loop on an op that fails and is not marked OPTIONAL, so every op
# BEHIND it never runs. That is right for a Manifest that is one transaction
# and wrong for the display daemon's, which packs a whole burst of output into
# one batch — a single cell the kernel would not draw took the entire rest of
# the burst with it, silently, up to and including the prompt printed at the
# end. Reported from the board: after `files` there was no prompt, after `me`
# there was, over and over.
#
# The instrument has to MAKE an op refuse, because on a desk nothing does. One
# PUTSTRING in the middle of the run is made to fail, and the question is what
# reaches the log ring afterwards — the ring is written by the op as it prints,
# so it is a faithful witness of what was EXECUTED, not of what was asked.
#
#   with the ops OPTIONAL : one line of output is missing and everything after
#                           it is there, prompt included
#   without               : the tail of the burst is gone

batchfail_inject_on() {
    mutate src/kernel/core/decks/hardware/hardware_ops.c
    python3 - <<'EOF2'
p = "src/kernel/core/decks/hardware/hardware_ops.c"
s = open(p).read()
a = """    uint32_t fg    = hw_color_param(&op->params[0]);
    uint32_t bg    = hw_color_param(&op->params[4]);
    uint8_t  flags = op->params[8];"""
assert s.count(a) == 1, "batchfail inject anchor missing"
s = s.replace(a, """    /* logcheck injection: one run in sixteen refuses.
     * ‼ A REFUSAL COUNTED FROM BOOT IS NOT A REFUSAL INSIDE THE BURST. The
     * first try failed exactly once, at the tenth run of the session — which
     * is somewhere in the boot banner, hundreds of runs before the listing
     * this scenario measures. The mutation caught it: the tail arrived with
     * the ops made non-optional, because nothing had refused inside that
     * batch at all. It has to keep refusing, so that a burst of seventy runs
     * is certain to contain one. */
    { static uint32_t n; if ((++n % 16u) == 0) return ERR_INVALID_ARGUMENT; }
""" + a, 1)
open(p, "w").write(s)
EOF2
    mut_done src/kernel/core/decks/hardware/hardware_ops.c "logcheck injection"
}

batchfail_inject_off() {
    restore src/kernel/core/decks/hardware/hardware_ops.c
}

batchfail_optional_off() {
    mutate src/userspace/boxlib/src/vga.c
    python3 - <<'EOF2'
p = "src/userspace/boxlib/src/vga.c"
s = open(p).read()
a = "    return ManifestBuilderAddOp(&s_mb, DECK_HARDWARE, opcode, OP_FLAG_OPTIONAL,"
assert s.count(a) == 1, "batchfail optional anchor missing"
s = s.replace(a, "    return ManifestBuilderAddOp(&s_mb, DECK_HARDWARE, opcode, 0,   /* logcheck mutation */", 1)
open(p, "w").write(s)
EOF2
    mut_done src/userspace/boxlib/src/vga.c "logcheck mutation"
}

batchfail_optional_on() {
    restore src/userspace/boxlib/src/vga.c
}

batchfail_run() {
    # batchfail_run NAME — boot, list the volume, keep the window of the log.
    make run-stop >/dev/null 2>&1
    make run-bg UEFI=on STRICT=on CORES=4 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    if ! grep -q "BoxOS Shell" build/serial.log 2>/dev/null; then
        make run-stop >/dev/null 2>&1; return 1
    fi
    sleep 3
    local MARK
    MARK=$(wc -l < build/serial.log)
    type_cmd "files"
    key_press ret
    # Started when the utility prints its heading, over when the log stops
    # growing. A flat six seconds was a guess that would cut the very tail this
    # scenario is about, on any host slower than the one it was written on.
    wait_for_line build/serial.log "Files:" "$WAIT_QUIET_S" $((MARK + 1))
    wait_for_quiet "$OVERTAKE_DRAIN_QUIET_S"
    tail -n +$((MARK + 1)) build/serial.log | tr -d '\r' > "$SCRATCH/serial.$1.log"
    # The mount line is in the BOOT, before the window — and it is the only
    # place the guest states how many files the volume holds, which is what the
    # listing below is measured against.
    tr -d '\r' < build/serial.log > "$SCRATCH/serial.$1.full.log"
    make run-stop >/dev/null 2>&1
    return 0
}

# How many files the guest itself says the volume holds, and how many listing
# rows arrived. A row is a filename, spaces, then the bracket that opens its
# tags — the shape files.c prints and nothing else in the log has.
batchfail_tally() {
    BF_TOTAL=$(grep -oE "MOUNTED — [0-9]+ file" "$2" | tail -1 | grep -oE "[0-9]+")
    BF_ROWS=$(grep -cE "^[^ ]+ +\[" "$1")
}

run_batchfail() {
    echo "== batchfail: a refused op costs its own cell and nothing behind it =="
    batchfail_inject_on; build
    local booted=1
    batchfail_run batchfail && booted=0
    batchfail_inject_off
    if [ $booted -ne 0 ]; then bad "batchfail: never reached a shell"; build; return; fi

    local L="$SCRATCH/serial.batchfail.log"
    batchfail_tally "$L" "$SCRATCH/serial.batchfail.full.log"

    # ‼ 50 WAS A NUMBER TYPED IN AGAINST THE FILES THIS VOLUME HELD THAT DAY.
    #
    # The quantity is now taken from the run: the guest states how many files
    # it mounted, and the listing is measured against that. The line between
    # the two behaviours is not narrow and does not need to be guessed — with
    # the ops OPTIONAL a refusal costs its own cell and the burst carries on,
    # so nearly every row arrives; without, each batch dies at its first
    # refusal and roughly half the listing is gone. Measured on this listing:
    # 60 rows against 30. Half the volume's own count sits between them and
    # moves with the volume, which a typed-in 50 could not.
    #
    # Naming a PARTICULAR row instead would be too strict: with one run in
    # sixteen refused the casualty can be any row, the last one included.
    if [ -z "$BF_TOTAL" ]; then
        bad "batchfail: the mount never said how many files the volume holds — the harness has nothing to measure the listing against"
    else
        [ "$BF_ROWS" -gt $((BF_TOTAL / 2)) ]
        chk $? "batchfail: $BF_ROWS of the volume's $BF_TOTAL files reached the log with one run in sixteen refused"
    fi

    grep -qE "^~" "$L"
    chk $? "batchfail: and the prompt printed after the burst"

    build   # leave the tree built from clean sources
}

run_batchfailmut() {
    echo "== batchfailmut: with the ops not optional, the tail must disappear =="
    batchfail_inject_on; batchfail_optional_off; build
    local booted=1
    batchfail_run batchfailmut && booted=0
    batchfail_inject_off; batchfail_optional_on
    if [ $booted -ne 0 ]; then bad "batchfailmut: never reached a shell"; build; return; fi

    local L="$SCRATCH/serial.batchfailmut.log"
    batchfail_tally "$L" "$SCRATCH/serial.batchfailmut.full.log"
    if [ -z "$BF_TOTAL" ]; then
        bad "batchfailmut: the mount never said how many files the volume holds — the harness has nothing to measure the listing against"
    elif [ "$BF_ROWS" -gt $((BF_TOTAL / 2)) ]; then
        bad "batchfailmut: the ops were made non-optional and $BF_ROWS of $BF_TOTAL rows STILL arrived - the oracle cannot see that defect"
    else
        ok "batchfailmut: the refusal took the rest of the burst with it — $BF_ROWS of the volume's $BF_TOTAL files reached the log, and the oracle sees it"
    fi
    build
}

# ===========================================================================
# caret — there is one cursor on the screen, and it is where the program put it
# ===========================================================================
#
# The caret is the part of a console a person actually watches, and until this
# was measured nobody could say where it was: the serial mirror carries the
# TEXT a program said and says nothing at all about the cursor. So this reads
# the glass itself. A screendump is 8x16 cells of pixels; the GOP backend draws
# the caret as two solid rows at the bottom of a cell, and the 8x16 font is
# right here in the tree, so a cell can be told back into the character it
# carries AND whether a caret is standing on it.
#
# Two things are checked, and they are different defects:
#   ONE caret — a caret is pixels inside the surface, so a scroll carries it up
#               with the text; erasing it at the row it was PUT in leaves a
#               ghost on the row it moved to, one per scroll, climbing the
#               screen. MEASURED 2026-09-10: three presses of Enter left four.
#   WHERE it is — a cursor move is one op, a single-op Manifest is not wrapped
#               in a Canvas batch, and CanvasSetCursor used to change the
#               cursor without committing. Left, Right, Home, End and Backspace
#               moved the insertion point and left the caret behind.

caret_read() {
    # caret_read PPM — prints "carets N at r,c r,c ..." and the last screen row
    # that carries anything, so a verdict can name what it saw.
    python3 - "$1" <<'EOF2'
import re, sys, collections
ROOT = "."
src = open(ROOT + "/src/kernel/drivers/video/font/vga_font.h").read()
body = src[src.index("vga_font_8x16[256][16] = {"):]
rows = re.findall(r"\{([^{}]*)\}", body)
FONT = []
for r in rows[:256]:
    v = [int(x, 0) for x in r.split(",") if x.strip()]
    if len(v) == 16: FONT.append(tuple(v))
BY = {}
TOP = {}
for code in range(255, -1, -1):
    # A cell of nothing is a SPACE, not a NUL: the glyphs are identical and the
    # lower code would win, and then a blank row could not be told from a row
    # of text by trimming it.
    if code == 0: continue
    BY[FONT[code]] = code
    TOP.setdefault(FONT[code][:14], code)

d = open(sys.argv[1], "rb").read()
parts = d.split(b"\n", 3)
w, h = map(int, parts[1].split())
px = parts[3]
def pix(x, y):
    i = (y * w + x) * 3
    return (px[i], px[i+1], px[i+2])

carets = []
lines = []
for r in range(h // 16):
    line = []
    for c in range(w // 8):
        box = [[pix(c*8 + x, r*16 + y) for x in range(8)] for y in range(16)]
        flat = [p for row in box for p in row]
        cnt = collections.Counter(flat)
        bg = cnt.most_common(1)[0][0]
        bits = tuple(sum(1 << (7-x) for x in range(8) if box[y][x] != bg) for y in range(16))
        ch = BY.get(bits)
        caret = False
        if ch is None:
            if bits[14] == 0xFF and bits[15] == 0xFF:
                ch = TOP.get(bits[:14])
                if ch is not None: caret = True
        elif bits[14] == 0xFF and bits[15] == 0xFF and not any(bits[:14]):
            caret = True; ch = 32
        if ch is None: ch = ord('?')
        if caret: carets.append((r, c))
        line.append(chr(ch) if 32 <= ch < 127 else '.')
    lines.append("".join(line).rstrip())

last = ""
for i, l in enumerate(lines):
    if l: last = "%d|%s" % (i, l)
print("carets %d at %s" % (len(carets), " ".join("%d,%d" % x for x in carets)))
print("last " + last)
EOF2
}

caret_boot() {
    make run-stop >/dev/null 2>&1
    make run-bg UEFI=on STRICT=on CORES=4 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3
    grep -q "BoxOS Shell" build/serial.log 2>/dev/null
}

caret_run() {
    # caret_run NAME — one scripted editing session; leaves two readings.
    local n=$1
    type_cmd "abcdef"
    sleep 1
    key_press left
    key_press left
    sleep 2
    mon_shot "$SCRATCH/$n.edit.ppm"
    caret_read "$SCRATCH/$n.edit.ppm" > "$SCRATCH/$n.edit.txt" 2>&1

    # then a screenful of output and three scrolls, which is what left ghosts
    key_press home
    key_press delete || true
    key_press ret
    sleep 2
    type_cmd "files"
    key_press ret
    sleep 4
    local i
    for i in 1 2 3; do key_press ret; sleep 1; done
    sleep 2
    mon_shot "$SCRATCH/$n.scroll.ppm"
    caret_read "$SCRATCH/$n.scroll.ppm" > "$SCRATCH/$n.scroll.txt" 2>&1
}

run_caret() {
    echo "== caret: one cursor on the glass, standing where the editor put it =="
    build
    if ! caret_boot; then bad "caret: never reached a shell"; make run-stop >/dev/null 2>&1; return; fi
    caret_run caret
    make run-stop >/dev/null 2>&1

    local E S EN SN
    E=$(head -1 "$SCRATCH/caret.edit.txt")
    S=$(head -1 "$SCRATCH/caret.scroll.txt")
    EN=$(echo "$E" | awk '{print $2}')
    SN=$(echo "$S" | awk '{print $2}')

    [ "$EN" = 1 ]
    chk $? "caret: one cursor while a line is being edited ($E)"

    [ "$SN" = 1 ]
    chk $? "caret: still one after a screenful of output and three scrolls ($S)"

    # Where it stands: `abcdef` typed, then Left twice, so the caret must be on
    # the cell holding 'e' — two back from the end of what was typed.
    local LINE COL WANT
    LINE=$(tail -1 "$SCRATCH/caret.edit.txt")
    COL=$(echo "$E" | awk '{print $4}' | cut -d, -f2)
    WANT=$(echo "$LINE" | sed 's/^last [0-9]*|//' | awk '{ i=index($0,"abcdef"); print (i>0 ? i-1+4 : -1) }')
    if [ -n "$COL" ] && [ "$COL" = "$WANT" ]; then
        ok "caret: it sits on the cell the editor is editing (column $COL of \"$(echo "$LINE" | sed 's/^last [0-9]*|//')\")"
    else
        bad "caret: the editor is two cells back from the end of \"abcdef\" and the caret is at column $COL, not $WANT"
    fi
}

caret_blind_on() {
    mutate src/kernel/drivers/video/canvas.c
    python3 - <<'EOF2'
p = "src/kernel/drivers/video/canvas.c"
s = open(p).read()
a = """    bool opened = implicit_open_locked(c);
    c->col = (uint32_t)x < c->cols ? (uint32_t)x : c->cols - 1u;
    c->row = (uint32_t)y < c->rows ? (uint32_t)y : c->rows - 1u;
    implicit_close_locked(c, opened);"""
assert s.count(a) == 1, "caret mutation anchor missing"
s = s.replace(a, """    /* logcheck mutation: the cursor moves and the glass is not told */
    c->col = (uint32_t)x < c->cols ? (uint32_t)x : c->cols - 1u;
    c->row = (uint32_t)y < c->rows ? (uint32_t)y : c->rows - 1u;""", 1)
open(p, "w").write(s)
EOF2
    mut_done src/kernel/drivers/video/canvas.c "logcheck mutation"
}

caret_blind_off() {
    restore src/kernel/drivers/video/canvas.c
}

run_caretmut() {
    echo "== caretmut: with the commit taken out, the caret must stop following the cursor =="
    caret_blind_on; build
    local booted=1
    caret_boot && booted=0
    if [ $booted -eq 0 ]; then caret_run caretmut; fi
    make run-stop >/dev/null 2>&1
    caret_blind_off
    if [ $booted -ne 0 ]; then bad "caretmut: never reached a shell"; build; return; fi

    local E COL LINE WANT
    E=$(head -1 "$SCRATCH/caretmut.edit.txt")
    LINE=$(tail -1 "$SCRATCH/caretmut.edit.txt")
    COL=$(echo "$E" | awk '{print $4}' | cut -d, -f2)
    WANT=$(echo "$LINE" | sed 's/^last [0-9]*|//' | awk '{ i=index($0,"abcdef"); print (i>0 ? i-1+4 : -1) }')
    if [ -n "$COL" ] && [ "$COL" = "$WANT" ]; then
        bad "caretmut: the commit was removed and the caret STILL followed the arrows — the oracle cannot see that defect"
    else
        ok "caretmut: the caret stayed at column $COL instead of $WANT, and the caret oracle would have caught it"
    fi
    build   # leave the tree built from clean sources
}

# ===========================================================================
# grow — the volume takes the ground it is given
# ===========================================================================
#
# An image is written to a medium with dd, and the medium is nearly always
# bigger than the image. Until the Deed could be amended in place, the volume
# simply did not use the rest: a 90 MiB image on a 64 GB stick left 63 GB of it
# dark for ever, and nothing said so.
#
# The instrument is the image itself. The MBR entry that claims the volume's
# ground is stretched and the file behind it grown — exactly what writing the
# image to a bigger medium and enlarging the partition does — and then the
# machine is booted on it. Nothing inside the volume is touched: whether it
# takes the ground is the machine's business, which is the whole point.

grow_stretch() {
    # grow_stretch IMG SECTORS — give the BoxOS partition more ground.
    python3 - "$1" "$2" <<'EOF2'
import struct, sys, os
img, more = sys.argv[1], int(sys.argv[2])
f = open(img, "r+b")
f.seek(446); table = f.read(64)
for i in range(4):
    e = table[i*16:(i+1)*16]
    if e[4] == 0x7f:
        start, count = struct.unpack_from("<II", e, 8)
        ent = bytearray(e)
        struct.pack_into("<I", ent, 12, count + more)
        f.seek(446 + i*16); f.write(bytes(ent))
        end = (start + count + more) * 512
        f.seek(0, os.SEEK_END)
        if f.tell() < end: f.truncate(end)
        f.close()
        print("%d %d" % (start, count + more))
        sys.exit(0)
f.close(); sys.exit(1)
EOF2
}

grow_boot() {
    # grow_boot NAME — boot the image as it stands and keep the log window.
    make run-stop >/dev/null 2>&1
    make run-bg UEFI=on STRICT=on CORES=4 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 2
    cp build/serial.log "$SCRATCH/serial.$1.log"
    grep -q "BoxOS Shell" build/serial.log 2>/dev/null
}

grow_rollback_head() {
    # Put the PRE-GROWTH head deed back: the state a power cut between the two
    # writes of a growth leaves behind — a new far copy and an old head.
    python3 - "$1" "$2" "$3" <<'EOF2'
import sys
pristine, img, start = sys.argv[1], sys.argv[2], int(sys.argv[3])
off = start * 512
o = open(pristine, "rb"); o.seek(off); head = o.read(4096); o.close()
f = open(img, "r+b"); f.seek(off); f.write(head); f.close()
EOF2
}

run_grow() {
    echo "== grow: a volume on a bigger medium takes the ground behind it =="
    rm -f build/boxos.img   # a scenario that measures growth starts from a volume that has not grown
    build
    make run-stop >/dev/null 2>&1

    # The stretched image goes under the manifest as the build product it is,
    # so a run that dies anywhere below leaves no stretched image behind for
    # the next scenario to measure.
    mutate_product build/boxos.img
    local START
    START=$(grow_stretch build/boxos.img 409600 | cut -d' ' -f1)
    if [ -z "$START" ]; then
        bad "grow: the image has no BoxOS partition to stretch"
        restore build/boxos.img; build; return
    fi

    if ! grow_boot grow; then
        bad "grow: never reached a shell"
        make run-stop >/dev/null 2>&1; restore build/boxos.img; build; return
    fi
    local L="$SCRATCH/serial.grow.log"

    grep -q "takes the rest" "$L"
    chk $? "grow: the volume sees the ground behind it and takes it"

    grep -qE "the volume now runs [0-9]+ sectors; its data run is [0-9]+ blocks, [0-9]+ more" "$L"
    chk $? "grow: $(grep -oE 'its data run is [0-9]+ blocks, [0-9]+ more than it had' "$L" | tail -1)"

    grep -q "far copy has moved to sector" "$L"
    chk $? "grow: the far copy moved to the new end"

    grep -q "is inside the data run now and has been erased" "$L"
    chk $? "grow: the deed that used to sit at the old end was erased"

    grep -q "its far copy agrees — the whole volume is present" "$L"
    chk $? "grow: both copies agree about the volume it has become"

    # The free count and the data run have to be a possible pair of numbers.
    local DB FB
    DB=$(grep -oE "^\[TagFS\] [0-9]+ data blocks" "$L" | tail -1 | grep -oE "[0-9]+" || true)
    FB=$(grep -oE "[0-9]+ free," "$L" | tail -1 | grep -oE "[0-9]+" || true)
    if [ -n "$DB" ] && [ -n "$FB" ] && [ "$FB" -le "$DB" ]; then
        ok "grow: $FB blocks free out of $DB — a volume cannot have more free than it has"
    else
        bad "grow: the volume claims $FB free out of $DB data blocks"
    fi

    grep -qE "MOUNTED — [0-9]+ file" "$L"
    chk $? "grow: $(grep -oE 'MOUNTED — [0-9]+ file\(s\)' "$L" | tail -1)"

    # And a file laid down before the growth still reads.
    local MARK
    MARK=$(wc -l < build/serial.log)
    type_cmd "show hello.txt"
    key_press ret
    sleep 3
    tail -n +$((MARK + 1)) build/serial.log | tr -d '\r' | grep -q "Hello from BoxOS"
    chk $? "grow: a file written before the growth still reads afterwards"

    # Boot it again: the volume now fills its ground and says so.
    if grow_boot grow2; then
        grep -q "fills the ground it was given" "$SCRATCH/serial.grow2.log"
        chk $? "grow: the next mount finds nothing left to take, and says so"
    else
        bad "grow: the grown volume would not boot a second time"
    fi

    make run-stop >/dev/null 2>&1
    restore build/boxos.img   # the stretched image is discarded
    build                     # and an unstretched one is made again
}

run_growcut() {
    echo "== growcut: a growth cut between its two writes still mounts, and finishes =="
    #
    # The order is the crash safety: the far copy is written first, the head
    # second, and the head is the commit point. This puts the machine in the
    # exact state a power cut between the two leaves — a new far copy and the
    # old head — by rolling the head deed back on the medium afterwards.
    rm -f build/boxos.img   # a scenario that measures growth starts from a volume that has not grown
    build
    make run-stop >/dev/null 2>&1
    # ‼ THIS COPY IS EVIDENCE, NOT A BACKUP. The scenario needs the bytes of the
    # head deed as they stood BEFORE the growth, because writing them back is
    # how the power cut between the two writes is reproduced. Putting the image
    # back afterwards is the manifest's job, below, and it does it by deleting.
    cp build/boxos.img "$SCRATCH/growcut.pristine"

    mutate_product build/boxos.img
    local START
    START=$(grow_stretch build/boxos.img 409600 | cut -d' ' -f1)
    if [ -z "$START" ]; then
        bad "growcut: the image has no BoxOS partition to stretch"
        restore build/boxos.img; build; return
    fi

    if ! grow_boot growcut1; then
        bad "growcut: never reached a shell"
        make run-stop >/dev/null 2>&1; restore build/boxos.img; build; return
    fi
    grep -q "the volume now runs" "$SCRATCH/serial.growcut1.log"
    chk $? "growcut: the volume grew once, so there is a growth to cut"

    make run-stop >/dev/null 2>&1
    sleep 1
    grow_rollback_head "$SCRATCH/growcut.pristine" build/boxos.img "$START"

    if ! grow_boot growcut2; then
        bad "growcut: a half-written growth would not boot"
        make run-stop >/dev/null 2>&1; restore build/boxos.img; build; return
    fi
    local L="$SCRATCH/serial.growcut2.log"

    grep -qE "MOUNTED — [0-9]+ file" "$L"
    chk $? "growcut: the volume mounts whole — $(grep -oE 'MOUNTED — [0-9]+ file\(s\)' "$L" | tail -1)"

    grep -q "takes the rest" "$L"
    chk $? "growcut: and finishes the growth from the head's own numbers"

    local DB FB
    DB=$(grep -oE "^\[TagFS\] [0-9]+ data blocks" "$L" | tail -1 | grep -oE "[0-9]+" || true)
    FB=$(grep -oE "[0-9]+ free," "$L" | tail -1 | grep -oE "[0-9]+" || true)
    if [ -n "$DB" ] && [ -n "$FB" ] && [ "$FB" -le "$DB" ]; then
        ok "growcut: $FB free out of $DB — the ground taken twice was not counted twice"
    else
        bad "growcut: the volume claims $FB free out of $DB data blocks"
    fi

    grep -qE "PANIC|\[EXCEPTION\]" "$L" \
        && bad "growcut: the kernel faulted on a half-written growth" \
        || ok "growcut: no panic, no fault"

    make run-stop >/dev/null 2>&1
    restore build/boxos.img
    build
}

grow_blind_on() {
    mutate src/kernel/tagfs/tagfs.c
    python3 - <<'EOF2'
p = "src/kernel/tagfs/tagfs.c"
s = open(p).read()
anchor = "        volume_take_more_ground(seat, ground_run, &head);"
assert s.count(anchor) == 1, "grow mutation anchor missing"
s = s.replace(anchor, "        (void)ground_run;   /* logcheck mutation: the ground is left unclaimed */", 1)
open(p, "w").write(s)
EOF2
    mut_done src/kernel/tagfs/tagfs.c "logcheck mutation"
}

grow_blind_off() {
    restore src/kernel/tagfs/tagfs.c
}

run_growmut() {
    echo "== growmut: with the taking removed, the volume must leave the ground unclaimed =="
    rm -f build/boxos.img
    grow_blind_on; build
    make run-stop >/dev/null 2>&1
    mutate_product build/boxos.img
    grow_stretch build/boxos.img 409600 >/dev/null
    local booted=1
    grow_boot growmut && booted=0
    grow_blind_off
    restore build/boxos.img
    if [ $booted -ne 0 ]; then bad "growmut: never reached a shell"; build; return; fi

    local L="$SCRATCH/serial.growmut.log"
    if grep -q "takes the rest" "$L"; then
        bad "growmut: the taking was removed and the volume STILL grew — the oracle cannot see that defect"
    else
        ok "growmut: nothing was taken, and the grow oracle would have caught it"
    fi
    make run-stop >/dev/null 2>&1
    build   # leave the tree built from clean sources
}


util_boot() {
    # util_boot NAME "RUN-BG ARGS" COMMAND DONE_REGEX [QUIET_S]
    #
    # The fifth argument used to be a number of LOOKS — a ceiling on how long
    # the command was allowed to take, which is a guess about a machine. It is
    # now the quiet window: how long the machine may say nothing before this
    # gives up on it. Left out, it is the ordinary one.
    local name=$1 args=$2 cmd=$3 done_re=$4 quiet=${5:-$WAIT_QUIET_S}
    make run-stop >/dev/null 2>&1
    # shellcheck disable=SC2086
    make run-bg $args >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    if ! grep -q "BoxOS Shell" build/serial.log 2>/dev/null; then
        make run-stop >/dev/null 2>&1
        return 1
    fi
    sleep 3
    local MARK
    MARK=$(wc -l < build/serial.log)
    type_cmd "$cmd"
    sleep 1
    key_press ret
    wait_for_line -E build/serial.log "$done_re" "$quiet" $((MARK + 1))
    sleep 2
    tail -n +$((MARK + 1)) build/serial.log | tr -d '\r' > "$SCRATCH/serial.$name.log"
    make run-stop >/dev/null 2>&1
    return 0
}

run_brigade() {
    echo "== brigade: 300 strands parked on one word, one wake, all 300 back =="
    build
    if ! util_boot brigade "STRICT=on CORES=16 MEM=8G" strandpark "\[SPK\] (PASS|FAIL): brigade"; then
        bad "brigade: never reached a shell"; return
    fi
    L="$SCRATCH/serial.brigade.log"
    grep -q "\[SPK\] PASS: brigade of 300 parked, one wake, 300 back" "$L" \
        && ok "brigade: one wake reached all 300 sleepers" \
        || bad "brigade: $(grep -m1 '\[SPK\] FAIL' "$L" || echo 'no verdict from strandpark at all')"
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "brigade: the kernel spoke of a stall or fault" \
        || ok "brigade: no verdict, no panic"
}

# The oracle measured against itself: put the tray of 256 back, and 44
# sleepers must be left behind — and the oracle must see them.
brigade_tray_on() {
    mutate src/kernel/core/decks/system/sync_ops.c
    python3 - <<'EOF2'
p = "src/kernel/core/decks/system/sync_ops.c"
s = open(p).read()
anchor = "    while (e && (count == 0 || wake_count < count))\n"
assert s.count(anchor) == 1, "brigade mutation anchor missing"
s = s.replace(anchor, "    while (e && (count == 0 || wake_count < count) && wake_count < 256u)   /* logcheck mutation: the tray of 256 */\n", 1)
open(p, "w").write(s)
EOF2
    mut_done src/kernel/core/decks/system/sync_ops.c "logcheck mutation"
}

brigade_tray_off() {
    restore src/kernel/core/decks/system/sync_ops.c
}

run_brigademut() {
    echo "== brigademut: the tray of 256 put back, and 44 sleepers must be left behind =="
    brigade_tray_on; build
    util_boot brigademut "STRICT=on CORES=16 MEM=8G" strandpark "\[SPK\] (PASS|FAIL): brigade"; local booted=$?
    brigade_tray_off
    if [ $booted -ne 0 ]; then bad "brigademut: never reached a shell"; build; return; fi
    L="$SCRATCH/serial.brigademut.log"
    if grep -q "\[SPK\] FAIL: brigade: one wake reached 256 of 300 sleepers" "$L"; then
        ok "brigademut: the tray left 44 asleep — and the oracle sees them"
    else
        bad "brigademut: the tray put back and the brigade STILL all woke — the oracle cannot see that defect ($(grep -m1 '\[SPK\] .*brigade' "$L" || echo 'no verdict'))"
    fi
    build   # leave the tree built from clean sources
}

# ===========================================================================
# headcount — every carrier of a tag is told once, and every one answers
# ===========================================================================
#
# system.broadcast used to take the carriers of a tag into a stack tray of
# 256 and stop when it was full: the 257th carrier was never told, and
# nothing said so. The snapshot is now made for the count the walk found,
# and made again if the list grew in between.
#
# headcount is the instrument: 300 children carry the crew tag from boarding,
# the parent says one word to the tag, and every child answers with a Touch.
run_headcount() {
    echo "== headcount: one word to 300 carriers of a tag, 300 answers =="
    build
    if ! util_boot headcount "STRICT=on CORES=16 MEM=8G" headcount "\[HEADCOUNT\] (PASS|FAIL)"; then
        bad "headcount: never reached a shell"; return
    fi
    L="$SCRATCH/serial.headcount.log"
    grep -q "\[HEADCOUNT\] PASS: one word to 300, 300 answered" "$L" \
        && ok "headcount: every carrier of the tag was told" \
        || bad "headcount: $(grep -m1 '\[HEADCOUNT\] FAIL' "$L" || echo 'no verdict from headcount at all')"
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "headcount: the kernel spoke of a stall or fault" \
        || ok "headcount: no verdict, no panic"
}

headcount_tray_on() {
    mutate src/kernel/core/decks/system/system_ops.c
    python3 - <<'EOF2'
p = "src/kernel/core/decks/system/system_ops.c"
s = open(p).read()
anchor = "            if (pid_count < pid_cap) pid_list[pid_count] = iter->pid;\n"
assert s.count(anchor) == 1, "headcount mutation anchor missing"
s = s.replace(anchor, "            if (pid_count >= 256u) break;   /* logcheck mutation: the tray of 256 */\n" + anchor, 1)
open(p, "w").write(s)
EOF2
    mut_done src/kernel/core/decks/system/system_ops.c "logcheck mutation"
}

headcount_tray_off() {
    restore src/kernel/core/decks/system/system_ops.c
}

run_headcountmut() {
    echo "== headcountmut: the tray of 256 put back, and 44 carriers must go untold =="
    headcount_tray_on; build
    util_boot headcountmut "STRICT=on CORES=16 MEM=8G" headcount "\[HEADCOUNT\] (PASS|FAIL)"; local booted=$?
    headcount_tray_off
    if [ $booted -ne 0 ]; then bad "headcountmut: never reached a shell"; build; return; fi
    L="$SCRATCH/serial.headcountmut.log"
    if grep -q "\[HEADCOUNT\] FAIL: one word to 300, 256 answered" "$L"; then
        ok "headcountmut: the tray left 44 untold — and the oracle sees them"
    else
        bad "headcountmut: the tray put back and all 300 STILL answered — the oracle cannot see that defect ($(grep -m1 '\[HEADCOUNT\]' "$L" || echo 'no verdict'))"
    fi
    build   # leave the tree built from clean sources
}


# ===========================================================================
# byname — a file is found by its name through the index, and the name's tag
# follows a rename
# ===========================================================================
#
# The volume stamps every file with the stem of its name as a tag, and that is
# how by-name lookup (find_file_by_name, boxcxx fopen) and proc_exec find a
# file — the index, not a walk of the first 255 files. rename used to change
# the string and leave the tag, so a renamed file answered to its old name and
# not its new one. The scenario creates a file, renames it, and asks the volume
# by tag and by name; then trashes it and erases the trash, which asks for the
# trashed files by their tag (a plain listing leaves them out on purpose).
byname_session() {
    local name=$1 args=$2
    make run-stop >/dev/null 2>&1
    # shellcheck disable=SC2086
    make run-bg $args >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    if ! grep -q "BoxOS Shell" build/serial.log 2>/dev/null; then
        make run-stop >/dev/null 2>&1
        return 1
    fi
    sleep 3
    local MARK
    MARK=$(wc -l < build/serial.log)
    local cmd
    for cmd in "create alpha.txt" "name alpha.txt beta.txt" "files beta" "files alpha" \
               "info beta.txt" "info alpha.txt" "trash beta.txt" "erase trashed" "files beta"; do
        type_line "$cmd" || break
        wait_for_prompt || break
    done
    sleep 1
    tail -n +$((MARK + 1)) build/serial.log | tr -d '\r' > "$SCRATCH/serial.$name.log"
    make run-stop >/dev/null 2>&1
    return 0
}

byname_judge() {
    local name=$1 L="$SCRATCH/serial.$1.log"
    grep -q "Created file: alpha.txt" "$L" \
        && ok "$name: the file was created" \
        || bad "$name: create did not answer"
    grep -q "File renamed" "$L" \
        && ok "$name: the file was renamed" \
        || bad "$name: rename did not answer"
    # `files beta` asks by tag: the renamed file must carry its new name's tag
    sed -n '/~ files beta/,/~ files alpha/p' "$L" | grep -q "^beta.txt" \
        && ok "$name: asked by its new name's tag, the volume answers with the file" \
        || bad "$name: the tag did not follow the name — 'files beta' does not list beta.txt"
    sed -n '/~ files alpha/,/~ info beta.txt/p' "$L" | grep -q "No files found" \
        && ok "$name: the old name's tag is gone" \
        || bad "$name: the file still answers to its old name's tag"
    sed -n '/~ info beta.txt/,/~ info alpha.txt/p' "$L" | grep -q "beta.txt" \
        && ok "$name: found by its new name" \
        || bad "$name: 'info beta.txt' did not find the file"
    sed -n '/~ info alpha.txt/,/~ trash beta.txt/p' "$L" | grep -q "File not found" \
        && ok "$name: not found by its old name" \
        || bad "$name: 'info alpha.txt' still finds the renamed file"
    grep -q "Deleted 1 trashed files" "$L" \
        && ok "$name: erase found the trashed file by its tag" \
        || bad "$name: 'erase trashed' did not delete the one trashed file ($(grep -m1 'Deleted' "$L" || echo 'no answer'))"
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "$name: the kernel spoke of a stall or fault" \
        || ok "$name: no verdict, no panic"
}

run_byname() {
    echo "== byname: a file is found by its name through the index, and the tag follows a rename =="
    build
    if ! byname_session byname "STRICT=on CORES=4 MEM=4G"; then
        bad "byname: never reached a shell"; return
    fi
    byname_judge byname
}

# The oracle measured against itself: rename leaves the tag where it was, and
# the renamed file must stop answering to its new name.
byname_mut_on() {
    mutate src/kernel/tagfs/tagfs.c
    python3 - <<'EOF2'
p = "src/kernel/tagfs/tagfs.c"
s = open(p).read()
anchor = "    if (old_tag != new_tag)\n    {\n        if (old_tag != TAGFS_INVALID_TAG_ID)\n"
assert s.count(anchor) == 1, "byname mutation anchor missing"
s = s.replace(anchor, "    if (old_tag != new_tag && false)   /* logcheck mutation: the tag stays behind */\n    {\n        if (old_tag != TAGFS_INVALID_TAG_ID)\n", 1)
open(p, "w").write(s)
EOF2
    mut_done src/kernel/tagfs/tagfs.c "logcheck mutation"
}

byname_mut_off() {
    restore src/kernel/tagfs/tagfs.c
}

run_bynamemut() {
    echo "== bynamemut: rename leaves the tag behind, and the oracle must see the file answer to the wrong name =="
    byname_mut_on; build
    byname_session bynamemut "STRICT=on CORES=4 MEM=4G"; local booted=$?
    byname_mut_off
    if [ $booted -ne 0 ]; then bad "bynamemut: never reached a shell"; build; return; fi
    L="$SCRATCH/serial.bynamemut.log"
    if sed -n '/~ files beta/,/~ files alpha/p' "$L" | grep -q "^beta.txt"; then
        bad "bynamemut: the tag was left behind and 'files beta' STILL lists beta.txt — the oracle cannot see that defect"
    else
        ok "bynamemut: the tag left behind, and the oracle sees the file gone from its new name"
    fi
    build   # leave the tree built from clean sources
}

# ===========================================================================
# stash — what a strand is handed that is not for the caller at hand is KEPT
# ===========================================================================
#
# A strand waiting for one Touch tag keeps the events of other tags; a strand
# waiting for a kernel reply keeps the IPC messages ahead of it. Both stashes
# used to hold 256 entries and lose the rest in silence — the Touch one shed
# the newest, the IPC one dropped the oldest. They grow by the chunk now
# (box/core/stash.h). TT 23 publishes 300 events of another tag past a wait,
# TT 24 sends 300 messages past a kernel reply; both must all be there.
run_stash() {
    echo "== stash: 300 events of another tag and 300 messages ahead of a reply, all kept =="
    build
    if ! util_boot stash "STRICT=on CORES=4 MEM=4G" touch_test "\[TT SUMMARY\]"; then
        bad "stash: never reached a shell"; return
    fi
    L="$SCRATCH/serial.stash.log"
    grep -q "\[TT 23\] PASS" "$L" \
        && ok "stash: 300 events of the other tag were kept across the wait" \
        || bad "stash: $(grep -m1 '\[TT 23\]' "$L" || echo 'TT 23 never reported')"
    grep -q "\[TT 24\] PASS" "$L" \
        && ok "stash: 300 messages ahead of a kernel reply were kept" \
        || bad "stash: $(grep -m1 '\[TT 24\]' "$L" || echo 'TT 24 never reported')"
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]|DEFECT" "$L" \
        && bad "stash: the kernel or boxlib spoke of a stall, fault or defect" \
        || ok "stash: no verdict, no panic, no defect"
}

# The oracle measured against itself: the stash of 256 put back — with the
# new rule that an entry with no place stays in the ring, said aloud.
stash_cap_on() {
    mutate src/userspace/boxlib/src/core/stash.c
    python3 - <<'EOF2'
p = "src/userspace/boxlib/src/core/stash.c"
s = open(p).read()
anchor = "    if (s->tail && s->tail->count < s->chunk_cap) return true;\n"
assert s.count(anchor) == 1, "stash mutation anchor missing"
s = s.replace(anchor, "    if (s->count >= 256u) return false;   /* logcheck mutation: the stash of 256 */\n" + anchor, 1)
open(p, "w").write(s)
EOF2
    mut_done src/userspace/boxlib/src/core/stash.c "logcheck mutation"
}

stash_cap_off() {
    restore src/userspace/boxlib/src/core/stash.c
}

run_stashmut() {
    echo "== stashmut: the stash of 256 put back — TT 23 and TT 24 must go red, and the strand must say why =="
    stash_cap_on; build
    util_boot stashmut "STRICT=on CORES=4 MEM=4G" touch_test "\[TT SUMMARY\]"; local booted=$?
    stash_cap_off
    if [ $booted -ne 0 ]; then bad "stashmut: never reached a shell"; build; return; fi
    L="$SCRATCH/serial.stashmut.log"
    if grep -q "\[TT 23\] PASS" "$L"; then
        bad "stashmut: the stash capped at 256 and TT 23 STILL passed — the oracle cannot see that loss"
    else
        ok "stashmut: TT 23 sees the events that had no place"
    fi
    if grep -q "\[TT 24\] PASS" "$L"; then
        bad "stashmut: the stash capped at 256 and TT 24 STILL passed — the oracle cannot see that loss"
    else
        ok "stashmut: TT 24 sees the messages that had no place"
    fi
    grep -q "DEFECT: no memory to keep" "$L" \
        && ok "stashmut: the strand said aloud that it had no place for what it was handed" \
        || bad "stashmut: nothing said about the entries that had no place"
    build   # leave the tree built from clean sources
}

# ===========================================================================
# wire — the serial line reads the log ring by its own interrupt, and the
# last words are drained before a halt
# ===========================================================================
#
# Every byte the kernel says goes into the log ring; the Wire is the line's
# reader, advanced by the UART's transmitter-empty interrupt (IRQ4). No core
# waits on the UART — what stood here spun four million times per byte and
# then dropped it. At panic and halt the line cannot drive itself (interrupts
# are off), so the last words are drained on the transmitter's own word.
# The scenario: a boot must reach the shell on the wire, a command must be
# heard, and `reboot` must leave "[HALT] Rebooting..." on the wire — the
# drain at halt, seen.
wire_session() {
    local name=$1
    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=4 MEM=4G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    local reached=0
    grep -q "BoxOS Shell" build/serial.log 2>/dev/null && reached=1
    sleep 3
    # Typed through the monitor's keyboard, not the serial line, so the wire's
    # own state cannot stop the command from landing.
    type_cmd "hw"
    sleep 1; key_press ret; sleep 4
    type_cmd "reboot"
    sleep 1; key_press ret
    wait_for_line build/serial.log "Rebooting" "$WAIT_QUIET_S"
    sleep 2
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.$name.log"
    return $((1 - reached))
}

run_wire() {
    echo "== wire: the line reads the ring by interrupt; the last words are drained at halt =="
    build
    wire_session wire; local reached=$?
    L="$SCRATCH/serial.wire.log"
    [ $reached -eq 0 ] \
        && ok "wire: the boot reached the shell on the wire" \
        || bad "wire: the shell never appeared on the wire"
    grep -q "TSC freq" "$L" \
        && ok "wire: a command's answer came down the wire" \
        || bad "wire: hw's answer never came down the wire"
    grep -q "\[HALT\] Rebooting" "$L" \
        && ok "wire: the last words before the reboot were drained onto the wire" \
        || bad "wire: '[HALT] Rebooting' never reached the wire"
    grep -q "\[WIRE\]" "$L" \
        && bad "wire: the line reports falling behind on a plain boot" \
        || ok "wire: the line never fell behind the ring"
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "wire: the kernel spoke of a stall or fault" \
        || ok "wire: no verdict, no panic"
}

# The oracle measured against itself: the ring grows and nobody tells the
# line (WireKick does nothing once the line drives itself), so after the
# first THRE has drained what was pending the line goes quiet for good — the
# shell never appears on the wire — and only the drain at halt brings the
# rest out.
#
# ‼ The other half of the mechanism — IRQ4 continuing a BUSY line on THRE —
# cannot be made red on this desk: QEMU's transmitter is empty again by the
# time the next byte is put, so every byte is pumped by its own kick and the
# interrupt is never needed (measured: with the THRE branch cut, the whole
# boot still came down the wire). That half is for the board, where a byte
# takes 87 us and a kick finds the transmitter busy.
wire_deaf_on() {
    mutate src/kernel/drivers/serial/serial.c
    python3 - <<'EOF2'
p = "src/kernel/drivers/serial/serial.c"
s = open(p).read()
anchor = "    __atomic_thread_fence(__ATOMIC_SEQ_CST);\n    if (!__atomic_load_n(&g_wire_idle, __ATOMIC_SEQ_CST)) return;\n"
assert s.count(anchor) == 1, "wire mutation anchor missing"
s = s.replace(anchor, "    return;   /* logcheck mutation: the ring grew and nobody told the line */\n" + anchor, 1)
open(p, "w").write(s)
EOF2
    mut_done src/kernel/drivers/serial/serial.c "logcheck mutation"
}

wire_deaf_off() {
    restore src/kernel/drivers/serial/serial.c
}

run_wiremut() {
    echo "== wiremut: nobody tells the line the ring grew — the shell must NOT reach the wire, and the drain at halt must bring it out =="
    wire_deaf_on; build
    wire_session wiremut; local reached=$?
    wire_deaf_off
    L="$SCRATCH/serial.wiremut.log"
    [ $reached -ne 0 ] \
        && ok "wiremut: with nobody telling the line, the shell never reached the wire — the oracle sees the stall" \
        || bad "wiremut: the shell reached the wire with nobody telling the line — the oracle cannot see a dead line"
    grep -q "BoxOS Shell" "$L" && grep -q "\[HALT\] Rebooting" "$L" \
        && ok "wiremut: the drain at halt brought the whole ring out, shell banner and last words alike" \
        || bad "wiremut: the drain at halt did not bring the ring out"
    build   # leave the tree built from clean sources
}

# ===========================================================================
# knock — an interrupt's event rides the ring and ONE knock; nothing drops
# ===========================================================================
#
# An interrupt cannot publish where it stands, so it leaves the event in the
# Touch IRQ ring and knocks; the drain core comes once per knock and reads
# everything up to the producers' cursor. What stood between the ring and the
# K-Core was a note per event in a ring of notes that could be full — and a
# full ring of notes lost the event in silence, three ways (before its init,
# when its chunk had no successor, after four tries in a race). The knock is
# a baton embedded in the ring itself: nothing to allocate, nowhere to
# overflow, and a burst of interrupts is one pass.
#
# The scenario: a boot on sixteen cores and on one (the idle/tick drain) must
# hear the keyboard — a builtin AND an external program — with the ring never
# saying it lost anything, and the baton self-test must have proven the knock
# at boot (a thousand knocks, one pass; a knock after the open, one more).
run_knock() {
    echo "== knock: a keystroke rides the ring and one knock, on 16 cores and on one =="
    build
    if ! util_boot knock16 "STRICT=on CORES=16 MEM=8G" help "Show available commands" "$REACT_QUIET_S"; then
        bad "knock: never reached a shell (16c)"; return
    fi
    L="$SCRATCH/serial.knock16.log"
    typed_ok "$L" \
        && ok "knock (16c): a builtin answered the keyboard" \
        || bad "knock (16c): 'help' was typed and nothing answered"
    grep -q "\[BATON-TEST\] PASS (never-drop + exactly-once + FIFO + stub-re-anchor + knock)" build/serial.log \
        && ok "knock (16c): the baton self-test proved the knock at boot" \
        || bad "knock (16c): no baton self-test PASS with the knock at boot"
    grep -q "\[TOUCH\] ERROR" "$L" \
        && bad "knock (16c): the ring reports a loss on plain typing" \
        || ok "knock (16c): the ring lost nothing"
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "knock (16c): the kernel spoke of a stall or fault" \
        || ok "knock (16c): no verdict, no panic"

    if ! util_boot knock1 "STRICT=on CORES=1 MEM=2G" hw "TSC freq" "$REACT_QUIET_S"; then
        bad "knock: never reached a shell (1c)"; return
    fi
    L="$SCRATCH/serial.knock1.log"
    grep -q "TSC freq" "$L" \
        && ok "knock (1c): an external program was started from the keyboard through the idle/tick drain" \
        || bad "knock (1c): 'hw' was typed and never ran"
    grep -q "\[TOUCH\] ERROR" "$L" \
        && bad "knock (1c): the ring reports a loss on plain typing" \
        || ok "knock (1c): the ring lost nothing"
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "knock (1c): the kernel spoke of a stall or fault" \
        || ok "knock (1c): no verdict, no panic"
}

# The oracle measured against itself: nobody knocks. The event is in the ring
# and no visit ever comes for it — the machine boots and is deaf.
knock_deaf_on() {
    mutate src/kernel/core/touch/touch.c
    python3 - <<'EOF2'
p = "src/kernel/core/touch/touch.c"
s = open(p).read()
anchor = "    KnockOn(&g_touch_irq_knock);\n}\n"
assert s.count(anchor) == 1, "knock mutation anchor missing"
s = s.replace(anchor, "    /* logcheck mutation: nobody knocks */\n}\n", 1)
open(p, "w").write(s)
EOF2
    mut_done src/kernel/core/touch/touch.c "logcheck mutation"
}

knock_deaf_off() {
    restore src/kernel/core/touch/touch.c
}

run_knockmut() {
    echo "== knockmut: nobody knocks — the machine must boot and be deaf, and the oracle must see it =="
    knock_deaf_on; build
    util_boot knockmut "STRICT=on CORES=4 MEM=4G" help "Show available commands" "$REACT_QUIET_S"; local booted=$?
    knock_deaf_off
    if [ $booted -ne 0 ]; then bad "knockmut: never reached a shell"; build; return; fi
    L="$SCRATCH/serial.knockmut.log"
    if typed_ok "$L"; then
        bad "knockmut: nobody knocked and the keyboard STILL answered — the oracle cannot see a dead ring"
    else
        ok "knockmut: with nobody knocking the keyboard is deaf — and the oracle sees it"
    fi
    build   # leave the tree built from clean sources
}

# ===========================================================================
# knockstorm — more events than the ring, and every one of them delivered;
# then the reader held back, the ring outrun, and the loss counted exactly
# ===========================================================================
#
# One key held for five seconds through the monitor. The kernel's own
# typematic repeat (PIT IRQ0, 33 ms) publishes some hundred and forty events
# — more than two rings — and each one must reach the shell. The shell's echo
# is the listener's account; the kernel's is said at the halt: published,
# delivered, lost, still in the ring. Every event the keyboard published in
# the session is an 'a' the shell echoed, plus the eight keys that ended the
# line and typed `reboot` (Enter, six letters, Enter — the driver publishes
# presses and repeats, never releases), so delivered = echoed + 8 exactly, and
# published = delivered + lost + still. Under the mutation the reader stalls
# three seconds on every visit: the ring is outrun, and the kernel must name
# the loss with a number that the same two equations confirm.
storm_session() {
    # storm_session NAME "RUN-BG ARGS"
    local name=$1 args=$2
    make run-stop >/dev/null 2>&1
    # shellcheck disable=SC2086
    make run-bg $args >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    if ! grep -q "BoxOS Shell" build/serial.log 2>/dev/null; then
        make run-stop >/dev/null 2>&1
        return 1
    fi
    sleep 3
    local MARK
    MARK=$(wc -l < build/serial.log)
    # 5000 is the monitor's own hold time in milliseconds; the seven seconds
    # are that hold plus its delivery, and they are a property of QEMU rather
    # than a guess about the guest.
    mon_raw "sendkey a 5000"
    sleep 7
    # Enter: the line of a's becomes a command nobody knows, and the prompt
    # comes back. Then `reboot`, key by key through the monitor with no echo
    # check — under the mutation the echo lags the keys by design.
    key_press ret
    wait_for_line build/serial.log "Unknown command" "$WAIT_QUIET_S" $((MARK + 1))
    sleep 1
    local k
    for k in r e b o o t ret; do
        key_press "$k"
    done
    wait_for_line build/serial.log "\[HALT\] Rebooting" "$WAIT_QUIET_S"
    sleep 2
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.$name.log"
    echo "$MARK" > "$SCRATCH/serial.$name.mark"
    return 0
}

# Reads the two accounts out of a storm session's log and prints them as
# shell assignments: E (a's the shell echoed on the storm line), P D L S (the
# kernel's account at the halt), K (of those, delivered under the keyboard's
# own name), X (the loss the kernel announced, -1 if it announced none).
# Kernel lines that landed inside the echo are removed before counting.
storm_numbers() {
    python3 - "$1" "$2" <<'EOF2'
import re, sys
log = open(sys.argv[1]).read().replace("\r", "")
mark = int(open(sys.argv[2]).read().strip())
tail = "\n".join(log.split("\n")[mark:])
clean = re.sub(r"\[[A-Z0-9 -]+\][^\n]*\n", "", tail)
i = clean.find("~ a")
E = clean[i:].split("\n")[0].count("a") if i >= 0 else 0
acc = re.search(r"interrupt ring since boot: published (\d+), delivered (\d+), lost (\d+), still in the ring (\d+)", log)
P, D, L, S = (int(x) for x in acc.groups()) if acc else (-1, -1, -1, -1)
err = re.search(r"\[TOUCH\] ERROR: (\d+) interrupt event\(s\) were overwritten", log)
X = int(err.group(1)) if err else -1
kb = re.search(r"delivered by name: keyboard (\d+)", log)
K = int(kb.group(1)) if kb else -1
names = " ".join(m.group(1) for m in re.finditer(r"delivered by name: (\S+ \d+)", log))
print(f"E={E} P={P} D={D} L={L} S={S} X={X} K={K} NAMES='{names}'")
EOF2
}

run_knockstorm() {
    echo "== knockstorm: a key held five seconds — more events than two rings, every one delivered =="
    build
    if ! storm_session knockstorm "STRICT=on CORES=4 MEM=4G"; then
        bad "knockstorm: never reached a shell"; return
    fi
    L="$SCRATCH/serial.knockstorm.log"
    eval "$(storm_numbers "$L" "$SCRATCH/serial.knockstorm.mark")"
    echo "   shell echoed $E a's; kernel: published $P delivered $D lost $L still $S; by name: $NAMES"
    [ "$E" -ge 65 ] \
        && ok "knockstorm: the shell received more events than the ring holds ($E a's)" \
        || bad "knockstorm: only $E a's reached the shell — the storm did not outsize the ring"
    grep -q "\[TOUCH\] ERROR" "$L" \
        && bad "knockstorm: the ring reports a loss under a burst it should carry" \
        || ok "knockstorm: the ring lost nothing"
    [ "$P" -ge 0 ] \
        && ok "knockstorm: the kernel gave its account at the halt" \
        || bad "knockstorm: no ring account at the halt"
    [ "$K" -eq $((E + 8)) ] \
        && ok "knockstorm: keyboard events delivered ($K) = echoed ($E) + the 8 keys after the storm — the kernel's count is the shell's" \
        || bad "knockstorm: keyboard events delivered $K but the shell echoed $E (+8 expected) — the kernel's account and the shell's disagree"
    [ "$P" -eq $((D + L + S)) ] && [ "$L" -eq 0 ] && [ "$S" -eq 0 ] \
        && ok "knockstorm: published = delivered, nothing lost, nothing left in the ring" \
        || bad "knockstorm: published $P, delivered $D, lost $L, still $S — the account does not add up"
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "knockstorm: the kernel spoke of a stall or fault" \
        || ok "knockstorm: no verdict, no panic"
}

# The oracle measured against itself: the reader stalls three seconds on
# every visit. During the first stall the held key outruns the ring; the
# oldest events are overwritten, and the kernel must say how many — a number
# the shell's echo and the halt account confirm.
storm_stall_on() {
    mutate src/kernel/core/touch/touch.c
    python3 - <<'EOF2'
p = "src/kernel/core/touch/touch.c"
s = open(p).read()
anchor = "    KnockOpen(&g_touch_irq_knock);\n\n    for (;;) {\n        uint32_t claimed = __atomic_load_n(&g_touch_irq_claimed, __ATOMIC_ACQUIRE);\n"
assert s.count(anchor) == 1, "storm mutation anchor missing"
stall = ("    KnockOpen(&g_touch_irq_knock);\n"
         "    {   /* logcheck mutation: the reader stalls three seconds on every visit */\n"
         "        uint64_t until = clockboard_uptime_ms() + 3000u;\n"
         "        while (clockboard_uptime_ms() < until) __asm__ volatile(\"pause\");\n"
         "    }\n\n    for (;;) {\n        uint32_t claimed = __atomic_load_n(&g_touch_irq_claimed, __ATOMIC_ACQUIRE);\n")
s = s.replace(anchor, stall, 1)
open(p, "w").write(s)
EOF2
    mut_done src/kernel/core/touch/touch.c "logcheck mutation"
}

storm_stall_off() {
    restore src/kernel/core/touch/touch.c
}

run_knockstormmut() {
    echo "== knockstormmut: the reader stalls, the ring is outrun, and the loss must be named exactly =="
    storm_stall_on; build
    storm_session knockstormmut "STRICT=on CORES=4 MEM=4G"; local booted=$?
    storm_stall_off
    if [ $booted -ne 0 ]; then bad "knockstormmut: never reached a shell"; build; return; fi
    L="$SCRATCH/serial.knockstormmut.log"
    eval "$(storm_numbers "$L" "$SCRATCH/serial.knockstormmut.mark")"
    echo "   shell echoed $E a's; kernel: published $P delivered $D lost $L still $S; announced $X; by name: $NAMES"
    [ "$X" -ge 1 ] \
        && ok "knockstormmut: the kernel said the ring was outrun and named the loss ($X)" \
        || bad "knockstormmut: the ring was outrun and nothing was said"
    [ "$L" -ge 1 ] && [ "$P" -eq $((D + L + S)) ] && [ "$S" -eq 0 ] \
        && ok "knockstormmut: the halt account carries the loss and adds up (published $P = delivered $D + lost $L)" \
        || bad "knockstormmut: the halt account does not add up (published $P, delivered $D, lost $L, still $S)"
    [ "$K" -eq $((E + 8)) ] \
        && ok "knockstormmut: keyboard events delivered ($K) = echoed ($E) + 8 — what the kernel counts as delivered is exactly what the shell received" \
        || bad "knockstormmut: keyboard events delivered $K but the shell echoed $E (+8 expected) — the count of the loss is not true"
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "knockstormmut: the kernel spoke of a stall or fault" \
        || ok "knockstormmut: no verdict, no panic"
    build   # leave the tree built from clean sources
}

# ===========================================================================
# deadline — a timed park's ERR_TIMEOUT rides the process's own baton
# ===========================================================================
#
# The PIT tick used to post the deadline's delivery through irq_defer, which
# drops when its chunk has no successor, and boxlib held a +100 ms clock of its
# own over that hop. The delivery now rides the process's embedded baton — a
# pass that cannot fail for want of a slot — and boxlib waits for the answer
# with no clock at all. strandpark is the instrument: 600 timed parks from
# four strands at once (T4), then the brigade. On one core the drain is the
# idle loop and the tick from ring 3, so that path is boot-tested too.
run_deadline() {
    echo "== deadline: 600 timed parks answered at their deadline, on 16 cores and on one =="
    build
    if ! util_boot deadline16 "STRICT=on CORES=16 MEM=8G" strandpark "\[SPK\] (PASS|FAIL): brigade"; then
        bad "deadline: never reached a shell (16c)"; return
    fi
    L="$SCRATCH/serial.deadline16.log"
    grep -q "\[SPK\] PASS: all 3 workers + main completed 150 concurrent parks" "$L" \
        && ok "deadline (16c): every timed park came back at its deadline" \
        || bad "deadline (16c): $(grep -m1 '\[SPK\] FAIL' "$L" || echo 'strandpark T4 never finished')"
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "deadline (16c): the kernel spoke of a stall or fault" \
        || ok "deadline (16c): no verdict, no panic"

    if ! util_boot deadline1 "STRICT=on CORES=1 MEM=2G" strandpark "\[SPK\] (PASS|FAIL): brigade"; then
        bad "deadline: never reached a shell (1c)"; return
    fi
    L="$SCRATCH/serial.deadline1.log"
    # The self-test speaks at boot, before the shell — that is before the mark
    # util_boot keeps from, so it is read off the whole serial log.
    grep -q "\[BATON-TEST\] PASS" build/serial.log \
        && ok "deadline (1c): the baton self-test passed at boot" \
        || bad "deadline (1c): no baton self-test PASS at boot"
    grep -q "\[SPK\] PASS: all 3 workers + main completed 150 concurrent parks" "$L" \
        && ok "deadline (1c): every timed park came back at its deadline through the idle/tick drain" \
        || bad "deadline (1c): $(grep -m1 '\[SPK\] FAIL' "$L" || echo 'strandpark T4 never finished')"
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "deadline (1c): the kernel spoke of a stall or fault" \
        || ok "deadline (1c): no verdict, no panic"
}

# The oracle measured against itself: the tick drops the pass, so no deadline
# is ever delivered. With no clock left in boxlib the timed park never returns,
# and Nightwatch must name it: the chit fell DUE at the tick and was never kept.
deadline_drop_on() {
    mutate src/kernel/core/touch/touch_queue.c
    python3 - <<'EOF2'
p = "src/kernel/core/touch/touch_queue.c"
s = open(p).read()
anchor = "            BatonPass(&target->deadline_baton);\n"
assert s.count(anchor) == 1, "deadline mutation anchor missing"
s = s.replace(anchor, "            process_ref_dec(target);   /* logcheck mutation: the pass is dropped */\n", 1)
open(p, "w").write(s)
EOF2
    mut_done src/kernel/core/touch/touch_queue.c "logcheck mutation"
}

deadline_drop_off() {
    restore src/kernel/core/touch/touch_queue.c
}

run_deadlinemut() {
    echo "== deadlinemut: the pass dropped, and the kernel must name the answer it owes =="
    deadline_drop_on; build
    util_boot deadlinemut "STRICT=on CORES=16 MEM=8G" strandpark "ANSWER OWED|\[SPK\] (PASS|FAIL): brigade"; local booted=$?
    deadline_drop_off
    if [ $booted -ne 0 ]; then bad "deadlinemut: never reached a shell"; build; return; fi
    L="$SCRATCH/serial.deadlinemut.log"
    if grep -q "ANSWER OWED: system.addr.park took the event" "$L"; then
        ok "deadlinemut: the dropped deadline is named — system.addr.park owes an answer it never delivered"
    else
        bad "deadlinemut: the pass dropped and nobody said so ($(grep -m1 -E 'ANSWER OWED|\[SPK\] (PASS|FAIL)' "$L" || echo 'no verdict, no strandpark line'))"
    fi
    if grep -q "\[SPK\] PASS: all 3 workers + main completed 150 concurrent parks" "$L"; then
        bad "deadlinemut: no deadline delivered and strandpark T4 STILL passed — the oracle cannot see that defect"
    else
        ok "deadlinemut: without the pass no timed park came back — and the oracle sees it"
    fi
    build   # leave the tree built from clean sources
}


# ===========================================================================
# handsetdeaf — a reader that vanished with the handset in its hand
# ===========================================================================
#
# The daemon said each key to the lane on top and never learned whether anyone
# heard: a reader that died in the middle of its reading kept the ear until the
# daemon met its death by another road, and the keys typed meanwhile went to a
# tag nobody wore. touch_send now returns how many heard; nobody drops that ear
# and the key is said again to the next listener.
#
# handset deaf is the instrument: a strand takes the ear and gives up its claim
# (alive, lane open), main reads a moment later. The line typed in between must
# reach main.
handsetdeaf_boot() {
    make run-stop >/dev/null 2>&1
    make run-bg STRICT=on CORES=16 MEM=8G >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    if ! grep -q "BoxOS Shell" build/serial.log 2>/dev/null; then
        make run-stop >/dev/null 2>&1
        return 1
    fi
    sleep 3
    local MARK
    MARK=$(wc -l < build/serial.log)
    type_cmd "handset deaf"; sleep 1
    key_press ret
    wait_for_line build/serial.log "\[HANDSET\] deaf ear on top" "$WAIT_QUIET_S"
    # RAW keys, no witness: a key nobody echoes is exactly the key this case is
    # about, and the typer would retype it once main listens and hide the loss.
    local k
    for k in a b c ret; do
        mon_raw "sendkey $k 30"
        sleep 0.05
    done
    wait_for_line build/serial.log "\[HANDSET\] got:" "$REACT_QUIET_S" $((MARK + 1))
    sleep 2
    tail -n +$((MARK + 1)) build/serial.log | tr -d '\r' > "$SCRATCH/serial.$1.log"
    make run-stop >/dev/null 2>&1
    return 0
}

run_handsetdeaf() {
    echo "== handsetdeaf: the reader on top hears nothing, and the line still reaches the next =="
    build
    if ! handsetdeaf_boot handsetdeaf; then bad "handsetdeaf: never reached a shell"; return; fi
    L="$SCRATCH/serial.handsetdeaf.log"
    grep -q "\[HANDSET\] got: abc" "$L" \
        && ok "handsetdeaf: the line said to a deaf ear reached the reader beneath" \
        || bad "handsetdeaf: $(grep -m1 '\[HANDSET\]' "$L" | tail -1; echo '— the line typed at the deaf ear was lost')"
    grep -qE "VERDICT|PANIC|\[EXCEPTION\]" "$L" \
        && bad "handsetdeaf: the kernel spoke of a stall or fault" \
        || ok "handsetdeaf: no verdict, no panic"
}

# The oracle measured against itself: the daemon takes a key nobody heard for
# heard, so the deaf ear keeps the handset and the line is lost.
handsetdeaf_blind_on() {
    mutate src/userspace/display/display.c
    python3 - <<'EOF2'
p = "src/userspace/display/display.c"
s = open(p).read()
anchor = "        if (heard > 0) {\n"
assert s.count(anchor) == 1, "handsetdeaf mutation anchor missing"
s = s.replace(anchor, "        if (heard >= 0) {   /* logcheck mutation: a key nobody heard counts as heard */\n", 1)
open(p, "w").write(s)
EOF2
    mut_done src/userspace/display/display.c "logcheck mutation"
}

handsetdeaf_blind_off() {
    restore src/userspace/display/display.c
}

run_handsetdeafmut() {
    echo "== handsetdeafmut: the count ignored, and the line at the deaf ear must be lost =="
    handsetdeaf_blind_on; build
    handsetdeaf_boot handsetdeafmut; local booted=$?
    handsetdeaf_blind_off
    if [ $booted -ne 0 ]; then bad "handsetdeafmut: never reached a shell"; build; return; fi
    L="$SCRATCH/serial.handsetdeafmut.log"
    if grep -q "\[HANDSET\] got: abc" "$L"; then
        bad "handsetdeafmut: the count ignored and the line STILL arrived — the oracle cannot see that defect"
    else
        ok "handsetdeafmut: with the count ignored the deaf ear kept the line — and the oracle sees it"
    fi
    build   # leave the tree built from clean sources
}


# ── unattended: a read nobody stands over, proved at boot ─────────────────────
#
# The proof used to be called [USB ASYNC TEST], and on this stand it never
# touched USB: the volume sits on AHCI under UEFI (q35) and on the legacy
# channel under BIOS. On AHCI it was red on every boot — the completion is
# delivered by an interrupt the BSP has not opened yet, and the proof turned
# only the USB handle — and under BIOS it said nothing at all. Measured with a
# probe: PxCI clear, PxIS.DHRS set, the MSI pending in the LAPIC's IRR, and the
# callback landing 259 ms after the proof had given up and returned, at the
# address of a stack frame that no longer existed.
#
# Now it turns the seat's own handle, has no clock of its own, and speaks on
# every boot — saying where it was not asked, and why.
unattended_boot() {   # $1 = tag, the rest = run-bg arguments
    local tag=$1; shift
    make run-stop >/dev/null 2>&1
    make run-bg "$@" >/dev/null 2>&1
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    sleep 3
    type_cmd "hw"
    sleep 1
    key_press ret
    sleep 4
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.$tag.log"
}

run_unattended() {
    echo "== unattended: the volume's seat answers a read nobody stands over, at boot =="
    build
    unattended_boot unattended UEFI=on STRICT=on CORES=4 MEM=4G
    L="$SCRATCH/serial.unattended.log"
    grep -q "BoxOS Shell" "$L"; chk $? "unattended: UEFI 4c reaches the shell"
    grep -qE "\[UNATTENDED READ\] seat [0-9]+ \(AHCI port [0-9]+\): PASSED — 4096 bytes read" "$L"
    chk $? "unattended: the AHCI seat under the volume answered, and the bytes agree"
    ! grep -q "\[UNATTENDED READ\].*FAILED" "$L"; chk $? "unattended: and nothing about it failed"
    # Judged during boot, before the machine was running — not by a late answer.
    local judged finished
    judged=$(grep -n "\[UNATTENDED READ\]" "$L" | head -1 | cut -d: -f1)
    finished=$(grep -n "Kernel initialization complete" "$L" | head -1 | cut -d: -f1)
    [ -n "$judged" ] && [ -n "$finished" ] && [ "$judged" -lt "$finished" ]
    chk $? "unattended: judged before the end of boot (line ${judged:-?} < ${finished:-?})"
    ! grep -q "went past its deadline" "$L"; chk $? "unattended: no port was called overdue"
    external_ok "$L"; chk $? "unattended: and the shell runs a program afterwards"

    # Where the machine will never read this way it says so, and asks nothing.
    unattended_boot unattended1c UEFI=on STRICT=on CORES=1 MEM=2G
    L="$SCRATCH/serial.unattended1c.log"
    grep -q "\[UNATTENDED READ\] not asked: with one core every read is attended" "$L"
    chk $? "unattended: one core — not asked, and said"
    unattended_boot unattendedbios STRICT=on CORES=4 MEM=4G
    L="$SCRATCH/serial.unattendedbios.log"
    grep -qE "\[UNATTENDED READ\] not asked: seat [0-9]+ \(ATA [a-z ]+\) answers only a read somebody stands over" "$L"
    chk $? "unattended: the legacy channel — not asked, and said"
}

# The disk's answer withheld: Tier 1 of the AHCI watch — the level the proof
# brings a completion in by — is switched off, so nothing can bring it in
# before the machine is running. The proof must then be ended by the disk's
# own patience (Tier 2, CONFIG_AHCI_IO_TIMEOUT_MS): the port is called overdue,
# the slot is failed and given back, the link is reset, the proof says the
# medium refused — and the machine boots. No clock of the proof's own is
# involved, which is what the deadline line measures.
unattended_silence_on() {
    mutate src/kernel/drivers/disk/ahci.c
    python3 - <<'EOF'
p = "src/kernel/drivers/disk/ahci.c"
s = open(p).read()
anchor = """        if (!port_error) {
            uint32_t outstanding = state->ncq"""
assert anchor in s, "unattended silence anchor missing"
mut = """        if (!port_error && 0) {   /* logcheck mutation: the disk's answer is never brought in by its level */
            uint32_t outstanding = state->ncq"""
open(p, "w").write(s.replace(anchor, mut, 1))
EOF
    mut_done src/kernel/drivers/disk/ahci.c "logcheck mutation: the disk's answer"
}
unattended_silence_off() {
    restore src/kernel/drivers/disk/ahci.c
}

# The wrong block: the unattended read asks for the volume's first sector
# instead of the one the attended read fetched. The bytes must disagree, and
# the proof must say at which byte.
unattended_wrongblock_on() {
    mutate src/kernel/core/boardroom/boardroom.c
    python3 - <<'EOF'
p = "src/kernel/core/boardroom/boardroom.c"
s = open(p).read()
anchor = """    error_t sub = BoardroomReadAsync(seat, 0, sectors, dma,"""
assert anchor in s, "unattended wrong-block anchor missing"
mut = """    error_t sub = BoardroomReadAsync(seat, 2048, sectors, dma,   /* logcheck mutation: a different block than the one compared against */"""
open(p, "w").write(s.replace(anchor, mut, 1))
EOF
    mut_done src/kernel/core/boardroom/boardroom.c "logcheck mutation: a different block"
}
unattended_wrongblock_off() {
    restore src/kernel/core/boardroom/boardroom.c
}

run_unattendedmut() {
    echo "== unattendedmut: withhold the disk's answer, then ask for the wrong block =="
    unattended_silence_on; build
    # The wait is the disk's own patience, thirty seconds; the boot waits it out.
    unattended_boot unattendedmut UEFI=on STRICT=on CORES=4 MEM=4G
    unattended_silence_off
    L="$SCRATCH/serial.unattendedmut.log"
    grep -qE "\[AHCI\] port [0-9]+ went past its deadline — failing slots 0x[0-9a-f]+ and resetting the link" "$L"
    chk $? "unattendedmut: the disk's own watch called the port overdue and gave the slot back"
    grep -qE "\[UNATTENDED READ\] seat [0-9]+ \(AHCI port [0-9]+\): FAILED — the medium refused the read \(error -?[0-9]+\)" "$L"
    chk $? "unattendedmut: and the proof said the medium refused"
    ! grep -q "\[UNATTENDED READ\].*PASSED" "$L"; chk $? "unattendedmut: no PASSED was said"
    grep -q "BoxOS Shell" "$L"; chk $? "unattendedmut: the machine still boots"
    external_ok "$L"; chk $? "unattendedmut: and runs a program afterwards"
    ! grep -qE "PANIC|^\[EXCEPTION\]" "$L"; chk $? "unattendedmut: no panic, no exception"

    unattended_wrongblock_on; build
    unattended_boot unattendedwrong UEFI=on STRICT=on CORES=4 MEM=4G
    unattended_wrongblock_off
    L="$SCRATCH/serial.unattendedwrong.log"
    grep -qE "\[UNATTENDED READ\] seat [0-9]+ \(AHCI port [0-9]+\): FAILED — the two reads disagree, first at byte [0-9]+" "$L"
    chk $? "unattendedwrong: the bytes disagreed and the proof said where"
    grep -q "BoxOS Shell" "$L"; chk $? "unattendedwrong: the machine still boots"
}

case "${1:-both}" in
    healthy)  run_healthy ;;
    novolume) run_novolume ;;
    badpool)  run_badpool ;;
    uefi)     run_uefi ;;
    stranger) run_stranger ;;
    latearrival) run_latearrival ;;
    replug)   run_replug ;;
    nofsgsbase) run_nofsgsbase ;;
    logsave)  run_logsave ;;
    mountfail) run_mountfail ;;
    yank)     run_yank ;;
    slowdisk) run_slowdisk ;;
    gpt)      run_gpt ;;
    stillthere) run_stillthere ;;
    sleeps)     run_sleeps ;;
    lines)      run_lines ;;
    linesmut)   run_linesmut ;;
    rollcall)   run_rollcall ;;
    rollcallmut) run_rollcallmut ;;
    handset)    run_handset ;;
    handsetmut) run_handsetmut ;;
    handsetdeaf) run_handsetdeaf ;;
    handsetdeafmut) run_handsetdeafmut ;;
    brigade)    run_brigade ;;
    brigademut) run_brigademut ;;
    headcount)  run_headcount ;;
    headcountmut) run_headcountmut ;;
    deadline)   run_deadline ;;
    deadlinemut) run_deadlinemut ;;
    byname)     run_byname ;;
    bynamemut)  run_bynamemut ;;
    stash)      run_stash ;;
    stashmut)   run_stashmut ;;
    wire)       run_wire ;;
    wiremut)    run_wiremut ;;
    knock)      run_knock ;;
    knockmut)   run_knockmut ;;
    knockstorm) run_knockstorm ;;
    knockstormmut) run_knockstormmut ;;
    unattended) run_unattended ;;
    unattendedmut) run_unattendedmut ;;
    sleepsmut)  run_sleepsmut ;;
    kcoreclaim) run_kcoreclaim ;;
    kcoreclaimmut) run_kcoreclaimmut ;;
    lostwake)   run_lostwake ;;
    lostwakemut) run_lostwakemut ;;
    chit)       run_chit ;;
    chitmut)    run_chitmut ;;
    chitmissmut) run_chitmissmut ;;
    turnin)     run_turnin ;;
    turninmut)  run_turninmut ;;
    twoctrl)  run_twoctrl ;;
    manyports) run_manyports ;;
    usbrecover) run_usbrecover ;;
    holdground) run_holdground ;;
    returnfail) run_returnfail ;;
    seal)     run_seal ;;
    ctrlgiveup) run_ctrlgiveup ;;
    isoch)    run_isoch ;;
    noexec)   run_noexec ;;
    earlyirq) run_earlyirq ;;
    lastsaid) run_lastsaid ;;
    draft)    run_draft ;;
    draftmut) run_draftmut ;;
    paint)    run_paint ;;
    paintmut) run_paintmut ;;
    finish)   run_finish ;;
    finishmut) run_finishmut ;;
    overtake) run_overtake ;;
    overtakemut) run_overtakemut ;;
    orderlife) run_orderlife ;;
    orderlifemut) run_orderlifemut ;;
    batchfail) run_batchfail ;;
    batchfailmut) run_batchfailmut ;;
    caret)    run_caret ;;
    caretmut) run_caretmut ;;
    grow)     run_grow ;;
    growcut)  run_growcut ;;
    growmut)  run_growmut ;;
    both)     run_healthy; echo; run_novolume ;;
    all)      run_healthy; echo; run_novolume; echo; run_stranger; echo; run_latearrival; echo; run_replug; echo; run_holdground; echo; run_returnfail; echo; run_nofsgsbase; echo; run_logsave; echo; run_yank; echo; run_stillthere; echo; run_sleeps; echo; run_lines; echo; run_rollcall; echo; run_handset; echo; run_handsetdeaf; echo; run_brigade; echo; run_headcount; echo; run_deadline; echo; run_byname; echo; run_stash; echo; run_wire; echo; run_knock; echo; run_knockstorm; echo; run_unattended; echo; run_kcoreclaim; echo; run_lostwake; echo; run_chit; echo; run_turnin; echo; run_slowdisk; echo; run_gpt; echo; run_twoctrl; echo; run_manyports; echo; run_usbrecover; echo; run_ctrlgiveup; echo; run_isoch; echo; run_seal; echo; run_uefi; echo; run_noexec; echo; run_earlyirq; echo; run_lastsaid; echo; run_draft; echo; run_paint; echo; run_finish; echo; run_overtake; echo; run_orderlife; echo; run_batchfail; echo; run_caret; echo; run_grow; echo; run_growcut; echo; run_mountfail; echo; run_badpool ;;
    *) echo "usage: $0 [healthy|novolume|uefi|noexec|earlyirq|lastsaid|draft|draftmut|paint|paintmut|finish|finishmut|overtake|overtakemut|orderlife|orderlifemut|batchfail|batchfailmut|caret|caretmut|grow|growcut|growmut|mountfail|holdground|returnfail|stranger|latearrival|replug|nofsgsbase|badpool|manyports|usbrecover|stillthere|sleeps|sleepsmut|lines|linesmut|rollcall|rollcallmut|knock|knockmut|knockstorm|knockstormmut|unattended|unattendedmut|handset|handsetmut|kcoreclaim|kcoreclaimmut|lostwake|lostwakemut|turnin|turninmut|slowdisk|gpt|seal|ctrlgiveup|isoch|both|all]"; exit 2 ;;
esac

LOGCHECK_FINISHED=1
echo
echo "logcheck: $PASS passed, $FAIL failed"
if [ "$HARNESS" != 0 ]; then
    echo "logcheck: $HARNESS harness failure(s) — this run could not talk to the"
    echo "          machine it was judging, so its verdicts are not evidence."
fi
[ "$FAIL" = 0 ] && [ "$HARNESS" = 0 ]
