#!/bin/bash
# powercut — the power goes at every flush, and at every subset of the writes
# between two of them.
#
#   tools/powercut.sh SCENARIO [MACHINE KEYS...]    one scenario on one machine
#   tools/powercut.sh all                           the matrix below, end of work only
#
# THE MODEL. A scenario boots a settled copy of the volume with `make run-bg
# POWERCUT=on`, so QEMU's blklogwrites puts every write and every flush the
# machine makes on a record, in the order the disk finished them. It asks the
# machine something, and then the power goes — kill -9, which no guest code
# outlives — or the machine is switched off by its power button. Every medium
# a power cut could have left is then built on the host from the base and the
# record (tools/volume-read.py, the one owner of both formats) and judged by
# volume-read's GroundCensus, which also predicts what the next mount does; a
# chosen few are booted, and the kernel's own mount is held to that
# prediction. The emulator cannot pull a plug. One run pulls it at every
# flush, at every subset of the writes between two flushes (or, past ten
# writes, every prefix, every single omission and a seeded sample — the
# report counts exactly what was not built), and through the torn last write
# of each epoch. tools/powercut.py plans, fans the checks out and judges.
#
# SCENARIOS (MACHINE KEYS are `make run-bg` keys; none is the plain BIOS stand)
#   noanchor    persist write, never anchored; 5+D s later the power goes; the
#               final state booted and `persist verify` asked: the cadence
#               settled the write (step 1.7 lifted the caveat).
#   nextfid     anchor_test, then the power goes. Every prefix of the anchor's
#               sync is booted and anchor_test asked again: no cut hands out an
#               id its medium already shows (step 1.6 lifted the caveat).
#   hog         one core: quilltest settle (a write, not anchored, beside a
#               hog); 5+D s later the power goes; `quilltest verify` (step 1.7
#               lifted the caveat). And the design's "an anchor under the hog
#               answers": `quilltest hogged anchor_test` on the booted state.
#   halt        quilltest churn writes for ever; the power button. The machine
#               must switch off, and what churn wrote is durable behind the
#               halt's flush: a guard, whose census codes are held (below).
#   race        quilltest race: 200 anchors against a churn, then the power
#               button; the halt's oracles, and an anchored file lost by a
#               later torn sync held under 1.4.
#   settlecut   the second oracle: kernels built SETTLECUT=ledger and =done,
#               each booted from a copy, anchor_test, the machine stopped
#               inside the cycle that settles the anchor; volume-read's codes
#               against PowerCut's cut at the same point of the same cycle.
#   chaingrow   MP-D5: the base brought by rounds of quilltest crowd, each
#               measured, to both edges — the file table one create short of
#               its next block, the tag registry's last block room for one more
#               tag — then `quilltest crowd` grows both chains and anchors, and
#               the power goes. Its census is kept by kind, prefix and subset
#               apart, and the first cut of every kind and every chain refused
#               is booted.
#   noanchormut nextfidmut poolmut flushmut
#               the fix taken out or the fault put in on purpose: the cadence
#               off (noanchor's verify), the mount's derived next file id off
#               (nextfid's reissue), the durability or the ack must say the
#               defect is back.
#   selftest    the planner's arithmetic, settlecut's patterns against the
#               sentences settle_cut.c prints, and a record forged by one byte
#               that the record's self-check must refuse.
#
# FORMAT v2 (VOLUME_FORMAT=v2): noanchor, nextfid, hog, halt, race and
# noanchormut on a book — every image the run builds is one, its record named
# by the book's units and its cuts judged by the book's census; the scenarios
# of format v1's own machinery are refused by name (below). `all` runs the
# book's set.
#
# Every census code a scenario was measured showing is its own caveat row —
# (scenario, code), stand_caveat.sh — held under 1.6, the ORPHAN of an
# unanchored create (noanchor, hog) and race's lost anchored file under 1.4
# (the owner, 2026-09-27); chaingrow's are (scenario, prefix|subset, code),
# a chain refused named by its chain; its refusals were held under 1.1, which
# lifted them.
# A caveated code no cut shows is lifted early only where its absence is
# proof: every medium of its kind built — every subset, or for a prefix
# code every prefix — and no writer left writing through the record.
# Anywhere else it is said, and counts for nothing.
#
# ORACLES, each one line: the record accounts for the disk QEMU left, byte for
# byte (else nothing measured means anything: HARNESS); every durable step has
# a flush behind its acknowledgement; no cut loses or changes a file the base
# held; an item made durable is whole in every cut after its flush; the
# volume is consistent wherever a sync's flush stood behind it; every
# damage or leak volume-read names is held under a named caveat or is a FAIL
# the owner must assign a step (no code is ever silently green); every booted
# cut mounts without a panic, and its mount, fsck and counts are the ones
# volume-read predicted; and the scenario's own question on the final state.
#
# Exit: 0 when nothing failed and the harness could talk to the machine,
# 1 otherwise, 2 for usage. Held caveats do not change it.

set -u
STAND_NAME=powercut
# ROOT, SCRATCH, the verdict counters, the mutation manifest and its
# preflights, the way out, the waits, the typer and the Caveat judge.
. "$(dirname "$0")/stand.sh"
export PYTHONDONTWRITEBYTECODE=1        # a tools/__pycache__ appeared once already

PC_SUBSET_ALL_MAX=${PC_SUBSET_ALL_MAX:-10}  # an epoch this small is enumerated whole
PC_SAMPLE_SUBSETS=${PC_SAMPLE_SUBSETS:-64}  # drawn per larger epoch, besides prefixes and omissions
PC_TEAR_ALL_MAX=${PC_TEAR_ALL_MAX:-16}      # a last write this short is torn at every sector
PC_SAMPLE_BOOTS=${PC_SAMPLE_BOOTS:-8}       # seeded boots beyond the ones the policy names
PC_CHECK_JOBS=${PC_CHECK_JOBS:-6}           # volume-read checks run at once
# How long the record may pause and still be the same burst of writes — a
# property of the writer, like BOOT_BURST_QUIET_S, not a guess at the machine.
PC_RECORD_QUIET_S=${PC_RECORD_QUIET_S:-3}
# The promise under test (tagfs.c's word on an unanchored write): settled
# within PC_CEILING_S seconds. D is the slack a loaded host is given.
PC_CEILING_S=5
PC_D_S=${PC_D_S:-2}
PC_ROOT="$SCRATCH/powercut"
VR="tools/volume-read.py"
PC="tools/powercut.py"
TAB=$(printf '\t')

# ---------------------------------------------------------------------------
# The stand
# ---------------------------------------------------------------------------

