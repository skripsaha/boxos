# powercut_mut — the *mut scenarios of tools/powercut.sh: a fix taken out or
# a fault put in on purpose, and the one verdict each must answer.
#
# SOURCED, NEVER RUN — by powercut.sh; everything it calls (pc_begin,
# pc_build, the scenarios, pc_expect, mutate, restore and the verdicts) is
# powercut.sh's or stand.sh's. Its own file so powercut.sh stays under a
# thousand lines as the scenarios grow.

# The *mut scenarios. Each puts one fix or one fault into the tree through
# stand.sh's mutation mechanism (restored by the exit trap on every way out),
# runs its scenario (on the plain stand unless it names a machine) and asks
# one verdict — and says every harness line the judge gave, because a verdict
# over cuts volume-read could not judge is not one.
mut_on() {   # mut_on FILE MARKER — FILE's edit (python on stdin) under the manifest
    mutate "$1"
    python3 - "$1" || { echo "powercut: the mutation of $1 did not apply"; exit 1; }
    mut_done "$1" "$2"
}

mut_run() {  # mut_run NAME BASE KEY SEEN WHY [KEYS...] — the mutated tree through BASE's scenario
    local key kind name seen what
    pc_begin "$1" "${@:6}"
    pc_build 4 || return
    if ! "pc_$2"; then
        bad "$1: the scenario stopped before its record could be judged (logs in $PC_DIR)"; return
    fi
    while IFS="$TAB" read -r key kind name seen what; do
        [ "$kind" = harness ] && harness "$1 $key: $what"
    done <"$PC_DIR/verdicts.tsv"
    pc_expect "$3" "$4" "$5"
}

# Since step 1.7 nothing waits for an anchor to settle a write: the cadence
# begins a cycle of its own (settle.h — the window, Lull, and a demand such as
# a program leaving with changes unsettled). So noanchor's fix is in, and its
# mutant takes the cadence out: the committer begins a cycle only for a waiter,
# as the Settle did under step 1.4 — and persist's write, which nobody waits
# for, must be gone after the power.
run_noanchormut() {
    local PC_LIES=off
    mut_on src/kernel/tagfs/settle/cadence.c "logcheck mutation" <<'EOF'
import sys
p = sys.argv[1]; s = open(p).read()
a = "    if (waiters || demand)\n        return CADENCE_NOW;\n"
assert s.count(a) == 1
open(p, "w").write(s.replace(a, "    if (waiters)\n        return CADENCE_NOW;\n"
                                "    return CADENCE_REST;   /* logcheck mutation: the cadence is off, only a waiter starts a cycle */\n"))
EOF
    mut_run noanchormut noanchor verify defect \
        "the cadence off, nothing settles persist's write before the power goes"
    restore src/kernel/tagfs/settle/cadence.c
}

# The book's own mutants: each puts one fault into the book's path
# (settle/sewing.c, mount/book_mount.c) and asks the verdict that must say so.
# None asks for the lie's media: one verdict of the power's is the question.

# The flush of a stitch's data (FIRST_FLUSH, §3.3) taken out: the stitch still
# says DATA_FLUSHED, and the record shows its data behind no flush its first
# atom is not. Asked of overwrite, whose first round is the first cycle of the
# boot and names fresh data: D is nought until a cycle of the mount settles
# (cadence.c CadenceMountReset), so its toll is paid by the rule, whatever the
# medium's pace (StitchToll) — not of halt, whose CLEAN stitch always pays but
# on a book names no fresh data: the churn's 16 bytes ride it as a Slip.
run_firstflushmut() {
    local PC_LIES=off
    mut_on src/kernel/tagfs/settle/sewing.c "logcheck mutation" <<'MUT_EOF'
import sys
p = sys.argv[1]; s = open(p).read()
a = "        rc           = VolumeFlushFor(vol, s->data_need);\n"
assert s.count(a) == 1
open(p, "w").write(s.replace(a, "        rc           = OK;   /* logcheck mutation: the stitch's data is never flushed before it */\n"))
MUT_EOF
    mut_run firstflushmut overwrite order fail \
        "the flush of a stitch's data taken out, a stitch that says DATA_FLUSHED stands before its data's flush"
    restore src/kernel/tagfs/settle/sewing.c
}

