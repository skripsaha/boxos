# shellcheck shell=bash
# logcheck_book_logger.sh — a logger of one line a second on a format-v2 book,
# for tools/logcheck.sh (loggerhour, loggerhourmut): what the host writes per
# cycle, how often cycles and Tallies come (spec_v2 §10.4 step 2.4, §12.1).
#
# SOURCED, NEVER RUN — by logcheck.sh, after logcheck_book.sh, whose judge of a
# book's boot (census_book_judge) it uses; the boots, the waits and the guest's
# hands are logcheck.sh's and stand.sh's. The oracles are tools/loggerhour.py's.
#
#     LOGGER_SECONDS=3600 tools/logcheck.sh loggerhour    the hour (about 65 minutes)
#     LOGGER_SECONDS=120  tools/logcheck.sh loggerhour    a short run to debug with
#     tools/gates.sh loggerhour                           the gate; not in the default
#                                                         list, it runs on its own
#     tools/logcheck.sh loggerhourmut                     the three mutants (below)
#
#     LOGGER_ANCHOR_EVERY=N   an anchor after every N-th line (default a sixth of
#                             the run, so the short run has anchors too)
#
# ---------------------------------------------------------------------------
# loggerhour
#
# A plain image of format v2 is booted on the BIOS machine, its disk recorded
# (POWERCUT=on, blklogwrites) and in the guest `quilltest logger loggerh N E`
# appends one line of 80 bytes a second for N seconds by the guest's clock,
# with an anchor after every E-th line and a last one; then the machine stops
# (`bye`). Four things are held, each a line of the verdict (loggerhour.py):
#
#   writes    every write of the record is a stitch of one atom, a Tally's node
#             or slot, or a block of the logger's data — among them, one a Tally
#             at the most, a tail the Helm's service laid (Engross) and the
#             logger's next line laid again before a cycle named it, as the
#             kernel reads the writer to be the Helm's holder, which a program
#             is for one write (loggerhour.py TailsLaidAgain, held to its cases
#             by loggerhour_selftest.py); a cycle of the logger costs the host
#             exactly one atom, and its lines are not carried to blocks a line
#             at a time (Slip) — only as the tail fills, or for a Tally that
#             owes an Engross
#   cycles    no more than one a window of five seconds, but those an anchor,
#             the file's making and the halt ask for at once — and no fewer than
#             the window's promise gives; every anchor answered inside half a
#             window
#   Tallies   as many as TALLY_SLACK owes by the useful bytes of the stitches
#             (tools/volumeread/binder.py Useful, the rule of settle/binder.c),
#             each at the stitch where the bytes reach its cost
#   halt      volume-read finds the book consistent, its census is the kernel's
#             at the next mount, and every line is in the file
#
# loggerhourmut: each oracle shown the defect it waits for, LOGGERMUT_SECONDS
# (120) of logger on a kernel with one edit — the window of five seconds taken
# back to a Lull (ink.c: a cycle a line), the Slip taken away (plot_slip.c: a
# block a line), a Tally owed by a stitch's atom and not by its useful bytes
# (sewing.c) — and the oracle named must turn.
# ---------------------------------------------------------------------------

LOGGERHOUR_SECONDS=${LOGGER_SECONDS:-3600}
LOGGERHOUR_ANCHOR_EVERY=${LOGGER_ANCHOR_EVERY:-$(( ${LOGGER_SECONDS:-3600} / 6 ))}
LOGGERMUT_SECONDS=${LOGGERMUT_SECONDS:-120}
LOGGERHOUR_SAID='\[QUILL\] logger: PASS — [^ ]+ \(fid [0-9]+\): [0-9]+ lines in [0-9]+ ms'
# What the logger says of its progress every QUILL_LOGGER_SAY (30) lines
# (quilltest_book.c): its wait is the stand's ordinary one, the quiet window a
# logger gone quiet and the runaway guard counted from the newest line said.
# Held to the guard from the wait's start, the hour stopped at 900 s.
LOGGERHOUR_PROGRESS='^\[QUILL\] logger: [0-9]+ of [0-9]+ lines appended'