pc_preflight() {
    qemu-system-x86_64 -drive format=help 2>/dev/null | grep -qw blklogwrites || {
        echo "powercut: this QEMU has no blklogwrites driver, and without it nothing is recorded"; exit 1; }
    python3 "$VR" --help 2>/dev/null | grep -q "check" && python3 "$VR" --help | grep -q "cut" || {
        echo "powercut: $VR does not offer check and cut"; exit 1; }
    case "$PC_ROOT" in *,*|*[[:space:]]*)
        echo "powercut: $PC_ROOT holds a comma or a blank: QEMU reads a comma in a file name as"
        echo "          the end of the option, and run-bg hands the disk's path to QEMU as"
        echo "          words — run under a TMPDIR with neither"; exit 1 ;;
    esac
}

# The QEMU a run left is killed on the way out — only one booted from this
# run's own directory, so another harness's machine is never touched.
stand_exit_hook() {
    local pid
    pid=$(cat build/qemu.pid 2>/dev/null) || return 0
    case "$pid" in ''|*[!0-9]*) return 0 ;; esac
    if kill -0 "$pid" 2>/dev/null && ps -o command= -p "$pid" | grep -qF "$PC_ROOT"; then
        kill -9 "$pid" 2>/dev/null
        rm -f build/qemu.pid build/qemu.mon build/qemu.qmp
    fi
}

# The record's superblock (blklogwrites.c 38-43): u64 magic, u64 version, u64
# entries. Read live while QEMU writes it; everything else about the record
# is volume-read's. 0 until the first entry lands. Both u64s are read in
# decimal: BSD od pads hex with blanks and GNU od with zeros, and a magic
# compared as padded hex is recognised on one desk and never on the other.
# 29963231459240050 is WRITE_LOG_MAGIC, 0x6a736677736872.
pc_record_count() {
    [ "$(od -An -t u8 -N 8 "$1" 2>/dev/null | tr -d ' ')" = 29963231459240050 ] || { echo 0; return; }
    od -An -t u8 -j 16 -N 8 "$1" | tr -d ' '
}

pc_clone() {   # pc_clone FROM TO — a copy-on-write clone where the filesystem has one
    rm -f "$2"
    cp -c "$1" "$2" 2>/dev/null && return 0
    echo "  (no clone here: $2 is a full copy of $(($(wc -c < "$1") >> 20)) MiB)"
    cp "$1" "$2"
}

# pc_build WANT — build, and make the plate say which tree the kernel is of.
# The plate is stamped when the kernel is linked, so an edit that relinks
# nothing (a program, a script) leaves the plate naming the tree before it;
# the kernel is then linked again, and the plate must say WANT: 0 for a clean
# tree, 4 (UNDER TEST) for one a *mut scenario mutated. Built with the STRICT
# the scenario's machine boots with (stand.sh, strict_key): every disk a
# scenario boots is a clone of this image and carries its kernel.
pc_build() {
    local rc strict
    strict=$(strict_key ${PC_KEYS[@]+"${PC_KEYS[@]}"})
    build "$strict"
    tools/buildplate verify build/boxos.img --root . >"$SCRATCH/plate.txt" 2>&1; rc=$?
    if [ "$rc" = 3 ]; then
        rm -f build/kernel.bin
        build "$strict"
        tools/buildplate verify build/boxos.img --root . >"$SCRATCH/plate.txt" 2>&1; rc=$?
    fi
    [ "$rc" = "$1" ] && return 0
    # The plate's own two numbers say which: a tree that changed under the
    # build (a file edited while this ran) or an image that is not this tree.
    harness "powercut: the image's plate answers $rc where $1 was due, so nothing says it is this tree: $(grep -a 'tree ' "$SCRATCH/plate.txt" | sed 's/^ *//' | tr '\n' ';')"
    return 1
}

# pc_begin NAME KEYS... — the machine keys are kept as the words they were
# given, so a key whose value holds a blank (QEMUEXTRA='-machine pc', the
# matrix's PIIX stand) reaches make as one key. Expanded as
# ${PC_KEYS[@]+"${PC_KEYS[@]}"}: bash 3.2 under set -u calls an empty array
# unbound, and a scenario with no keys is the plain stand.
pc_begin() {
    local key
    PC_NAME=$1; shift
    PC_KEYS=("$@")
    PC_DIR="$PC_ROOT/$PC_NAME-$(printf '%s' "${PC_KEYS[*]:-plain}" | tr ' =,/' '_-__')"
    PC_CORES=1
    for key in ${PC_KEYS[@]+"${PC_KEYS[@]}"}; do
        case "${key#CORES=}" in "$key"|''|*[!0-9]*) ;; *) PC_CORES=${key#CORES=} ;; esac
    done
    PC_PATH="unknown"
    rm -rf "$PC_DIR"
    mkdir -p "$PC_DIR/boots"
    : > "$PC_DIR/steps.jsonl"
    : > "$PC_DIR/facts.txt"
    PC_T0=$(date +%s)
    echo "== powercut $PC_NAME${PC_KEYS[*]:+ (${PC_KEYS[*]})} =="
}

# pc_extra WORDS — the QEMUEXTRA key this scenario's machine is booted with,
# WORDS added: what the machine keys (or the environment) said, and WORDS
# after it. make keeps only the last of a key given twice, so a scenario that
# added its own would boot another machine than the one it was asked for.
pc_extra() {
    local key extra=${QEMUEXTRA:-}
    for key in ${PC_KEYS[@]+"${PC_KEYS[@]}"}; do
        case "$key" in QEMUEXTRA=*) extra=${key#QEMUEXTRA=} ;; esac
    done
    echo "QEMUEXTRA=${extra:+$extra }$1"
}

# pc_boot DISK [KEYS...] — DISK booted as it stands, on this scenario's
# machine, to a shell that has stopped talking.
pc_boot() {
    local disk=$1; shift
    make run-stop >/dev/null 2>&1
    if ! make run-bg BOOTDISK="$disk" ${PC_KEYS[@]+"${PC_KEYS[@]}"} "$@" >"$PC_DIR/run-bg.out" 2>&1; then
        harness "$PC_NAME: run-bg would not start $disk: $(tail -1 "$PC_DIR/run-bg.out")"
        return 1
    fi
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S" || return 1
    wait_for_quiet "$BOOT_BURST_QUIET_S"
}

pc_gone() {    # pc_gone PID — the machine is gone, and so are its sockets
    wait_until "$WAIT_QUIET_S" "QEMU $1 to be gone" "! kill -0 $1 2>/dev/null" || return 1
    rm -f build/qemu.pid build/qemu.mon build/qemu.qmp
}