# The cycle's one kept flush (LAST_FLUSH, S5) taken out: the anchor's ack
# stands behind no flush after its stitch.
run_lastflushmut() {
    local PC_LIES=off
    mut_on src/kernel/tagfs/settle/sewing.c "logcheck mutation" <<'MUT_EOF'
import sys
p = sys.argv[1]; s = open(p).read()
a = "        rc       = VolumeFlushFor(vol, VolumeFlushNeed(vol));\n"
assert s.count(a) == 1
open(p, "w").write(s.replace(a, "        rc       = OK;   /* logcheck mutation: the stitch's last flush is never asked */\n"))
MUT_EOF
    mut_run lastflushmut nextfid ack:anchor_test fail "the flush after the stitch taken out, the anchor's ack has none behind it"
    restore src/kernel/tagfs/settle/sewing.c
}

# The next file id derived from the ribbon past the Tally (§4.5 п.8) taken
# out: the slot's count and the Tally's leaves stand, and a cut whose ribbon
# bore the anchor's file hands its id out again — to the file its boot is
# asked to make (pc_nextfid: persist write; anchor_test finds its own file on
# that ribbon and makes none).
run_booknextfidmut() {
    local PC_LIES=off
    mut_on src/kernel/tagfs/mount/book_mount.c "logcheck mutation" <<'MUT_EOF'
import sys
p = sys.argv[1]; s = open(p).read()
a = "    if (w->seen_fid > vol->book.next_fid)\n"
assert s.count(a) == 1
open(p, "w").write(s.replace(a, "    if (0 && w->seen_fid > vol->book.next_fid)   /* logcheck mutation: the ribbon's fids are not counted */\n"))
MUT_EOF
    mut_run booknextfidmut nextfid reissue defect \
        "the ribbon's fids left out of the next file id, a cut whose ribbon bore the anchor's file hands its id out again"
    restore src/kernel/tagfs/mount/book_mount.c
}

# A stitch never written — the cycle answers as though it were: the anchor's
# ack stands behind no stitch of its own.
run_stitchmut() {
    local PC_LIES=off
    mut_on src/kernel/tagfs/settle/sewing.c "logcheck mutation" <<'MUT_EOF'
import sys
p = sys.argv[1]; s = open(p).read()
a = "    int rc = tagfs_volume_write(vol, vlba, n, s->sectors + (uint64_t)s->written * BOOK_SECTOR_BYTES);\n"
assert s.count(a) == 1
open(p, "w").write(s.replace(a, "    int rc = (int)(vlba & 0u);   /* logcheck mutation: the stitch is never written */\n"))
MUT_EOF
    mut_run stitchmut nextfid ack:anchor_test fail "a stitch never written, the anchor's ack stands behind no stitch of its own"
    restore src/kernel/tagfs/settle/sewing.c
}

# The late arrival (powercut_book.sh replug): the machine passes over its own
# ATA disk, so the volume it mounts is the USB stick the host hands it — the
# stick a replug takes away and brings back. Not a mutant asked for a verdict:
# a tree edited so the scenario can be put at all, under the same manifest,
# plate and restore as every mutation (the same edit as logcheck.sh's).
pc_latearrival_on() {
    mut_on src/kernel/tagfs/tagfs.c "logcheck mutation" <<'MUT_EOF'
import sys
p = sys.argv[1]; s = open(p).read()
a = "static bool tagfs_recognise(void *ctx, uint8_t seat, uint8_t out_uuid[16])\n{\n"
assert s.count(a) == 1
open(p, "w").write(s.replace(a, a + "    if (BoardroomSeatKind(seat) == BOARD_ATA) return false;   /* logcheck mutation: the internal disk is not this machine's */\n"))
MUT_EOF
}