# ‼ A RUN THE HARNESS ASKED TO BE LONG IS WAITED OUT BY ITS OWN PROGRESS.
# progress_case plays the machine on the host — a line a second for ten
# seconds, the first SAID of them the logger's progress and the rest a
# kernel's line, then the verdict — and holds what wait_for_line answers with
# ARGS before its file and the runaway guard three seconds long: the run
# waited out by -P while its progress keeps coming, given up past the guard
# by the startup tests it never said, and by -P once its progress stops,
# however the log grows.
progress_case() {
    local name=$1 want=$2 said=$3 f="$SCRATCH/progress.log" w got
    shift 3
    : > "$f"
    ( for i in 1 2 3 4 5 6 7 8 9 10; do
          sleep 1
          if [ "$i" -le "$said" ]; then echo "[QUILL] logger: $((i * 30)) of 300 lines appended, 0 anchor(s)"
          else echo "[TagFS] Tally $i is written over slot 1: the state after stitch $((i * 50))"; fi >> "$f"
      done
      echo "[QUILL] logger: PASS — progress-g.log (fid 81): 300 lines in 10000 ms" >> "$f" ) &
    w=$!
    got=$(WAIT_RUNAWAY_S=3 wait_for_line -E "$@" "$f" '\[QUILL\] logger: (PASS|FAIL)' 5 >/dev/null; echo $?)
    kill "$w" 2>/dev/null; wait "$w" 2>/dev/null
    [ "$got" = "$want" ]
    chk $? "progress: $name — wait_for_line answers $want (answered: ${got:-nothing})"
}

progress_selfcheck() {
    progress_case "the logger's progress said past the guard, counted by -P" 0 10 -P "$LOGGERHOUR_PROGRESS"
    progress_case "the same run held to the startup tests, none said" 1 10
    progress_case "its progress stopped, the kernel still talking" 1 2 -P "$LOGGERHOUR_PROGRESS"
}

# loggerhour_selftest — the judge's rule for a block of free ground the
# logger's, held to the cases loggerhour_selftest.py makes up: a self-test
# that cannot answer is one that failed.
loggerhour_selftest() {
    local line
    python3 tools/loggerhour_selftest.py >"$SCRATCH/loggerhour.selftest" 2>&1
    [ $? = 0 ] || grep -q '^bad ' "$SCRATCH/loggerhour.selftest" || bad "loggerhour, the judge's self-test did not answer"
    while IFS= read -r line; do
        case "$line" in
            "ok "*)  ok  "loggerhour: ${line#ok }" ;;
            "bad "*) bad "loggerhour: ${line#bad }" ;;
            *)       bad "loggerhour, the judge's self-test said: $line" ;;
        esac
    done < "$SCRATCH/loggerhour.selftest"
}

# loggerhour_record TAG SECONDS EVERY — the book's image booted on the BIOS
# machine with its disk recorded, the logger run and the machine stopped:
# $SCRATCH/TAG.base.img, .img, .record and serial.TAG_write.log. 0 once the
# machine stopped itself.
loggerhour_record() {
    local tag=$1 seconds=$2 every=$3 rc
    local img="$SCRATCH/$tag.img" base="$SCRATCH/$tag.base.img" record="$SCRATCH/$tag.record"
    rm -f "$img" "$base" "$record" "$SCRATCH/serial.${tag}_write.log"
    cp build/boxos.img "$img"
    cp -c "$img" "$base" 2>/dev/null || cp "$img" "$base"
    make run-stop >/dev/null 2>&1
    if ! make run-bg BOOTDISK="$img" POWERCUT=on POWERCUT_LOG="$record" >"$SCRATCH/$tag.run-bg" 2>&1; then
        harness "$tag: run-bg would not start the book's image: $(tail -1 "$SCRATCH/$tag.run-bg")"
        return 1
    fi
    wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S" && wait_for_prompt &&
        type_line "quilltest logger loggerh $seconds $every" &&
        wait_for_line -E -P "$LOGGERHOUR_PROGRESS" build/serial.log '\[QUILL\] logger: (PASS|FAIL)' &&
        wait_for_prompt && type_line "bye" && wait_for_line -E build/serial.log "$STAND_HALT_ENDS" "$WAIT_QUIET_S"
    rc=$?
    sleep 1
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.${tag}_write.log"
    return $rc
}