# pc_base — the volume every cut starts from: this tree's image, booted once
# and switched off by its power button, so the mount's first writes and a
# halt's sync are behind it. Settled, the next boot writes nothing to it.
pc_base() {
    local pid
    pc_clone build/boxos.img "$PC_DIR/base.img" || return 1
    if ! pc_boot "$PC_DIR/base.img"; then
        bad "$PC_NAME: the base never reached a shell"; return 1
    fi
    pid=$(cat build/qemu.pid)
    mon_raw system_powerdown && pc_gone "$pid" || {
        bad "$PC_NAME: the base would not switch off by its power button"; return 1; }
    cp build/serial.log "$PC_DIR/base.serial.log"
    PC_BASE_UUID=$(deed_uuid "$PC_DIR/base.img")
    python3 "$VR" "$PC_DIR/base.img" check --json >"$PC_DIR/base.check.json"
    [ $? = 0 ] || { bad "$PC_NAME: the base is not consistent before anything was cut"; return 1; }
    echo "base: volume $PC_BASE_UUID, sha256 $(shasum -a 256 "$PC_DIR/base.img" | cut -c1-16)…, booted once and switched off by its power button; check says consistent; plate: $(head -1 "$SCRATCH/plate.txt" | sed 's/^\[buildplate\] [^:]*: //')" \
        >>"$PC_DIR/facts.txt"
}

# pc_logged [KEYS...] — a clone of the base, booted with every write on the
# record (and KEYS on the machine besides).
#
# The machine is asked which disk it has: run.img is a clone of the base, so
# nothing read off either image can tell them apart, and a boot of any other
# disk would leave run.img an untouched clone that still looks right.
pc_logged() {
    local pid
    pc_clone "$PC_DIR/base.img" "$PC_DIR/run.img" || return 1
    if ! pc_boot "$PC_DIR/run.img" POWERCUT=on POWERCUT_LOG="$PC_DIR/run.log" "$@"; then
        bad "$PC_NAME: the recorded boot never reached a shell"; return 1
    fi
    pid=$(cat build/qemu.pid 2>/dev/null)
    case "$(ps -o command= -p "$pid" 2>/dev/null)" in
        *"filename=$PC_DIR/run.img,"*"filename=$PC_DIR/run.log,"*) ;;
        *) harness "$PC_NAME: the recorded machine (pid $pid) was not started on $PC_DIR/run.img with its record at $PC_DIR/run.log"
           return 1 ;;
    esac
    PC_PATH=$(stand_path build/serial.log "$PC_CORES")
}

# pc_step NAME CMD ACK_ERE [DURABLE [noquiet]] — ask, wait for the answer, and
# note where the record stood before, at the answer and once it went quiet.
# DURABLE is NAME:RECORD:FROM:TO for a step whose answer promises durability
# (TO 0: as many records as the record holds writes).
# noquiet: the step leaves a writer behind, and the record never goes quiet —
# which the judge is told, because such a record is one run's timing.
pc_step() {
    local name=$1 cmd=$2 ack=$3 durable=${4:-} quiet="" n0 nack nq mark line
    n0=$(pc_record_count "$PC_DIR/run.log")
    mark=$(wc -l < build/serial.log)
    type_line "$cmd" || { bad "$PC_NAME: the machine never took '$cmd'"; return 1; }
    if ! wait_for_line -E build/serial.log "$ack" "$WAIT_QUIET_S" $((mark + 1)); then
        bad "$PC_NAME: '$cmd' never answered /$ack/"; return 1
    fi
    nack=$(pc_record_count "$PC_DIR/run.log")
    line=$(tail -n +$((mark + 1)) build/serial.log | tr -d '\r' | grep -aE "$ack" | head -1)
    if [ "${5:-}" = noquiet ]; then
        nq=$nack; quiet=--noquiet
    else
        wait_for_quiet "$PC_RECORD_QUIET_S" "$PC_DIR/run.log"
        nq=$(pc_record_count "$PC_DIR/run.log")
    fi
    python3 "$PC" step "$PC_DIR/steps.jsonl" --step "$name" --cmd "$cmd" --before "$n0" \
        --ack "$nack" --quiet "$nq" --line "$line" ${durable:+--durable "$durable"} $quiet
}

# pc_cut_power — kill -9 by pid, and the fact that it is gone. Never
# run-stop: SIGTERM lets QEMU run on and finish what it was doing.
pc_cut_power() {
    local pid
    pid=$(cat build/qemu.pid 2>/dev/null)
    case "$pid" in ''|*[!0-9]*) harness "$PC_NAME: no QEMU to cut the power of"; return 1 ;; esac
    kill -9 "$pid"
    pc_gone "$pid" || { harness "$PC_NAME: QEMU $pid outlived kill -9"; return 1; }
    cp build/serial.log "$PC_DIR/run.serial.log"
}

# pc_halt_orderly ITEM — the power button, while whatever the scenario started
# still runs. The machine must switch itself off, and "[HALT] Storage sync
# complete" is its acknowledgement for ITEM (NAME:RECORD). A sentence the
# kernel's rodata holds proves nothing by itself, so it is paired with a
# number: the record's entry count read the moment the sentence is seen —
# the sync's flush must stand at or before that entry, since the kernel says
# it only once the flush has come back. The recorded boot of these scenarios
# runs with -no-shutdown, so at its power-off QEMU stops instead of exiting:
# nothing QEMU does of its own at exit, such as a flush of its own, can stand
# in the record for the guest's, and the power is then cut as in every other
# scenario.
pc_halt_orderly() {
    local n0 nack noff mark floor
    n0=$(pc_record_count "$PC_DIR/run.log")
    mark=$(wc -l < build/serial.log)
    mon_raw system_powerdown || return 1
    if wait_for_line build/serial.log '^\[HALT\] Storage sync complete' "$WAIT_QUIET_S" $((mark + 1)); then
        nack=$(pc_record_count "$PC_DIR/run.log")
    fi
    if ! wait_until "$WAIT_QUIET_S" "the machine to switch itself off" pc_switched_off; then
        bad "$PC_NAME: the machine cannot be switched off while a program writes — the power button went at record entry $n0 and the machine still runs ($(tail -n +$((mark + 1)) build/serial.log | tr -d '\r' | grep -a '^\[HALT\]' | tail -1))"
        pc_cut_power
        return 1
    fi
    noff=$(pc_record_count "$PC_DIR/run.log")
    pc_cut_power || return 1
    if [ -z "${nack:-}" ]; then
        bad "$PC_NAME: the machine switched off without '[HALT] Storage sync complete' ($(tail -n +$((mark + 1)) "$PC_DIR/run.serial.log" | tr -d '\r' | grep -a '^\[HALT\]' | tail -1))"
        return 1
    fi
    # The least a whole record may say: the last seq churn reported before the
    # halt stopped every program.
    floor=$(tr -d '\r' <"$PC_DIR/run.serial.log" | sed '/^\[HALT\] Terminating all processes/q' \
            | grep -aoE "churn: ${1%%:*} fid=[0-9]+ wrote seq=[0-9]+" | tail -1 | sed 's/.*seq=//')
    floor=${floor:-1}
    python3 "$PC" step "$PC_DIR/steps.jsonl" --step halt --cmd system_powerdown --before "$n0" \
        --ack "$nack" --quiet "$noff" --durable "$1:$floor:0" \
        --line "[HALT] Storage sync complete seen at record entry $nack; the machine off at entry $noff; churn had reported seq=$floor"
}

