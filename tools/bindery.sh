#!/bin/bash
# bindery — receipts across kernels: a volume one kernel wrote, read by the next.
#
#     HEIRLOOM_FROM=<commit> [HEIRLOOM_FORMAT=v1|v2] tools/bindery.sh heirloom
#     HEIRLOOM_FROM=<commit> tools/bindery.sh refusal
#
# A machine outlives its builds. The machines on the desk and the sticks beside
# them carry volumes written by whichever kernel they last ran, and the next
# kernel booted on them meets that volume as it stands — no build lays it down
# again. So a step that changes the kernel owes a receipt: the volume kernel N
# wrote is mounted by kernel N+1 and read whole, and kernel N can still read
# it afterwards. N is the commit HEIRLOOM_FROM names; N+1 is this tree.
#
#   1. Commit N, built whole in a git worktree of its own under SCRATCH — a
#      volume is laid down by mkfs N and written by kernel N, and both are N's.
#   2. Kernel N boots its own image, writes on it (`persist write`) and is
#      halted through `bye`. That volume is the heirloom.
#   3. This tree, built with ONE mutation: the boarding pass names the
#      heirloom (BoardingPassVolume). Its kernel boots from its own disk with
#      the heirloom on a USB stick, the pass wins, and the volume it mounts is
#      the heirloom as kernel N left it — the host has not written one byte of
#      it. It reads what kernel N wrote (`persist verify`), writes its own, and
#      is halted.
#   4. Kernel N boots the heirloom once more and reads what kernel N+1 wrote:
#      the way back, which a frozen format path owes as much as the way out.
#
# Between the machines the host only reads the heirloom (volume-read check),
# and that is measured, not said: the heirloom's sha256 is taken the moment a
# machine lets go of it, before the host reads a byte, and held to after every
# census and again just before the next machine starts.
#
# A write is told from a file left over by its id, not by its bytes: `persist
# write` writes the same sixteen bytes every time (persist.c), so the file
# kernel N wrote and the one kernel N+1 wrote read the same. So the id a WROTE
# line names is the one the census then finds, and the only one; the id a
# kernel's `persist verify` reads back is the one the other kernel wrote; and
# the files kernel N left are held to their ids as well as to their bytes.
#
# The programs run on the heirloom are the heirloom's, so kernel N+1 runs
# commit N's userspace: the receipt is the system-call surface's as well as the
# format's.
#
# HEIRLOOM_FORMAT=v2 makes the heirloom a book: commit N's volume laid down
# with VOLUME_FORMAT=v2 and this tree's disk too — kernel N's book read and
# written by kernel N+1, and back (receipt (а) of step 2.3: the kernel of 2.3j
# and the next). Unset, format v1 — named to every make of the run, commit N's
# and this tree's alike, never left to the format a build makes when nobody
# names one (spec_v2 §16.5 п.22): commit b9962f55's volume, the last kernel's
# before the book, is receipt (в) — the frozen path read on.
#
# `refusal` is the receipt the other way round, (б): a book THIS tree laid
# down and its kernel wrote, handed to kernel N — one from before the book
# (commit b9962f55, step 2.2b) — whose boarding pass names it. N must refuse it
# in the words step 2.1 gave it ("carries a format-v2 volume ... cannot read a
# format-v2 book") and leave every byte of it as it found it: its sha256 is
# held to the one this tree's halt left.

set -u
STAND_NAME=bindery
# ROOT, SCRATCH, the verdict counters, the mutation manifest and its
# preflights, the way out, the waits and the typer: tools/stand.sh.
. "$(dirname "$0")/stand.sh"

HEIRLOOM_FORMAT=${HEIRLOOM_FORMAT:-v1}
case "$HEIRLOOM_FORMAT" in v1|v2) ;; *) echo "bindery: HEIRLOOM_FORMAT=$HEIRLOOM_FORMAT names no format"; exit 2 ;; esac

HEIRLOOM_TREE="$SCRATCH/heirloom.tree"       # commit N, checked out and built
HEIRLOOM_IMG="$SCRATCH/heirloom.img"         # the volume kernel N writes
HEIRLOOM_BOOT="$SCRATCH/heirloom.boot.img"   # this tree's disk, kernel N+1 on it
HEIRLOOM_MARK="logcheck mutation: the boarding pass names the heirloom volume"
HEIRLOOM_PERSIST="_persist_test"             # what `persist write` names its file