book_loggerhour() {
    local name=loggerhour seconds=$LOGGERHOUR_SECONDS every=$LOGGERHOUR_ANCHOR_EVERY
    local img="$SCRATCH/$name.img" w="$SCRATCH/serial.${name}_write.log" rc line
    echo "== loggerhour: a logger of a line a second, $seconds s, anchored every $every lines, on a book's record =="
    prompt_selfcheck
    progress_selfcheck
    loggerhour_selftest
    rm -f build/boxos.img "$SCRATCH/serial.$name.log"
    if ! make >"$SCRATCH/build.log" 2>&1; then
        bad "loggerhour: make does not build: $(grep -aE 'ERROR|error' "$SCRATCH/build.log" | tail -1)"
        return
    fi
    loggerhour_record "$name" "$seconds" "$every"
    rc=$?
    grep -aqE "$LOGGERHOUR_SAID" "$w"
    chk $? "loggerhour: the guest appends a line a second for $seconds s, anchors every $every lines, and reads every byte back ($(grep -aoE '\[QUILL\] logger: [A-Z]+ — [^,]*, [^,]*' "$w" | head -1))"
    [ $rc = 0 ] && grep -aq '^\[HALT\] Storage sync complete' "$w"
    chk $? "loggerhour: the machine stops, the halt syncing the volume ($(grep -aE '^\[HALT\] (Storage sync complete|.*NOT MADE DURABLE|.*DID NOT WRITE)' "$w" | head -1 | cut -c1-100))"
    python3 tools/loggerhour.py "$SCRATCH/$name.base.img" "$SCRATCH/$name.record" "$img" "$w" >"$SCRATCH/$name.verdict" 2>&1
    while IFS= read -r line; do
        case "$line" in
            "ok "*)  ok  "loggerhour: ${line#ok }" ;;
            "bad "*) bad "loggerhour: ${line#bad }" ;;
            *)       harness "loggerhour: the judge said: $line" ;;
        esac
    done < "$SCRATCH/$name.verdict"
    python3 tools/volume-read.py "$img" check --json >"$SCRATCH/census.$name.json" 2>"$SCRATCH/$name.check.err"
    chk $? "loggerhour: volume-read finds the book consistent ($(python3 -c 'import json, sys
print(json.load(open(sys.argv[1])).get("verdict", "no verdict"))' "$SCRATCH/census.$name.json" 2>/dev/null))"
    if ! make run-bg BOOTDISK="$img" >"$SCRATCH/$name.again.run-bg" 2>&1; then
        harness "loggerhour: run-bg would not start the written book again: $(tail -1 "$SCRATCH/$name.again.run-bg")"
        return
    fi
    wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S"
    rc=$?
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.$name.log"
    [ $rc = 0 ] || bad "loggerhour: the written book never reached a shell"
    census_book_judge "$name"
    while IFS= read -r line; do
        case "$line" in
            "ok "*)  ok  "loggerhour, the next mount: ${line#ok }" ;;
            "bad "*) bad "loggerhour, the next mount: ${line#bad }" ;;
        esac
    done < "$SCRATCH/census.$name.verdict"
    ! grep -aqE 'KERNEL PANIC|\[EXCEPTION\]' "$w" "$SCRATCH/serial.$name.log"
    chk $? "loggerhour: no panic, no fault"
    [ "$FAIL" != 0 ] || rm -f "$img" "$SCRATCH/$name.base.img" "$SCRATCH/$name.record"
}