pc_switched_off() {   # QEMU, stopped by -no-shutdown, says so
    ./tools/qemu-input.sh raw "info status" 2>/dev/null | grep -q "shutdown"
}

# pc_record — the record held against the disk it describes, then listed,
# planned and every distinct medium in the plan judged by volume-read.
pc_record() {
    local said
    if ! said=$(python3 "$VR" "$PC_DIR/base.img" cut "$PC_DIR/run.log" --against "$PC_DIR/run.img" 2>&1); then
        harness "$PC_NAME: the record does not account for the disk QEMU left — $said"
        return 1
    fi
    ok "$PC_NAME: the record accounts for the disk QEMU left — $said"
    echo "machine: ${PC_KEYS[*]:-plain BIOS}, $PC_CORES core(s), a userspace write goes by $PC_PATH" >>"$PC_DIR/facts.txt"
    python3 "$VR" "$PC_DIR/base.img" cut "$PC_DIR/run.log" --list --json >"$PC_DIR/list.json" || {
        harness "$PC_NAME: volume-read could not list the record"; return 1; }
    if [ "$PC_FORMAT" = v2 ]; then
        # A book's every block is an owner's, and its record names each write
        # by its sectors' heads or the stitch after it (blklog_book.py).
        said=$(python3 -c 'import json, sys
print(sum(t["kind"] == "unowned" for w in json.load(open(sys.argv[1]))["writes"] for t in w["touches"]))' \
            "$PC_DIR/list.json")
        [ "$said" = 0 ]
        chk $? "$PC_NAME: every write of the record on the book is named — $said unowned"
    fi
    PC_SEED=$(shasum -a 256 "$PC_DIR/run.log" | cut -c1-16)
    python3 "$PC" plan "$PC_DIR/list.json" --seed "$PC_SEED" --all-max "$PC_SUBSET_ALL_MAX" \
        --sample "$PC_SAMPLE_SUBSETS" --tear-all-max "$PC_TEAR_ALL_MAX" >"$PC_DIR/plan.json" || {
        harness "$PC_NAME: the plan could not be made"; return 1; }
    python3 "$PC" checks "$PC_DIR/plan.json" "$PC_DIR/base.img" "$PC_DIR/run.log" \
        "$PC_DIR/base.check.json" --jobs "$PC_CHECK_JOBS" >"$PC_DIR/cuts.jsonl" || {
        harness "$PC_NAME: the checks could not be run"; return 1; }
}

# pc_ask CMD ACK_ERE — one question to a booted cut; its answer, or the
# silence, is read by the judge from the boot's log.
pc_ask() {
    local mark
    mark=$(wc -l < build/serial.log)
    type_line "$1" || return 1
    wait_for_line -E build/serial.log "$2" "$WAIT_QUIET_S" $((mark + 1)) || return 1
    wait_for_quiet "$BOOT_BURST_QUIET_S"
}

# pc_boots PICKS [ARGS...] — the media the policy names (tools/powercut.py
# boots, ARGS handed to it), each built, booted, asked the scenario's question
# (PC_ASK_CMD/PC_ASK_ACK — none when PC_ASK_CMD is empty — and on the final
# state PC_FINAL_CMD/PC_FINAL_ACK too), and its log kept.
pc_boots() {
    local id epoch keep tear why img="$PC_DIR/boot.img"
    python3 "$PC" boots "$PC_DIR" --picks "$1" --sample "$PC_SAMPLE_BOOTS" --seed "$PC_SEED" "${@:2}" \
        >"$PC_DIR/boots.txt" || { harness "$PC_NAME: no boot could be chosen"; return 1; }
    while IFS="$TAB" read -r id epoch keep tear why <&3; do
        if [ "$tear" = - ]; then
            python3 "$VR" "$PC_DIR/base.img" cut "$PC_DIR/run.log" --epoch "$epoch" --keep "$keep" \
                --out "$img" >/dev/null
        else
            python3 "$VR" "$PC_DIR/base.img" cut "$PC_DIR/run.log" --epoch "$epoch" --keep "$keep" \
                --tear "$tear" --out "$img" >/dev/null
        fi || { harness "$PC_NAME: $id could not be built to boot"; continue; }
        # A volume the kernel refused holds no program to ask with; the
        # refusal itself is what the judge weighs.
        if pc_boot "$img" && ! grep -aq '\[TagFS\] this volume is NOT mounted' build/serial.log; then
            [ -n "$PC_ASK_CMD" ] && pc_ask "$PC_ASK_CMD" "$PC_ASK_ACK"
            case "$why" in *"final state"*)
                [ -n "${PC_FINAL_CMD:-}" ] && pc_ask "$PC_FINAL_CMD" "$PC_FINAL_ACK" ;;
            esac
        fi
        make run-stop >/dev/null 2>&1
        cp build/serial.log "$PC_DIR/boots/$id.serial.log"
        rm -f "$img"
    done 3<"$PC_DIR/boots.txt"
}

# pc_caveats NAME — the caveats a scenario's verdicts may be held under:
# KEY=ROW, ROW a row of stand_caveat.sh's one table. Every census code the
# scenario was measured showing is its own (scenario, code) row (the owner,
# 2026-09-27); a code no row names is a FAIL until the owner assigns it a step.
pc_caveats() {
    local base code codes=""
    case "$1" in
        noanchor|noanchormut) base=noanchor; echo "verify=noanchor"; codes="ORPHAN" ;;
        hog)                  base=hog; echo "verify=hog"; codes="ORPHAN" ;;
        nextfid|nextfidmut|poolmut|flushmut)
            base=nextfid; echo "reissue=nextfid"
            codes="FID_PAST_NEXT FILE_LOST MISSING ORPHAN SPARE_UNNAMED TAG_ABSENT UNNAMED_RECORD" ;;
        halt)
            base=halt
            codes="CHAIN_REFUSED FID_PAST_NEXT FILE_LOST MISSING ORPHAN SPARE_UNNAMED TAG_ABSENT UNNAMED_RECORD" ;;
        race)
            base=race; echo "durable:_qt_churn@race=race:durable"
            codes="CHAIN_REFUSED FID_PAST_NEXT FILE_LOST MISSING ORPHAN RECORD_SHAPE SPARE_UNNAMED TAG_ABSENT TAG_DUPLICATE UNNAMED_RECORD" ;;
        settlecut) base=settlecut; codes="FID_PAST_NEXT ORPHAN SPARE_UNNAMED" ;;
        chaingrow)
            base=chaingrow
            codes="prefix:CHAIN_REFUSED:registry prefix:CHAIN_REFUSED:table prefix:FID_PAST_NEXT prefix:ORPHAN
                   prefix:SPARE_UNNAMED subset:CHAIN_REFUSED:registry subset:CHAIN_REFUSED:table
                   subset:FID_PAST_NEXT subset:FILE_LOST subset:MISSING subset:ORPHAN subset:SPARE_UNNAMED
                   subset:TAG_ABSENT" ;;
    esac
    for code in $codes; do echo "code:$code=$base:$code"; done
}