# Commit N's worktree is the run's own: it goes when the run does, whichever
# way the run ends. `worktree remove --force` takes its registration with it,
# even when its directory is already gone, so nothing is pruned: a prune is the
# whole repository's, and it would take other trees' registrations too.
stand_exit_hook() {
    git -C "$ROOT" worktree remove --force "$HEIRLOOM_TREE" >/dev/null 2>&1
    rm -rf "$HEIRLOOM_TREE"
}

# heirloom_tree COMMIT — commit N checked out beside this tree and built whole.
heirloom_tree() {
    stand_exit_hook
    git -C "$ROOT" worktree add --detach "$HEIRLOOM_TREE" "$1" >"$SCRATCH/heirloom.tree.log" 2>&1 \
        || { cat "$SCRATCH/heirloom.tree.log"; return 1; }
    # Not this file's make(): commit N is not this tree, so nothing about it is
    # asked for on its behalf but the format its volume is laid down in and,
    # for `refusal`, the pass it carries (heirloom_old_pass, UUID given). The
    # one key every make of a harness carries is given all the same (stand.sh
    # says why): the pass is an injection, and N's own pack gate refuses to
    # stamp a tree carrying one unless the harness says it is asking.
    [ -z "${2:-}" ] || heirloom_old_pass "$2" || return 1
    command make -C "$HEIRLOOM_TREE" HARNESS=on VOLUME_FORMAT="$HEIRLOOM_FORMAT" \
        >"$SCRATCH/heirloom.build.log" 2>&1 || { tail -25 "$SCRATCH/heirloom.build.log"; return 1; }
}

# The pass names the heirloom: BoardingPassVolume answers the heirloom's
# sixteen bytes, so the Boardroom's choice and TagFS's ground are both the
# heirloom's, and the rest of the kernel is untouched.
heirloom_pass_on() {
    mutate src/kernel/core/boarding/boarding.c
    python3 - "$1" <<'EOF'
import sys
p = "src/kernel/core/boarding/boarding.c"
s = open(p).read()
anchor = """    if (out_uuid) {
        memcpy(out_uuid, v->uuid, 16);
    }
    return true;
}"""
assert s.count(anchor) == 1, "heirloom anchor missing"
uuid = bytes.fromhex(sys.argv[1])
assert len(uuid) == 16, "the heirloom's identity is not sixteen bytes"
words = ", ".join("0x%02x" % b for b in uuid)
mutated = ("    if (out_uuid) {\n"
           "        static const uint8_t heirloom[16] = { " + words + " };   "
           "/* logcheck mutation: the boarding pass names the heirloom volume */\n"
           "        memcpy(out_uuid, heirloom, 16);\n"
           "    }\n"
           "    return true;\n"
           "}")
open(p, "w").write(s.replace(anchor, mutated, 1))
EOF
    mut_done src/kernel/core/boarding/boarding.c "$HEIRLOOM_MARK" 1
}
heirloom_pass_off() {
    restore src/kernel/core/boarding/boarding.c
}

# heirloom_old_pass UUID — commit N's tree, checked out for this run alone,
# given the one change heirloom_pass_on gives this tree: the pass names UUID.
# Not under the manifest — the tree is the run's and goes with it.
heirloom_old_pass() {
    python3 - "$HEIRLOOM_TREE/src/kernel/core/boarding/boarding.c" "$1" "$HEIRLOOM_MARK" <<'PASS_EOF'
import sys
p, uuid, mark = sys.argv[1], bytes.fromhex(sys.argv[2]), sys.argv[3]
s = open(p).read()
anchor = "        memcpy(out_uuid, v->uuid, 16);\n"
assert s.count(anchor) == 1 and len(uuid) == 16, "heirloom anchor missing in commit N"
words = ", ".join("0x%02x" % b for b in uuid)
open(p, "w").write(s.replace(anchor, "        static const uint8_t heirloom[16] = { " + words + " };   /* " +
                             mark + " */\n        memcpy(out_uuid, heirloom, 16);\n", 1))
PASS_EOF
}

# heirloom_census IMG NAME — volume-read's census of IMG: its exit status;
# heirloom.NAME.files, every file the next mount would show — its name, its
# file id and its sha256; heirloom.NAME.count, how many files the census says
# the kernel will count.
heirloom_census() {
    python3 tools/volume-read.py "$1" check --json >"$SCRATCH/heirloom.$2.json" 2>&1
    local rc=$?
    python3 - "$SCRATCH/heirloom.$2.json" "$SCRATCH/heirloom.$2" <<'PY'
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except ValueError:
    d = {}
with open(sys.argv[2] + ".files", "w") as out:
    for f in sorted(d.get("files", []), key=lambda f: f["name"]):
        if f.get("visible"):
            out.write("%s %s %s\n" % (f["name"], f["fid"], f.get("sha256")))
k = d.get("kernel") or {}
says = (k.get("says") or {}).get("files") if d.get("format") != "v2" else k.get("files")
with open(sys.argv[2] + ".count", "w") as out:
    out.write("%s\n" % ("" if says is None else says))
PY
    return $rc
}

