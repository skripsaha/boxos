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
    mut_on src/kernel/tagfs/settle/cadence.c "logcheck mutation" <<'EOF'
import sys
p = sys.argv[1]; s = open(p).read()
a = "    if (waiters || closed || demand)\n        return CADENCE_NOW;\n"
assert s.count(a) == 1
open(p, "w").write(s.replace(a, "    if (waiters)\n        return CADENCE_NOW;\n"
                                "    return CADENCE_REST;   /* logcheck mutation: the cadence is off, only a waiter starts a cycle */\n"))
EOF
    mut_run noanchormut noanchor verify defect \
        "the cadence off, nothing settles persist's write before the power goes"
    restore src/kernel/tagfs/settle/cadence.c
}

# Since step 1.6 the mount's next file id is past every id the medium shows
# (mount/mount.c MountFidBoundary), so nextfid's fix is in, and its mutant
# takes it out: the Ledger's count stands, and a cut whose medium names the
# anchor's file ahead of its Ledger must hand that id out again.
run_nextfidmut() {
    mut_on src/kernel/tagfs/mount/mount.c "logcheck mutation" <<'EOF'
import sys
p = sys.argv[1]; s = open(p).read()
a = "    if (next != led->next_file_id) {\n"
assert s.count(a) == 1
open(p, "w").write(s.replace(a, "    if (0 && next != led->next_file_id) {   /* logcheck mutation: the Ledger's count stands */\n"))
EOF
    mut_run nextfidmut nextfid reissue defect \
        "the Ledger's count standing at the mount, a cut whose medium names the anchor's file hands its id out again"
    restore src/kernel/tagfs/mount/mount.c
}

# The durability oracle's own mutant. It used to leave the file table
# unwritten; since step 1.6 a sealed volume derives the table from the pool
# (mount/mount.c MountFile: a row the table lacks is set from the record), so
# the file is whole without it and that claim is gone. What a file's
# durability stands on now is its record in the pool, under the seal: the
# cycle's S3 writes nothing of the pool, and the anchored file must not be
# found whole after the flush its ack stands on. And the state that flush
# stood behind names what is wrong with it in volume-read's own code: the
# newest seal names records the medium does not hold, and the mount falls
# back from it (SEAL_FELL_BACK, with the rows it lets go) — where a cut before
# the flush may leave the same, and is no defect.
run_poolmut() {
    mut_on src/kernel/tagfs/settle/settle.c "logcheck mutation" <<'EOF'
import sys
p = sys.argv[1]; s = open(p).read()
a = "        rc          = PoolSnapWriteNext(&snap->pool, &done);\n"
assert s.count(a) == 1
open(p, "w").write(s.replace(a, "        rc          = OK;   /* logcheck mutation: the cycle writes nothing of the pool */\n"
                                "        done        = true;\n"))
EOF
    mut_run poolmut nextfid boundaries fail \
        "the cycle writing nothing of the pool, the state its flush stood behind does not stand on its newest seal"
    awk -F"$TAB" '$1 == "boundaries"' "$PC_DIR/verdicts.tsv" | grep -q 'SEAL_FELL_BACK: '
    chk $? "poolmut: and names it SEAL_FELL_BACK, the rows it lets go with it"
    pc_expect "durable:_anchor_test@anchor_test" fail \
        "the cycle writing nothing of the pool, the anchored file is not durable"
    restore src/kernel/tagfs/settle/settle.c
}

# Since step 1.7 an anchor asks the medium for nothing itself: it waits for
# the cycle that settles its change, and is answered from that cycle's end
# (settle_ops.c). So the one flush its ack stands on is the cycle's last, S5,
# after the Ledger — and that alone is taken out; the cycle's first flush, S2,
# still covers the data and the chains. The mutant is asked on the WriteJob's
# machine too, where the data's own flush comes before the cycle: the ack must
# not be credited with it.
run_flushmut() {
    mut_on src/kernel/tagfs/settle/settle.c "logcheck mutation" <<'EOF'
import sys
p = sys.argv[1]; s = open(p).read()
a = "        rc = VolumeFlushFor(VolumeFlushNeed());\n        snap->stage = \"the flush after the ledger\";"
assert s.count(a) == 1
open(p, "w").write(s.replace(a, "        rc = OK;   /* logcheck mutation: the cycle never asks the medium to flush after its ledger */\n"
                                "        snap->stage = \"the flush after the ledger\";"))
EOF
    mut_run flushmut nextfid ack:anchor_test fail "the cycle never flushing after its Ledger, the anchor's ack has no flush behind it"
    mut_run flushmut nextfid ack:anchor_test fail \
        "on the WriteJob the data's own flush comes first, and still the anchor's ack has none behind it" \
        STRICT=on CORES=4 MEM=4G
    restore src/kernel/tagfs/settle/settle.c
}