# pc_judge ARGS... — every oracle (tools/powercut.py judge), into
# $PC_DIR/verdicts.tsv; the report printed and kept.
pc_judge() {
    local c args=()
    for c in $(pc_caveats "$PC_NAME"); do args+=(--caveat "$c"); done
    echo "took $(( $(date +%s) - PC_T0 )) s to judge; the report is $PC_DIR/report.txt" >>"$PC_DIR/facts.txt"
    python3 "$PC" judge "$PC_DIR" --path "$PC_PATH" ${args[@]+"${args[@]}"} "$@" >"$PC_DIR/verdicts.tsv" || {
        harness "$PC_NAME: the judge could not read this run"; return 1; }
    sed '$d' "$PC_DIR/report.txt" | sed 's/^/  | /'
}

# pc_say — the verdicts in stand.sh's words, and a copy of them in the report.
# A note is no verdict: a caveated defect a census could not show gone, said
# in the words the waits use for what is not a count.
pc_say() {
    local key kind name seen what
    while IFS="$TAB" read -r key kind name seen what; do
        case "$kind" in
            caveat)  caveat_judge "$name" "$seen" "$PC_NAME $key: $what" ;;
            harness) harness "$PC_NAME $key: $what" ;;
            note)    echo "  ($PC_NAME $key, caveat $name: $what)" ;;
            *) if [ "$seen" = behaves ]; then ok "$PC_NAME $key: $what"; else bad "$PC_NAME $key: $what"; fi ;;
        esac
    done <"$PC_DIR/verdicts.tsv"
    { echo "verdicts:"; sed 's/^/  /' "$PC_DIR/verdicts.tsv"; } >>"$PC_DIR/report.txt"
}

# pc_expect KEY SEEN WHY — a *mut scenario's one question: did the oracle KEY
# answer SEEN once the fix or the fault was put in?
pc_expect() {
    local got
    got=$(awk -F"$TAB" -v k="$1" '$1 == k { print $4 "\t" $5 }' "$PC_DIR/verdicts.tsv" | head -1)
    [ "${got%%$TAB*}" = "$2" ]
    chk $? "$PC_NAME: $3 — $1 answered ${got%%$TAB*}${got:+: ${got#*$TAB}}"
}

# ---------------------------------------------------------------------------
# The scenarios
# ---------------------------------------------------------------------------

pc_noanchor() {
    pc_base && pc_logged || return 1
    pc_step persist "persist write" \
        '\[PERSIST\] WROTE fid=[0-9]+ seq=[0-9]+ magic=0x42504552 checksum=0x[0-9a-f]+' || return 1
    sleep $((PC_CEILING_S + PC_D_S))
    pc_cut_power && pc_record || return 1
    PC_ASK_CMD="persist verify"; PC_ASK_ACK='\[PERSIST\] (PASS|FAIL)'; PC_FINAL_CMD=""
    pc_boots final
    pc_judge --verify persist --waited $((PC_CEILING_S + PC_D_S))
}

pc_nextfid() {
    pc_base && pc_logged || return 1
    pc_step anchor_test anchor_test '\[ANCHOR\] WROTE\+ANCHORED fid=[0-9]+\.' "_anchor_test:anchor:1:1" \
        || return 1
    pc_cut_power && pc_record || return 1
    PC_ASK_CMD="anchor_test"; PC_ASK_ACK='\[ANCHOR\] (PASS|WROTE|FAIL|create rc|fwrite rc|anchor rc|read rc)'
    PC_FINAL_CMD=""
    pc_boots prefixes:anchor_test
    pc_judge --verify reissue:anchor_test
}

pc_hog() {
    pc_base && pc_logged || return 1
    pc_step settle "quilltest settle _qt_settle" \
        '\[QUILL\] settle: PENDING — wrote _qt_settle fid=[0-9]+ seq=[0-9]+ bytes=16' || return 1
    sleep $((PC_CEILING_S + PC_D_S))
    pc_cut_power && pc_record || return 1
    PC_ASK_CMD="quilltest verify _qt_settle"; PC_ASK_ACK='\[QUILL\] verify: (PASS|FAIL)'
    PC_FINAL_CMD="quilltest hogged anchor_test"; PC_FINAL_ACK='\[QUILL\] hogged: (PASS|FAIL)'
    pc_boots final
    pc_judge --verify quill-exact:_qt_settle:settle --waited $((PC_CEILING_S + PC_D_S)) --hogged
}

pc_halt() {
    pc_base && pc_logged "$(pc_extra -no-shutdown)" || return 1
    pc_step churn "quilltest churn _qt_churn" '\[QUILL\] churn: _qt_churn fid=[0-9]+ starts at seq=[0-9]+' \
        "" noquiet || return 1
    pc_halt_orderly _qt_churn:quill && pc_record || return 1
    PC_ASK_CMD="quilltest verify _qt_churn"; PC_ASK_ACK='\[QUILL\] verify: (PASS|FAIL)'; PC_FINAL_CMD=""
    pc_boots final
    pc_judge --verify quill-floor:_qt_churn:halt --final-consistent
}

pc_race() {
    pc_base && pc_logged "$(pc_extra -no-shutdown)" || return 1
    # Every verdict race can give is its answer, the ones before the anchors
    # started too: "never answered" is kept for a machine that said nothing.
    pc_step race "quilltest race _qt_churn 200" \
        '\[QUILL\] race: (PASS|FAIL) — ' "_qt_churn:quill:1:0" noquiet \
        || return 1
    grep -aq '\[QUILL\] race: PASS — anchored 200 of 200' build/serial.log
    chk $? "$PC_NAME: $(grep -aoE '\[QUILL\] race: (PASS|FAIL) — [^.]*' build/serial.log | head -1)"
    pc_halt_orderly _qt_churn:quill && pc_record || return 1
    PC_ASK_CMD="quilltest verify _qt_churn"; PC_ASK_ACK='\[QUILL\] verify: (PASS|FAIL)'; PC_FINAL_CMD=""
    pc_boots final
    pc_judge --verify quill-floor:_qt_churn:halt --final-consistent
}