# The shell's prompt is read from the LINES, never from the last two bytes of
# the file, as wait_for_prompt read it then: on a book the cycle a write inked
# runs in the window after it, and what the kernel says on its way — the
# reserve of its own paths growing, a stitch settling — lands wherever its
# moment falls. Said right after a prompt it carries the prompt away from the
# end of the file ("~ [KLIB] …") and the echo of what is typed next onto a
# line of its own; wait_for_prompt waited its whole ceiling on a shell that
# stood there all along, and type_line, finding no "~ bye", typed it again
# (measured: refusal and the format-v2 heirloom, 2026-10-28). Read as
# fullvolume_type reads it (logcheck.sh).
#
# And counted from the ECHO, never from a count of lines taken before the
# typing: a kernel line said after the prompt and before that count ends the
# prompt's line inside it, and the shell's prompt before the command is then
# no longer past it — the echo and the next prompt are all there is, one
# prompt where two were counted, and the wait gives up on a shell standing at
# its prompt.
#
# heirloom_prompt AFTER — a line beginning "~ " past line AFTER: the shell's
# prompt, whatever the kernel appended to it; then the burst it may still be
# in, over.
heirloom_prompt() {
    wait_until "$WAIT_QUIET_SLOW_S" "the shell's prompt" \
        "tail -n +$(($1 + 1)) build/serial.log | grep -aq '^~ '" build/serial.log \
        || { echo "  (the shell never came back to its prompt)"; return 1; }
    wait_for_quiet "$BOOT_BURST_QUIET_S" build/serial.log
}

# heirloom_echo COMMAND FROM — the number of the first line past line FROM
# that carries COMMAND's echo, on the prompt's own line or on one of its own;
# nothing while there is none.
heirloom_echo() {
    local n
    n=$(tail -n +$(($2 + 1)) build/serial.log 2>/dev/null | grep -anE "^(~ )?$1" | head -1 | cut -d: -f1)
    [ -z "$n" ] || echo $(($2 + n))
}

# heirloom_type COMMAND FROM — typed once, the line counted FROM before it,
# and its echo waited for (heirloom_echo). Once: qemu-input.sh has seen every
# character echoed, a short line erased and typed whole again, and a second
# typing here would land on a line that holds the command already, or run it
# twice — two files where the receipt holds kernel N's one, and only it.
heirloom_type() {
    type_cmd "$1" && sleep 1 && key_press ret &&
        wait_until "$TYPE_ECHO_QUIET_S" "the echo of '$1'" "[ -n \"\$(heirloom_echo '$1' $2)\" ]" build/serial.log
}

# heirloom_machine TAG DISK STICK CMD... — boot DISK (with STICK on USB, when
# one is named), wait for a volume to be mounted and its shell to stand at the
# prompt, type each CMD, halt through `bye`, and keep the log as
# serial.heirloom_TAG.log. 0 when the halt's sync wrote the volume; 1 when a
# machine ran and did not get that far; 2 when none was started at all.
heirloom_machine() {
    local tag=$1 disk=$2 stick=$3 extra="" mark from cmd ran=1; shift 3
    [ -n "$stick" ] && extra="-drive if=none,id=stick,file=$stick,format=raw -device usb-storage,bus=xhci1.0,drive=stick,id=usbstick"
    make run-stop >/dev/null 2>&1
    rm -f build/serial.log
    if ! make run-bg BOOTDISK="$disk" USB=on CORES=4 MEM=4G QEMUEXTRA="$extra" \
            >"$SCRATCH/runbg.heirloom_$tag.log" 2>&1 || [ ! -S build/qemu.mon ]; then
        harness "heirloom: no machine was started for $tag — $(tail -1 "$SCRATCH/runbg.heirloom_$tag.log" | tr -d '\r')"
        make run-stop >/dev/null 2>&1
        return 2
    fi
    # The volume's own shell, which is the one that comes up AFTER the mount:
    # a stand-in may have stood at a prompt while the stick was still arriving.
    if wait_for_line -E build/serial.log 'is MOUNTED — [0-9]+ file|KERNEL PANIC' "$WAIT_QUIET_S" &&
       mark=$(grep -an 'is MOUNTED — [0-9]* file' build/serial.log | head -1 | cut -d: -f1) &&
       [ -n "$mark" ] && wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S" "$mark" &&
       heirloom_prompt "$mark"; then
        ran=0
        for cmd in "$@"; do
            from=$(wc -l < build/serial.log)
            heirloom_type "$cmd" "$from" && heirloom_prompt "$(heirloom_echo "$cmd" "$from")" || { ran=1; break; }
        done
        from=$(wc -l < build/serial.log)
        if [ $ran = 0 ] && ! { heirloom_type "bye" "$from" &&
                               wait_for_line -E build/serial.log "$STAND_HALT_ENDS" "$WAIT_QUIET_S" &&
                               grep -aq '^\[HALT\] Storage sync complete' build/serial.log; }; then
            ran=1
        fi
    fi
    make run-stop >/dev/null 2>&1
    tr -d '\r' < build/serial.log > "$SCRATCH/serial.heirloom_$tag.log"
    return $ran
}