run_loggerhour() {
    book_loggerhour
    buildkey_restore
}

# loggerhourmut_edit FILE OLD NEW — one edit under the manifest, OLD found
# once in FILE and NEW carrying the words that say it is a mutation.
loggerhourmut_edit() {
    mutate "$1"
    python3 - "$1" "$2" "$3" <<'MUT_EOF'
import sys
path, old, new = sys.argv[1:4]
text = open(path).read()
assert text.count(old) == 1, "loggerhourmut: %s carries %d copies of the anchor" % (path, text.count(old))
open(path, "w").write(text.replace(old, new, 1))
MUT_EOF
    mut_done "$1" "logcheck mutation" 1
}

# loggerhourmut_leg FILE OLD NEW PATTERN — the kernel with the edit, the
# logger run on it, and the oracle PATTERN names (a line the judge says is
# `bad`) turned.
loggerhourmut_leg() {
    local file=$1 pattern=$4 w="$SCRATCH/serial.loggerhourmut_write.log" turned
    loggerhourmut_edit "$file" "$2" "$3"
    if ! make >"$SCRATCH/build.log" 2>&1; then
        restore "$file"
        bad "loggerhourmut: the kernel with the edit in $file does not build: $(grep -aE 'ERROR|error' "$SCRATCH/build.log" | tail -1)"
        return
    fi
    loggerhour_record loggerhourmut "$LOGGERMUT_SECONDS" "$((LOGGERMUT_SECONDS / 6))"
    restore "$file"
    if ! grep -aqE "$LOGGERHOUR_SAID" "$w"; then
        harness "loggerhourmut: the logger did not finish on the kernel with the edit in $file"
        return
    fi
    python3 tools/loggerhour.py "$SCRATCH/loggerhourmut.base.img" "$SCRATCH/loggerhourmut.record" \
        "$SCRATCH/loggerhourmut.img" "$w" >"$SCRATCH/loggerhourmut.verdict" 2>&1
    turned=$(grep -E "^bad .*$pattern" "$SCRATCH/loggerhourmut.verdict" | head -1)
    [ -n "$turned" ]
    chk $? "loggerhourmut: the edit in $file turns the oracle on '$pattern' (${turned:-it held: $(grep -E "^(ok|bad) .*$pattern" "$SCRATCH/loggerhourmut.verdict" | head -1 | cut -c1-200)})"
    cp "$SCRATCH/loggerhourmut.verdict" "$SCRATCH/loggerhourmut.$(basename "$file" .c).verdict"
}

run_loggerhourmut() {
    echo "== loggerhourmut: the window, the Slip and the useful bytes taken away, each oracle must turn =="
    progress_selfcheck
    rm -f build/boxos.img
    loggerhourmut_leg src/kernel/tagfs/ink/ink.c \
        '(p->book ? SETTLE_WINDOW_US : SettleLullUs());' \
        'SettleLullUs(); /* logcheck mutation: a Lull, no window of five seconds */' \
        'no cycle follows another by less than a window'
    loggerhourmut_leg src/kernel/tagfs/plot/plot_slip.c \
        'return !p->rec ||' \
        'return 1 || !p->rec || /* logcheck mutation: no Slip */' \
        'the lines ride the stitches'
    loggerhourmut_leg src/kernel/tagfs/settle/sewing.c \
        'BinderStitchSettled(s->seq, s->useful, fid_most, births);' \
        'BinderStitchSettled(s->seq, 4096u /* logcheck mutation: an atom, not the useful bytes */, fid_most, births);' \
        'falls owed where the useful bytes reach'
    rm -f "$SCRATCH"/loggerhourmut.img "$SCRATCH"/loggerhourmut.base.img "$SCRATCH"/loggerhourmut.record
    buildkey_restore
}