# chaingrow — MP-D5. The file table and the tag registry took a new block by
# writing their last block's link to it before the block itself, the one in
# its sync's flush and the other as the create of a new tag flushes it, until
# step 1.1 grew both onto Spares — stamped, and the stamp behind a kept flush
# of its own, before any link names them — which splits the sync's epochs. The
# base is brought to both edges with quilltest crowd — the table one create
# short of the row its blocks run out at, the registry's last block room for
# PC_CROWD_ROOM more of crowd's tags — so the recorded run's few creates and
# its anchor grow both chains, and every prefix of it is built.
PC_CROWD_NAME_BYTES=15    # "crowd" and ten digits: quilltest_powercut.c QuillCrowdName
PC_CROWD_ROOM=1
# Rounds of crowd the base is given to stand at both edges (pc_edges). Each
# round lands there unless the machine interned a tag of its own during it,
# and each such tag is interned once — two were measured, a console lane's and
# `anchor` — so a machine still short after this many is one that interns a
# new tag every round, and that is said, not waited out.
PC_CROWD_ROUNDS=${PC_CROWD_ROUNDS:-4}

# pc_crowd CMD — one quilltest crowd on a machine that is not recorded, its
# PASS required and said in the facts.
pc_crowd() {
    local mark said
    mark=$(wc -l < build/serial.log)
    type_line "$1" || { bad "$PC_NAME: the machine never took '$1'"; return 1; }
    if ! wait_for_line -E build/serial.log '\[QUILL\] crowd: (PASS|FAIL) — ' "$WAIT_QUIET_S" $((mark + 1)); then
        bad "$PC_NAME: '$1' never answered"; return 1
    fi
    said=$(tail -n +$((mark + 1)) build/serial.log | tr -d '\r' | grep -aoE '\[QUILL\] crowd: (PASS|FAIL) — .*' | head -1)
    case "$said" in
        *"crowd: PASS"*) echo "edges: \`$1\` — $said" >>"$PC_DIR/facts.txt" ;;
        *) bad "$PC_NAME: $said"; return 1 ;;
    esac
    wait_for_quiet "$BOOT_BURST_QUIET_S"
}

# pc_crowded ROUND CMD... — the base booted, every CMD (a quilltest crowd) run
# on it, switched off by its power button and judged again: consistent, or
# nothing after it means anything.
pc_crowded() {
    local round=$1 cmd pid
    shift
    pc_boot "$PC_DIR/base.img" || {
        bad "$PC_NAME: the base never reached a shell for crowd's round $round"; return 1; }
    for cmd in "$@"; do pc_crowd "$cmd" || return 1; done
    pid=$(cat build/qemu.pid)
    mon_raw system_powerdown && pc_gone "$pid" || {
        bad "$PC_NAME: the base would not switch off by its power button after crowd's round $round"; return 1; }
    cp build/serial.log "$PC_DIR/crowd$round.serial.log"
    python3 "$VR" "$PC_DIR/base.img" check --json >"$PC_DIR/base.check.json"
    [ $? = 0 ] || { bad "$PC_NAME: the base is not consistent after crowd's round $round"; return 1; }
}