# heirloom_mounted LOG UUID FILES — the volume UUID was mounted, on the seat
# its Deed was read from, with FILES files; the seat's number is the answer.
heirloom_mounted() {
    local seat
    seat=$(grep -aoE "^\[Deed\] seat [0-9]+: volume $2, " "$1" | head -1 | grep -oE '[0-9]+' | head -1)
    [ -n "$seat" ] && grep -aqE "^\[TagFS\] the volume on seat $seat is MOUNTED — $3 file\(s\)" "$1" \
        && echo "$seat"
}

# heirloom_kernel LOG IMG — the kernel that ran is the one IMG carries: the
# tree its plate names, as tools/buildplate reads it off the image. Not
# anchored at the start of a line: the plate is the kernel's first line, and
# the loader's progress letters stand in front of it on the same one.
heirloom_kernel() {
    local tree
    tree=$(tools/buildplate say "$2" 2>/dev/null | grep -oE 'tree [0-9a-f]{16}' | head -1)
    [ -n "$tree" ] && grep -aq "\[BuildPlate\] $tree, " "$1" && echo "${tree#tree }"
}

# heirloom_clean LOG — no panic and no fault on the way.
heirloom_clean() { ! grep -aqE "KERNEL PANIC|\[EXCEPTION\]" "$1"; }

heirloom_sha() { shasum -a 256 < "$HEIRLOOM_IMG" | cut -d' ' -f1; }

# heirloom_let_go WHO — the heirloom's bytes the moment the machine WHO ran let
# go of it, before the host has read one of them: every look the host takes
# after it is held to these.
heirloom_let_go() { HEIRLOOM_SHA=$(heirloom_sha); HEIRLOOM_LEFT_BY=$1; }

# heirloom_untouched WHAT — WHAT wrote nothing on the heirloom since the last
# machine let go of it.
heirloom_untouched() {
    [ -n "$HEIRLOOM_SHA" ] && [ "$(heirloom_sha)" = "$HEIRLOOM_SHA" ]
    chk $? "heirloom: $1 wrote nothing on the heirloom — its sha256 is still the one $HEIRLOOM_LEFT_BY left"
}

# heirloom_fid LOG WORD — the file id persist's WORD line in LOG names: WROTE,
# the file it made; found, the file it read back. Empty when it said neither.
heirloom_fid() { grep -aoE "\[PERSIST\] $2 fid=[0-9]+" "$1" | head -1 | grep -oE '[0-9]+$'; }

# heirloom_persist NAME — the ids of every persist file census NAME shows, in a
# row; "none" when it shows none.
heirloom_persist() {
    local ids
    ids=$(grep "^$HEIRLOOM_PERSIST " "$SCRATCH/heirloom.$1.files" | cut -d' ' -f2 | paste -sd' ' -)
    echo "${ids:-none}"
}

# heirloom_said LOG — persist's verdict line, for the record.
heirloom_said() { grep -aoE '\[PERSIST\] (PASS|FAIL).*' "$1" | head -1 | cut -c1-60; }

# What one stage leaves the next: the heirloom's identity, the tree kernel N
# was built from, the heirloom's bytes as the last machine let go of them and
# which machine that was, and the file each kernel wrote.
HEIRLOOM_UUID=""
HEIRLOOM_N_TREE=""
HEIRLOOM_SHA=""
HEIRLOOM_LEFT_BY=""
HEIRLOOM_N_FID=""
HEIRLOOM_N1_FID=""

# Kernel N lays the heirloom down and writes on it.
heirloom_written() {
    local from=$1 L="$SCRATCH/serial.heirloom_n.log" rc
    echo "-- kernel N: commit $from, built whole in $HEIRLOOM_TREE --"
    if ! heirloom_tree "$from" || ! cp "$HEIRLOOM_TREE/build/boxos.img" "$HEIRLOOM_IMG"; then
        harness "heirloom: commit $from would not check out and build ($SCRATCH/heirloom.build.log)"
        return 1
    fi
    HEIRLOOM_UUID=$(deed_uuid "$HEIRLOOM_IMG") || HEIRLOOM_UUID=""
    [ -n "$HEIRLOOM_UUID" ]
    chk $? "heirloom: commit $from laid down a volume whose Deed reads (${HEIRLOOM_UUID:-none})"
    [ -n "$HEIRLOOM_UUID" ] || return 1

    heirloom_machine n "$HEIRLOOM_IMG" "" "persist write"; rc=$?
    [ $rc = 2 ] && return 1
    heirloom_let_go "kernel N"
    HEIRLOOM_N_TREE=$(heirloom_kernel "$L" "$HEIRLOOM_IMG")
    [ -n "$HEIRLOOM_N_TREE" ]
    chk $? "heirloom: the machine ran kernel N, the one on the heirloom (tree ${HEIRLOOM_N_TREE:-unread})"
    [ -n "$(heirloom_mounted "$L" "$HEIRLOOM_UUID" "[0-9]+")" ]
    chk $? "heirloom: kernel N mounted its own volume"
    HEIRLOOM_N_FID=$(heirloom_fid "$L" WROTE)
    [ -n "$HEIRLOOM_N_FID" ]
    chk $? "heirloom: kernel N wrote on it ($(grep -aoE '\[PERSIST\] WROTE fid=[0-9]+ seq=[0-9]+' "$L" | head -1))"
    [ $rc = 0 ]
    chk $? "heirloom: kernel N halted through bye, and its sync wrote the volume"
    heirloom_clean "$L"
    chk $? "heirloom: kernel N — no panic, no fault"

    heirloom_census "$HEIRLOOM_IMG" n
    chk $? "heirloom: the volume kernel N left is consistent (volume-read check)"
    heirloom_untouched "the host's census after $HEIRLOOM_LEFT_BY"
    [ -n "$HEIRLOOM_N_FID" ] && [ "$(heirloom_persist n)" = "$HEIRLOOM_N_FID" ]
    chk $? "heirloom: and it carries the file kernel N wrote, and only it — $HEIRLOOM_PERSIST fid(s) $(heirloom_persist n), kernel N's ${HEIRLOOM_N_FID:-unread}"
    # With no file of N's the rest has nothing to hand on, and would judge
    # kernel N+1 by one never written — a magic that does not match, a census
    # short of it.
    if [ -z "$HEIRLOOM_N_FID" ]; then
        heirloom_unwritten "$from" "$L"
        return 1
    fi
}

# heirloom_unwritten COMMIT LOG — why kernel N, commit COMMIT, left no file to
# hand on, said as far as LOG and the history establish it and no further. Its
# machine never took `persist write`: that, and the FAILs above. It took it on
# a book, and COMMIT is from before the first kernel that writes a file's data
# on one (HEIRLOOM_BOOK_DATA): the operator's choice of N, no verdict on
# BoxOS — a HARNESS line, the run inconclusive. Otherwise only what persist
# said and what the census found: the FAILs above are the verdict on kernel N.
HEIRLOOM_BOOK_DATA=9825f2b1   # step 2.3j: a format-v2 book takes a file's data
heirloom_unwritten() {
    local said census
    said=$(grep -aoE '\[PERSIST\] .*' "$2" | tail -1 | cut -c1-80)
    census="$HEIRLOOM_PERSIST fid(s) on its volume: $(heirloom_persist n)"
    if ! grep -aqE '^(~ )?persist write' "$2"; then
        echo "  (kernel N, commit $1, never took 'persist write' — its machine stopped before it; nothing of N's to hand on)"
    elif [ "$HEIRLOOM_FORMAT" = v2 ] &&
         { git -C "$ROOT" merge-base --is-ancestor "$HEIRLOOM_BOOK_DATA" "$1" 2>/dev/null; [ $? = 1 ]; }; then
        harness "heirloom: commit $1 is from before $HEIRLOOM_BOOK_DATA (step 2.3j), the first kernel to write a file's data on a book — no N for HEIRLOOM_FORMAT=v2 (persist: ${said:-nothing}; $census)"
    else
        echo "  (kernel N, commit $1, took 'persist write' and said no WROTE — persist: ${said:-nothing}; $census; nothing of N's to hand on)"
    fi
}