# pc_edges — the base brought to both edges and judged again. Sets CG_FROM and
# CG_RUN: the first number and the count of the recorded run's files.
#
# ‼ MEASURED, NOT COUNTED, BECAUSE THE MACHINE INTERNS TAGS OF ITS OWN. A count
# of crowd's files says where the registry should end up, and the machine
# adds what it likes on the way: the first line crowd printed (at its 50th
# file) put its console lane's tag `console:1` there, and `anchor` came after
# the last of crowd's records — 27 bytes no file asked for, and the registry's
# last block came out 193 records from its edge instead of one. So each round
# is planned from the volume as check reads it (tools/powercut.py crowd), run,
# switched off and read again, until the volume stands at both edges — or
# PC_CROWD_ROUNDS rounds have not got it there, which is a machine this
# harness cannot bring to the edges, said as such.
pc_edges() {
    local plan verdict fill again round=0 names=0 cmds
    while :; do
        plan=$(python3 "$PC" crowd "$PC_DIR/base.check.json" --name-bytes "$PC_CROWD_NAME_BYTES" \
                   --room "$PC_CROWD_ROOM" --names "$names" 2>&1) || {
            harness "$PC_NAME: $plan"; return 1; }
        verdict=${plan%%"$TAB"*}; plan=${plan#*"$TAB"}
        if [ "$verdict" = edges ]; then
            CG_FROM=$names
            CG_RUN=${plan%%"$TAB"*}
            echo "edges: after $round round(s) of crowd, switched off by its power button, check says consistent; ${plan#*"$TAB"}" \
                >>"$PC_DIR/facts.txt"
            return 0
        fi
        fill=${plan%%"$TAB"*}; plan=${plan#*"$TAB"}
        again=${plan%%"$TAB"*}; plan=${plan#*"$TAB"}
        if [ "$round" -ge "$PC_CROWD_ROUNDS" ]; then
            harness "$PC_NAME: the volume is not at both edges after $round round(s) of crowd: $plan"
            return 1
        fi
        round=$((round + 1))
        echo "edges: round $round — the base had $plan" >>"$PC_DIR/facts.txt"
        cmds=()
        [ "$fill" -gt 0 ] && cmds+=("quilltest crowd $fill $names")
        [ "$again" -gt 0 ] && cmds+=("quilltest crowd $again 0")
        pc_crowded "$round" ${cmds[@]+"${cmds[@]}"} || return 1
        names=$((names + fill))
    done
}

pc_chaingrow() {
    local last
    pc_base && pc_edges && pc_logged || return 1
    last=$(printf 'crowd%010u' $((CG_FROM + CG_RUN - 1)))
    pc_step crowd "quilltest crowd $CG_RUN $CG_FROM" '\[QUILL\] crowd: (PASS|FAIL) — ' "$last:empty:1:1" \
        || return 1
    grep -aq '\[QUILL\] crowd: PASS — made '"$CG_RUN"' files' build/serial.log
    chk $? "$PC_NAME: $(tr -d '\r' <build/serial.log | grep -aoE '\[QUILL\] crowd: (PASS|FAIL) — [^;]*' | tail -1)"
    pc_cut_power && pc_record || return 1
    PC_ASK_CMD=""; PC_FINAL_CMD=""
    pc_boots final --by-kind
    pc_judge --by-kind --verify grown
}

# pc_lockrank — every [LockRank] line of this scenario's boots judged, each
# place once however many of the boots named it (stand_caveat.sh).
pc_lockrank() {
    local f logs=""
    for f in "$PC_DIR"/*.serial.log "$PC_DIR"/boots/*.serial.log; do
        [ -f "$f" ] && logs="$logs $f"
    done
    [ -z "$logs" ] || lockrank_judge "$logs" "$PC_NAME"
}

run_scenario() {   # run_scenario NAME KEYS... — build, run, judge, say
    local name=$1
    pc_begin "$@"
    pc_build 0 || return
    if "pc_$name"; then
        pc_say
    else
        bad "$PC_NAME: the scenario stopped before its record could be judged (logs in $PC_DIR)"
    fi
    make run-stop >/dev/null 2>&1
    pc_lockrank
}

# settlecut — the second oracle. A kernel built SETTLECUT=<point> stops the
# whole machine at that point of the cycle that settles the boot's first
# anchor (src/kernel/tagfs/settle_cut), which leaves the medium a power cut
# there would leave. Its answer must be PowerCut's answer for the same point
# of the same cycle.
#
# The two sentences of settle_cut.c the scenario waits for, said once here for
# sc_boot_cut and for the selftest that holds them against that file's text.
SC_SETTLE_CUT_C=src/kernel/tagfs/settle_cut/settle_cut.c
SC_ARMED_ERE='^\[SETTLECUT\] armed: the first cycle to reach point [0-9]+ of [0-9]+, .*, with the change of the first anchor after this line in its snapshot stops this machine there$'
SC_CUT_ERE='\[SETTLECUT\] the power went at point [0-9]+ of [0-9]+, .*, in cycle [0-9]+ of this boot, whose snapshot reaches Ink [0-9]+ of the anchor.s [0-9]+,'

# sc_says — every sentence settle_cut.c prints, rendered as kprintf renders
# it: "armed" or "cut", a tab and the line, each %u and %llu a number and a %s
# the ledger point's words (SettleCutWhere). The literals of one kprintf are
# joined as the compiler joins them.
sc_says() {
    python3 - "$SC_SETTLE_CUT_C" <<'EOF'
import re, sys
src = open(sys.argv[1]).read()
where = dict(re.findall(r'\[SETTLE_CUT_POINT_(\w+)\]\s*=\s*"([^"]*)"', src))
for call in re.findall(r'kprintf\(((?:\s*"(?:[^"\\]|\\.)*")+)', src):
    line = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', call)).replace("\\n", "")
    line = re.sub(r"%(?:ll)?u", "6", line).replace("%s", where.get("LEDGER", "?"))
    for which, head in (("armed", "[SETTLECUT] armed: "), ("cut", "[SETTLECUT] the power went ")):
        if line.startswith(head):
            print("%s\t%s" % (which, line))
EOF
}

# sc_boot_cut POINT — the POINT kernel's image, settled as PowerCut's base is:
# booted once and switched off by its power button, its mount's writes and the
# shutdown's cycle on the disk — no anchor was taken, so no cycle of that boot
# stops. Then booted again, anchor_test, and the machine stops itself inside
# the cycle that settles the anchor.
sc_boot_cut() {
    local pid mark img="$PC_DIR/$1.img"
    if ! build_with_key SETTLECUT="$1" "$(strict_key ${PC_KEYS[@]+"${PC_KEYS[@]}"})"; then
        bad "settlecut: SETTLECUT=$1 does not build: $(tail -3 "$SCRATCH/build.log" | tr '\n' ' ')"; return 1
    fi
    pc_clone build/boxos.img "$img"
    pc_boot "$img" || { bad "settlecut: the SETTLECUT=$1 kernel never reached a shell"; return 1; }
    pid=$(cat build/qemu.pid)
    mon_raw system_powerdown && pc_gone "$pid" || {
        bad "settlecut: the SETTLECUT=$1 kernel would not switch off by its power button"; return 1; }
    pc_boot "$img" || { bad "settlecut: the settled SETTLECUT=$1 volume never reached a shell"; return 1; }
    tr -d '\r' <build/serial.log | grep -aqE "$SC_ARMED_ERE"
    chk $? "settlecut: $1 — $(grep -aoE '\[SETTLECUT\] armed: [^,]*' build/serial.log | head -1)"
    mark=$(wc -l < build/serial.log)
    type_line anchor_test || { bad "settlecut: the machine never took anchor_test"; return 1; }
    if ! wait_for_line -E build/serial.log "$SC_CUT_ERE" "$WAIT_QUIET_S" $((mark + 1)); then
        bad "settlecut: SETTLECUT=$1 never stopped the machine after anchor_test"; return 1
    fi
    ok "settlecut: $1 — $(tail -n +$((mark + 1)) build/serial.log | tr -d '\r' | grep -aoE "$SC_CUT_ERE" | sed 's/,$//')"
    pid=$(cat build/qemu.pid); kill -9 "$pid"; pc_gone "$pid"
    cp build/serial.log "$PC_DIR/$1.serial.log"
    # 0 consistent and 1 inconsistent are both answers — the ledger point is
    # inconsistent on HEAD — and the judge weighs them; 2 is a volume that
    # would not read.
    python3 "$VR" "$img" check --json >"$PC_DIR/$1.check.json"
    [ $? -le 1 ] || { harness "settlecut: volume-read could not read the SETTLECUT=$1 volume"; return 1; }
}

pc_settlecut() {
    local said again="$PC_DIR/done.again.serial.log"
    pc_base && pc_logged || return 1
    pc_step anchor_test anchor_test '\[ANCHOR\] WROTE\+ANCHORED fid=[0-9]+\.' "_anchor_test:anchor:1:1" || return 1
    pc_cut_power || return 1
    said=$(python3 "$VR" "$PC_DIR/base.img" cut "$PC_DIR/run.log" --against "$PC_DIR/run.img" 2>&1) || {
        harness "settlecut: the record does not account for the disk QEMU left — $said"; return 1; }
    python3 "$VR" "$PC_DIR/base.img" cut "$PC_DIR/run.log" --list --json >"$PC_DIR/list.json"
    sc_boot_cut ledger && sc_boot_cut done || return 1
    pc_boot "$PC_DIR/done.img" && pc_ask anchor_test '\[ANCHOR\] (PASS|WROTE|FAIL|read rc)'
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$again"
    grep -aqE '\[ANCHOR\] PASS — anchored write survived hard crash \(seq=1\)' "$again"
    chk $? "settlecut: done — the file its sync finished survives a reboot: $(grep -aoE '\[ANCHOR\] [^.(]*' "$again" | head -1)"
    buildkey_restore || bad "settlecut: the plain kernel could not be built back"
    local c args=()
    for c in $(pc_caveats settlecut); do args+=(--caveat "$c"); done
    python3 "$PC" settlecut "$PC_DIR" --path "$PC_PATH" ${args[@]+"${args[@]}"} >"$PC_DIR/verdicts.tsv" || {
        harness "settlecut: the judge could not read this run"; return 1; }
}

run_settlecut() {
    pc_begin settlecut "$@"
    pc_build 0 || return
    if pc_settlecut; then
        pc_say
    else
        bad "settlecut: the scenario stopped before it could be judged (logs in $PC_DIR)"
    fi
    make run-stop >/dev/null 2>&1
}

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

# selftest — the gate judging itself: the planner's arithmetic, settlecut's
# patterns against settle_cut.c, then one real record that the self-check must
# accept and, forged by one byte, refuse.
run_selftest() {
    local said says which line ere
    pc_begin selftest
    python3 "$PC" selftest >"$PC_DIR/planner.txt" 2>&1
    chk $? "selftest: the planner's arithmetic — $(tail -1 "$PC_DIR/planner.txt")"
    grep -v '^  ok' "$PC_DIR/planner.txt" | grep -v '^selftest:' | sed 's/^/  /'
    # settlecut waits for two of settle_cut.c's sentences: its patterns held
    # against that file's own text, so a word changed on either side is a FAIL
    # here, and not a scenario that waits a minute for a line nobody prints.
    says=$(sc_says)
    [ "$(printf '%s\n' "$says" | cut -f1 | sort | tr '\n' ' ')" = "armed cut " ]
    chk $? "selftest: $SC_SETTLE_CUT_C prints one sentence of each kind settlecut waits for (found: $(printf '%s\n' "$says" | cut -f1 | tr '\n' ' '))"
    while IFS="$TAB" read -r which line; do
        case "$which" in armed) ere=$SC_ARMED_ERE ;; cut) ere=$SC_CUT_ERE ;; *) continue ;; esac
        printf '%s\n' "$line" | grep -qE "$ere"
        chk $? "selftest: settlecut's $which pattern matches the sentence $SC_SETTLE_CUT_C prints — $line"
    done <<SAYS_EOF
$says
SAYS_EOF
    pc_build 0 || return
    pc_base && pc_logged && pc_step anchor_test anchor_test '\[ANCHOR\] WROTE\+ANCHORED fid=[0-9]+\.' && pc_cut_power \
        || { bad "selftest: no record to judge (logs in $PC_DIR)"; return; }
    said=$(python3 "$VR" "$PC_DIR/base.img" cut "$PC_DIR/run.log" --against "$PC_DIR/run.img" 2>&1)
    chk $? "selftest: the record accounts for the disk it describes — $said"
    python3 "$VR" "$PC_DIR/base.img" cut "$PC_DIR/run.log" --list --json >"$PC_DIR/list.json"
    local forged
    forged=$(python3 "$PC" forge "$PC_DIR/list.json" "$PC_DIR/run.log" "$PC_DIR/forged.log") || {
        bad "selftest: the record holds no write to forge"; return; }
    said=$(python3 "$VR" "$PC_DIR/base.img" cut "$PC_DIR/forged.log" --against "$PC_DIR/run.img" 2>&1)
    [ $? = 1 ]
    chk $? "selftest: $forged flipped, the self-check refuses the record — $said"
    make run-stop >/dev/null 2>&1
    pc_lockrank
}

pc_preflight
# VOLUME_FORMAT=v2: every image of the run is a book (the Makefile's key, held
# for every make of the run by stand.sh's make through STAND_FORMAT), and the
# record is judged by the book's census and blklog_book.py — a step committed
# by its stitch (powercut.py COMMIT_KINDS), the slot's counts in the Ledger's
# place. A scenario that stands on format v1's own machinery has no book to
# ask and is refused by name: settlecut's points of the v1 cycle, chaingrow's
# registry and table chains, and the mutations of v1's mount (nextfidmut) and
# of its cycle's pool and Ledger (poolmut, flushmut).
PC_FORMAT=${VOLUME_FORMAT:-v1}
case "$PC_FORMAT" in v1|v2) ;; *) echo "powercut: VOLUME_FORMAT=$PC_FORMAT names no format"; exit 2 ;; esac
[ "$PC_FORMAT" = v2 ] && STAND_FORMAT=v2
case "$PC_FORMAT:${1:-}" in
    v2:settlecut|v2:chaingrow|v2:nextfidmut|v2:poolmut|v2:flushmut)
        echo "powercut: $1 stands on format v1's own machinery and is no scenario of a book (VOLUME_FORMAT=v2)"
        exit 2 ;;
    v2:all)
        run_selftest
        for keys in "" "STRICT=on CORES=4 MEM=4G" "UEFI=on STRICT=on CORES=4 MEM=4G"; do
            # shellcheck disable=SC2086
            run_scenario noanchor $keys; run_scenario nextfid $keys
        done
        run_scenario hog; run_scenario hog UEFI=on STRICT=on
        for keys in "STRICT=on CORES=4 MEM=4G" "UEFI=on STRICT=on CORES=16 MEM=8G"; do
            # shellcheck disable=SC2086
            run_scenario halt $keys; run_scenario race $keys
        done
        run_noanchormut
        set -- done ;;
esac
case "${1:-}" in
    done) ;;
    noanchor|nextfid|hog|halt|race|chaingrow)
        run_scenario "$@" ;;
    settlecut)   shift; run_settlecut "$@" ;;
    noanchormut) run_noanchormut ;;
    nextfidmut)  run_nextfidmut ;;
    poolmut)     run_poolmut ;;
    flushmut)    run_flushmut ;;
    selftest)    run_selftest ;;
    all)
        run_selftest
        for keys in "" "STRICT=on CORES=4 MEM=4G" "UEFI=on STRICT=on CORES=4 MEM=4G"; do
            # shellcheck disable=SC2086
            run_scenario noanchor $keys; run_scenario nextfid $keys
        done
        run_scenario hog; run_scenario hog UEFI=on STRICT=on
        for keys in "STRICT=on CORES=4 MEM=4G" "UEFI=on STRICT=on CORES=16 MEM=8G"; do
            # shellcheck disable=SC2086
            run_scenario halt $keys; run_scenario race $keys
        done
        run_settlecut
        run_scenario chaingrow
        run_noanchormut; run_nextfidmut; run_poolmut; run_flushmut ;;
    *)
        echo "usage: [VOLUME_FORMAT=v2] $0 noanchor|nextfid|hog|halt|race|settlecut|chaingrow [MACHINE KEYS...]"
        echo "       [VOLUME_FORMAT=v2] $0 noanchormut|nextfidmut|poolmut|flushmut|selftest|all"
        exit 2 ;;
esac

STAND_FINISHED=1
echo
echo "powercut: $PASS passed, $FAIL failed, $CAVEAT caveat(s) held"
if [ "$CAVEAT" != 0 ]; then
    echo "powercut: $CAVEAT caveat(s) held — each is a known defect reproduced on purpose, and names the step that discharges it"
fi
if [ "$HARNESS" != 0 ]; then
    echo "powercut: $HARNESS harness failure(s) — this run could not talk to the"
    echo "          machine it was judging, so its verdicts are not evidence."
fi
[ "$FAIL" = 0 ] && [ "$HARNESS" = 0 ]