# Kernel N+1 is handed the heirloom on a stick, reads it, and writes on it.
heirloom_handed() {
    local L="$SCRATCH/serial.heirloom_n1.log" rc count tree seat own kept found handed
    echo "-- kernel N+1: this tree, its boarding pass naming the heirloom $HEIRLOOM_UUID --"
    heirloom_pass_on "$HEIRLOOM_UUID"
    STAND_FORMAT=$HEIRLOOM_FORMAT build
    if ! cp build/boxos.img "$HEIRLOOM_BOOT"; then
        harness "heirloom: this tree's image could not be copied to $HEIRLOOM_BOOT"
        heirloom_pass_off
        return 1
    fi
    count=$(cat "$SCRATCH/heirloom.n.count")
    own=$(deed_uuid "$HEIRLOOM_BOOT") || own=""

    heirloom_untouched "the host, between the machines,"
    handed=$(heirloom_sha)
    STAND_FORMAT=$HEIRLOOM_FORMAT heirloom_machine n1 "$HEIRLOOM_BOOT" "$HEIRLOOM_IMG" "persist verify" "persist write"; rc=$?
    heirloom_pass_off
    [ $rc = 2 ] && return 1
    heirloom_let_go "kernel N+1"
    [ "$HEIRLOOM_SHA" != "$handed" ]
    chk $? "heirloom: kernel N+1's writes reached the heirloom — its sha256 is not the one it was handed"
    tree=$(heirloom_kernel "$L" "$HEIRLOOM_BOOT")
    [ -n "$tree" ] && [ -n "$HEIRLOOM_N_TREE" ] && [ "$tree" != "$HEIRLOOM_N_TREE" ]
    chk $? "heirloom: the machine ran kernel N+1, this tree's (tree ${tree:-unread}; kernel N's is ${HEIRLOOM_N_TREE:-unread})"
    seat=$(heirloom_mounted "$L" "$HEIRLOOM_UUID" "$count")
    [ -n "$seat" ] && [ -n "$own" ] && [ -z "$(heirloom_mounted "$L" "$own" "[0-9]+")" ]
    chk $? "heirloom: kernel N+1 mounted the heirloom — seat ${seat:-none}, the $count file(s) kernel N left — and not the volume on its own disk"
    found=$(heirloom_fid "$L" found)
    grep -aq '\[PERSIST\] PASS: file survived reboot, payload intact' "$L" &&
        [ -n "$found" ] && [ "$found" = "$HEIRLOOM_N_FID" ]
    chk $? "heirloom: kernel N+1 read back what kernel N wrote — fid ${found:-none}, kernel N's ${HEIRLOOM_N_FID:-unread} ($(heirloom_said "$L"))"
    HEIRLOOM_N1_FID=$(heirloom_fid "$L" WROTE)
    [ -n "$HEIRLOOM_N1_FID" ] && [ "$HEIRLOOM_N1_FID" != "$HEIRLOOM_N_FID" ]
    chk $? "heirloom: kernel N+1 wrote a file of its own on the heirloom — fid ${HEIRLOOM_N1_FID:-none}, not kernel N's ${HEIRLOOM_N_FID:-unread}"
    [ $rc = 0 ]
    chk $? "heirloom: kernel N+1 halted through bye, and its sync wrote the heirloom"
    heirloom_clean "$L"
    chk $? "heirloom: kernel N+1 — no panic, no fault"

    heirloom_census "$HEIRLOOM_IMG" n1
    chk $? "heirloom: the heirloom kernel N+1 left is consistent (volume-read check)"
    heirloom_untouched "the host's census after $HEIRLOOM_LEFT_BY"
    [ -n "$HEIRLOOM_N1_FID" ] && [ "$(heirloom_persist n1)" = "$HEIRLOOM_N1_FID" ]
    chk $? "heirloom: and it carries the file kernel N+1 wrote, and only it — $HEIRLOOM_PERSIST fid(s) $(heirloom_persist n1), kernel N+1's ${HEIRLOOM_N1_FID:-unread}"
    kept=$(diff <(grep -v "^$HEIRLOOM_PERSIST " "$SCRATCH/heirloom.n.files") \
                <(grep -v "^$HEIRLOOM_PERSIST " "$SCRATCH/heirloom.n1.files") | grep -c '^[<>]')
    [ "$kept" = 0 ]
    chk $? "heirloom: every file kernel N left is there under its id, byte for byte, but the one kernel N+1 wrote again ($kept line(s) differ)"
}

# Kernel N boots the heirloom kernel N+1 left, and reads what N+1 wrote.
heirloom_back() {
    local L="$SCRATCH/serial.heirloom_back.log" rc count found
    echo "-- kernel N again, on the heirloom kernel N+1 left --"
    count=$(cat "$SCRATCH/heirloom.n1.count")

    heirloom_untouched "the host, between the machines,"
    heirloom_machine back "$HEIRLOOM_IMG" "" "persist verify"; rc=$?
    [ $rc = 2 ] && return 1
    heirloom_let_go "kernel N, the way back,"
    [ -n "$HEIRLOOM_N_TREE" ] && [ "$(heirloom_kernel "$L" "$HEIRLOOM_IMG")" = "$HEIRLOOM_N_TREE" ]
    chk $? "heirloom: the machine ran kernel N again (tree ${HEIRLOOM_N_TREE:-unread})"
    [ -n "$(heirloom_mounted "$L" "$HEIRLOOM_UUID" "$count")" ]
    chk $? "heirloom: kernel N mounted the heirloom kernel N+1 left, its $count file(s)"
    found=$(heirloom_fid "$L" found)
    grep -aq '\[PERSIST\] PASS: file survived reboot, payload intact' "$L" &&
        [ -n "$found" ] && [ "$found" = "$HEIRLOOM_N1_FID" ]
    chk $? "heirloom: kernel N read back what kernel N+1 wrote — fid ${found:-none}, kernel N+1's ${HEIRLOOM_N1_FID:-unread} ($(heirloom_said "$L"))"
    [ $rc = 0 ]
    chk $? "heirloom: kernel N halted through bye, and its sync wrote the heirloom"
    heirloom_clean "$L"
    chk $? "heirloom: kernel N, the way back — no panic, no fault"

    heirloom_census "$HEIRLOOM_IMG" back
    chk $? "heirloom: the heirloom kernel N left the second time is consistent (volume-read check)"
    heirloom_untouched "the host's census after $HEIRLOOM_LEFT_BY"
}

# --- refusal: a book this tree wrote, handed to a kernel from before the book --

# Step 2.1's refusal of a book (deed.c, DeedFormatRefused), as kernel N says it.
REFUSAL_SAID='carries a format-v2 volume \(its deed bears FRAME\); this kernel cannot read a format-v2 book'

# The book laid down by this tree and written by its kernel: `persist write`,
# halted through `bye`.
refusal_written() {
    local L="$SCRATCH/serial.heirloom_k.log" rc
    echo "-- this tree: a book laid down (VOLUME_FORMAT=v2) and written by its kernel --"
    rm -f build/boxos.img
    if ! STAND_FORMAT=v2 build_with_key VOLUME_FORMAT=v2 || ! cp build/boxos.img "$HEIRLOOM_IMG"; then
        harness "refusal: this tree would not build a book ($SCRATCH/build.log)"
        return 1
    fi
    HEIRLOOM_UUID=$(deed_uuid "$HEIRLOOM_IMG") || HEIRLOOM_UUID=""
    [ -n "$HEIRLOOM_UUID" ] || { harness "refusal: the book's Deed would not read"; return 1; }
    STAND_FORMAT=v2 heirloom_machine k "$HEIRLOOM_IMG" "" "persist write"; rc=$?
    [ $rc = 2 ] && return 1
    heirloom_let_go "this tree's kernel"
    [ -n "$(heirloom_fid "$L" WROTE)" ] && [ $rc = 0 ]
    chk $? "refusal: this tree's kernel wrote on its book and halted, its sync writing it ($(grep -aoE '\[PERSIST\] WROTE fid=[0-9]+' "$L" | head -1))"
    heirloom_census "$HEIRLOOM_IMG" k
    chk $? "refusal: volume-read finds the book this tree's kernel left consistent"
    heirloom_untouched "the host's census after $HEIRLOOM_LEFT_BY"
}

# refusal_machine DISK STICK — DISK booted with STICK on USB until a volume is
# mounted or refused or the kernel panics, then stopped without a halt: a
# kernel that refuses its volume has no shell to say `bye` to. The log is kept
# as serial.heirloom_old.log; 2 when no machine was started.
refusal_machine() {
    local extra="-drive if=none,id=stick,file=$2,format=raw -device usb-storage,bus=xhci1.0,drive=stick,id=usbstick"
    make run-stop >/dev/null 2>&1
    rm -f build/serial.log
    if ! make run-bg BOOTDISK="$1" USB=on CORES=4 MEM=4G QEMUEXTRA="$extra" \
            >"$SCRATCH/runbg.heirloom_old.log" 2>&1 || [ ! -S build/qemu.mon ]; then
        harness "refusal: no machine was started for kernel N — $(tail -1 "$SCRATCH/runbg.heirloom_old.log" | tr -d '\r')"
        make run-stop >/dev/null 2>&1
        return 2
    fi
    wait_for_line -E build/serial.log "$REFUSAL_SAID|is MOUNTED — [0-9]+ file|KERNEL PANIC" "$WAIT_QUIET_S"
    sleep 3       # what the kernel says after the refusal, what it stands on instead
    make run-stop >/dev/null 2>&1
    tr -d '\r' < build/serial.log > "$SCRATCH/serial.heirloom_old.log"
}

run_refusal() {
    echo "== refusal: a book this tree wrote, handed to kernel N, is refused in step 2.1's words and not touched =="
    local from=${HEIRLOOM_FROM:-} L="$SCRATCH/serial.heirloom_old.log" seat old
    if [ -z "$from" ] || ! git -C "$ROOT" rev-parse -q --verify "$from^{commit}" >/dev/null; then
        harness "refusal: HEIRLOOM_FROM names no commit ('$from') — it says which kernel from before the book refuses it"
        return
    fi
    build
    refusal_written || { buildkey_restore; return; }
    echo "-- kernel N: commit $from, its boarding pass naming this tree's book $HEIRLOOM_UUID --"
    if ! HEIRLOOM_FORMAT=v1 heirloom_tree "$from" "$HEIRLOOM_UUID" ||
       ! cp "$HEIRLOOM_TREE/build/boxos.img" "$HEIRLOOM_BOOT"; then
        harness "refusal: commit $from would not check out and build ($SCRATCH/heirloom.build.log)"
        buildkey_restore; return
    fi
    heirloom_untouched "the host, between the machines,"
    refusal_machine "$HEIRLOOM_BOOT" "$HEIRLOOM_IMG"
    [ $? = 2 ] && { buildkey_restore; return; }
    old=$(heirloom_kernel "$L" "$HEIRLOOM_BOOT")
    [ -n "$old" ]
    chk $? "refusal: the machine ran kernel N, commit $from's (tree ${old:-unread})"
    seat=$(grep -aoE "^\[Deed\] seat [0-9]+: volume $HEIRLOOM_UUID, " "$L" | head -1 | grep -oE '[0-9]+' | head -1)
    [ -n "$seat" ] && grep -aqE "^\[TagFS\] seat $seat $REFUSAL_SAID" "$L"
    chk $? "refusal: kernel N found the book on seat ${seat:-none} and refused it in step 2.1's words ($(grep -aoE "seat [0-9]+ $REFUSAL_SAID" "$L" | head -1 | cut -c1-90))"
    ! grep -aqE "^\[TagFS\] the volume on seat ${seat:-x} is MOUNTED" "$L"
    chk $? "refusal: and did not mount it"
    heirloom_clean "$L"
    chk $? "refusal: kernel N — no panic, no fault"
    heirloom_untouched "kernel N"
    buildkey_restore
}

run_heirloom() {
    echo "== heirloom: the volume kernel N wrote, mounted and read by kernel N+1 — and back =="
    local from=${HEIRLOOM_FROM:-}
    if [ -z "$from" ] || ! git -C "$ROOT" rev-parse -q --verify "$from^{commit}" >/dev/null; then
        harness "heirloom: HEIRLOOM_FROM names no commit ('$from') — it says which kernel's volume is the heirloom"
        return
    fi
    build   # this tree, built, before anything is asked of its tools
    heirloom_written "$from" && heirloom_handed && heirloom_back
    build   # leave the tree built from clean sources
}

case "${1:-}" in
    heirloom) run_heirloom ;;
    refusal)  run_refusal ;;
    *) echo "usage: HEIRLOOM_FROM=<commit> [HEIRLOOM_FORMAT=v1|v2] $0 heirloom"
       echo "       HEIRLOOM_FROM=<commit> $0 refusal"; exit 2 ;;
esac

STAND_FINISHED=1
echo
echo "bindery: $PASS passed, $FAIL failed"
if [ "$HARNESS" != 0 ]; then
    echo "bindery: $HARNESS harness failure(s) — this run could not do what it set out"
    echo "         to, so its verdicts are not evidence."
fi
[ "$FAIL" = 0 ] && [ "$HARNESS" = 0 ]
