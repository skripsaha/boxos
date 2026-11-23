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
# record (tools/volume-read.py) and judged by volume-read's census of the
# book, which also predicts what the next mount does; a
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
#               sync is booted and `persist write` asked — the cut that asks
#               bears the anchor's file on its ribbon, and anchor_test would
#               find it and make none: no cut hands out an id its medium
#               already shows.
#   hog         one core: quilltest settle (a write, not anchored, beside a
#               hog); 5+D s later the power goes; `quilltest verify` (step 1.7
#               lifted the caveat). And the design's "an anchor under the hog
#               answers": `quilltest hogged anchor_test` on the booted state.
#   halt        quilltest churn writes for ever; the power button. The machine
#               must switch off, and what churn wrote is durable behind the
#               halt's flush.
#   race        quilltest race: 200 anchors against a churn, then the power
#               button; the halt's oracles.
#   overwrite logger tipin sessions dedup replug machineb spared undonecut
#   engross     the book's own scenarios (powercut_book.sh).
#   noanchormut firstflushmut lastflushmut booknextfidmut stitchmut
#               the fix taken out or the fault put in on purpose: the cadence
#               off (noanchor's verify), a stitch's flushes, the next file id
#               the ribbon bears, a stitch never written — the oracle must say
#               the defect is back (powercut_mut.sh).
#   selftest    the planner's arithmetic, the medium's words read off the
#               kernel's source, and a record forged by one byte that the
#               record's self-check must refuse.
#
# Every image the run builds is a book, its record named by the book's units
# and its cuts judged by the book's census. A book on a fixed medium (the Boardroom's seat, stand_seat_kind) holds a smudge's wipe of
# the settled stitches a spoilt page holds with nothing past them as the
# format's limit, and fails it on a removable one.
#
# A scenario's verdicts are held under its verify row and the rows of a
# book's own oracles alone (stand_caveat.sh); a census code no row names is a
# FAIL until the owner assigns it a step. A caveated code no cut shows is lifted early only where its absence is
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
    local rc strict toll
    strict=$(strict_key ${PC_KEYS[@]+"${PC_KEYS[@]}"})
    toll=$(pc_toll "$PC_NAME")
    pc_make "$strict" "$toll" || return 1
    tools/buildplate verify build/boxos.img --root . >"$SCRATCH/plate.txt" 2>&1; rc=$?
    if [ "$rc" = 3 ]; then
        rm -f build/kernel.bin
        pc_make "$strict" "$toll" || return 1
        tools/buildplate verify build/boxos.img --root . >"$SCRATCH/plate.txt" 2>&1; rc=$?
    fi
    [ "$rc" = "$1" ] && return 0
    # The plate's own two numbers say which: a tree that changed under the
    # build (a file edited while this ran) or an image that is not this tree.
    harness "powercut: the image's plate answers $rc where $1 was due, so nothing says it is this tree: $(grep -a 'tree ' "$SCRATCH/plate.txt" | sed 's/^ *//' | tr '\n' ';')"
    return 1
}

# pc_make STRICT TOLL — the scenario's kernel built: plain but for its STRICT,
# or with the key of its toll as well (pc_toll, powercut_book.sh), which the
# tree records so the plain kernel is built back after (stand.sh
# build_with_key, buildkey_restore).
pc_make() {
    if [ -z "$2" ]; then
        build "$1"
        return 0
    fi
    build_with_key "$1" "$2" && return 0
    harness "$PC_NAME: $2 does not build: $(tail -3 "$SCRATCH/build.log" | tr '\n' ' ')"
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
    PC_MEDIUM="unknown"
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
    wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S" || return 1
    wait_for_quiet "$BOOT_BURST_QUIET_S"
}

pc_gone() {    # pc_gone PID — the machine is gone, and so are its sockets
    wait_until "$WAIT_QUIET_S" "QEMU $1 to be gone" "! kill -0 $1 2>/dev/null" || return 1
    rm -f build/qemu.pid build/qemu.mon build/qemu.qmp
}

# pc_base [CMD] — the volume every cut starts from: this tree's image, booted
# once and switched off by its power button, so the mount's first writes and a
# halt's sync are behind it. Settled, the next boot writes nothing to it. CMD,
# typed on that boot and answered before the power button, is what the
# scenario's machine is to be from then on.
pc_base() {
    local pid
    pc_clone build/boxos.img "$PC_DIR/base.img" || return 1
    if ! pc_boot "$PC_DIR/base.img"; then
        bad "$PC_NAME: the base never reached a shell"; return 1
    fi
    if [ -n "${1:-}" ] && ! { type_line "$1" && wait_for_prompt; }; then
        bad "$PC_NAME: the base never answered '$1'"; return 1
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
    pc_sign build/serial.log
}

# pc_sign LOG — the run signed by the machine that wrote LOG: PC_PATH, the way
# its writes went down (stand_path), and PC_MEDIUM, the medium its
# volume was mounted on (stand_seat_kind), which decides whether a smudge's
# wipe is the format's limit or a FAIL. A path it cannot read stops the
# scenario, said: a verdict about a write is a verdict about one path. A
# book's medium it cannot read is said too, and the run goes on unsigned —
# the limit's row is not given, so a wipe stays a FAIL that names why.
pc_sign() {
    PC_PATH=$(stand_path "$1" "$PC_CORES") ||
        { harness "$PC_NAME: the recorded boot names no way its writes go down — $PC_PATH"; return 1; }
    PC_MEDIUM=$(stand_seat_kind "$1") ||
        harness "$PC_NAME: the recorded boot's medium is not known, so a smudge's wipe is judged a FAIL and never the format's limit — $PC_MEDIUM"
    return 0
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
    echo "machine: ${PC_KEYS[*]:-plain BIOS}, $PC_CORES core(s), a userspace write goes by $PC_PATH, the volume's medium as the Boardroom seats it: $PC_MEDIUM" \
        >>"$PC_DIR/facts.txt"
    python3 "$VR" "$PC_DIR/base.img" cut "$PC_DIR/run.log" --list --json >"$PC_DIR/list.json" || {
        harness "$PC_NAME: volume-read could not list the record"; return 1; }
    # A book's every block is an owner's or free: its record names each write
    # by its heads, the stitch after it or its census (blklog_book.py).
    said=$(python3 -c 'import json, sys; k = [t["kind"] for w in json.load(open(sys.argv[1]))["writes"]
for t in w["touches"]]; print(k.count("unowned"), k.count("free"))' "$PC_DIR/list.json")
    [ "${said% *}" = 0 ]
    chk $? "$PC_NAME: every write of the record on the book is named, or data on ground free as of its epoch (§3.4 L-8) — ${said% *} unowned, ${said#* } free"
    PC_SEED=$(shasum -a 256 "$PC_DIR/run.log" | cut -c1-16)
    python3 "$PC" plan "$PC_DIR/list.json" --seed "$PC_SEED" --all-max "$PC_SUBSET_ALL_MAX" \
        --sample "$PC_SAMPLE_SUBSETS" --tear-all-max "$PC_TEAR_ALL_MAX" >"$PC_DIR/plan.json" || {
        harness "$PC_NAME: the plan could not be made"; return 1; }
    # A book's checks share the block digests they take (powercut.py checks
    # --digests): every cut of the run holds the base's files to theirs.
    python3 "$PC" checks "$PC_DIR/plan.json" "$PC_DIR/base.img" "$PC_DIR/run.log" \
        "$PC_DIR/base.check.json" --jobs "$PC_CHECK_JOBS" --digests "$PC_DIR/digests.json" >"$PC_DIR/cuts.jsonl" || {
        harness "$PC_NAME: the checks could not be run"; return 1; }
    pc_lies
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
# boots, ARGS handed to it), each built, booted (by PC_BOOT, pc_boot unless a
# scenario's volume arrives otherwise), asked the scenario's question
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
        if "${PC_BOOT:-pc_boot}" "$img" && ! grep -aq '\[TagFS\] this volume is NOT mounted' build/serial.log; then
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
# KEY=ROW, ROW a row of stand_caveat.sh's one table — the verify rows of
# noanchor and hog, and a book's own oracles, each with the defect it stands
# for; a census code no row names is a FAIL until the owner assigns it a step.
pc_caveats() {
    case "$1" in
        noanchor|noanchormut) echo "verify=noanchor" ;;
        hog)                  echo "verify=hog" ;;
    esac
    printf '%s\n' files:whole=book:whole order=book:order lines:pc-l.log=book:lines \
        slip:blocks=book:slip tipin=book:tipin spared=book:spared engross=book:engross chain=book:chain dedup=book:dedup \
        blot:owners=book:owners smudge:owners=book:owners blot:proven=book:lost smudge:said=book:lost \
        smudge:wiped=book:wiped blot:boots=book:boots smudge:boots=book:boots
    # On a fixed medium a book's ribbon keeps the medium's own atom, and a
    # smudge that wipes the settled stitches of a spoilt page whole with
    # nothing past them is the format's limit there: held
    # by its own row, signed by the medium the recorded boot mounted (pc_sign).
    [ "${PC_MEDIUM:-}" != fixed ] || echo smudge:wiped:fixed=book:wiped:fixed
    [ "$1" != machineb ] || echo lines:b-l.log=book:lines
}

# pc_judge ARGS... — every oracle (tools/powercut.py judge), into
# $PC_DIR/verdicts.tsv; the report printed and kept.
pc_judge() {
    local c args=()
    for c in $(pc_caveats "$PC_NAME"); do args+=(--caveat "$c"); done
    args+=(--medium "$PC_MEDIUM")
    pc_lie_boots
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

# ‼ PERSIST'S WRITE ALONE IN THE WINDOW. A program's start is heard by the
# gardener, which writes garden.plants and anchors it (keep.cpp): an anchor
# that comes after persist's write settles that write too, as every waiter's
# cycle settles all that was inked before it began. Whether it comes before
# or after is the scheduler's to say on one core, and noanchormut asks what
# the cadence alone does — so the gardener is taken off the base's list of
# programs the machine starts, and nothing but the cadence settles anything.
pc_noanchor() {
    pc_base "autostart remove gardener" && pc_logged || return 1
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
    # A cut that asks the question bears the anchor's file on its ribbon, and
    # anchor_test would find it there and make none: the create is a file no
    # medium of the run held (powercut_judge.py Reissue).
    PC_ASK_CMD="persist write"; PC_ASK_ACK='\[PERSIST\] (WROTE|FAIL|create failed|fwrite failed)'
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
    [ -z "$(pc_toll "$name")" ] || buildkey_restore || bad "$PC_NAME: the plain kernel could not be built back"
}

# medium_says DIR — the logs stand_seat_kind reads, in the kernel's own words:
# the Boardroom's seat line and TagFS's mount line, each format read off its
# source by the arguments it prints (boardroom.c, tagfs.c), so a word changed
# there is a FAIL of the selftest and not a book's every run unsigned.
# DIR/fixed.log: a volume on the seat the Boardroom calls plainly;
# DIR/removable.log: one on a seat it calls removable.
medium_says() {
    python3 - "$1" src/kernel/core/boardroom/boardroom.c src/kernel/tagfs/tagfs.c <<'EOF'
import os, re, sys
d, room, fs = sys.argv[1], open(sys.argv[2]).read(), open(sys.argv[3]).read()
seat = re.search(r'kprintf\("([^"]*)\\n",\s*s->number,\s*s->name,\s*s->removable \? "([^"]*)" : ""\);', room)
mount = re.search(r'kprintf\("([^"]*)\\n",\s*vol->mount\.seat,\s*BoardroomSeatName\(vol->mount\.seat\)\);', fs)
if not seat or not mount:
    sys.exit("medium_says: %s prints no %s line by its arguments" % (
        (sys.argv[2], "seat") if not seat else (sys.argv[3], "mount")))
ata, usb = "ATA primary master", "usb0 QEMU QEMU HARDDISK"
lines = {"fixed": [(seat, (0, ata, "")), (mount, (0, ata))],
         "removable": [(seat, (0, ata, "")), (seat, (1, usb, seat.group(2))), (mount, (1, usb))]}
for name, said in lines.items():
    with open(os.path.join(d, name + ".log"), "w") as fh:
        fh.write("".join(m.group(1).replace("%u", "%d") % args + "\n" for m, args in said))
EOF
}

# selftest — the gate judging itself: the planner's arithmetic, the medium's
# words read off the kernel's source, then one real record that the
# self-check must accept and, forged by one byte, refuse.
run_selftest() {
    local said
    pc_begin selftest
    python3 "$PC" selftest >"$PC_DIR/planner.txt" 2>&1
    chk $? "selftest: the planner's arithmetic — $(tail -1 "$PC_DIR/planner.txt")"
    grep -v '^  ok' "$PC_DIR/planner.txt" | grep -v '^selftest:' | sed 's/^/  /'
    # A smudge's wipe is the format's limit on a fixed medium alone: the
    # medium read off the kernel's own words of the volume's
    # seat, as boardroom.c and tagfs.c print them (medium_says), and the
    # limit's row given only to a volume on one.
    medium_says "$PC_DIR"
    chk $? "selftest: the Boardroom's seat line and the mount's line read off the kernel's source"
    said="$(stand_seat_kind "$PC_DIR/fixed.log"), $(stand_seat_kind "$PC_DIR/removable.log"), $(stand_seat_kind "$PC_DIR/planner.txt")"
    [ "$said" = "fixed, removable, no medium: $PC_DIR/planner.txt mounted no volume" ]
    chk $? "selftest: a volume's medium is fixed or removable as the Boardroom calls its seat ($said)"
    [ "$(PC_MEDIUM=fixed pc_caveats halt | grep -cx 'smudge:wiped:fixed=book:wiped:fixed')" = 1 ] &&
        [ "$(PC_MEDIUM=removable pc_caveats halt | grep -c '^smudge:wiped:fixed=')" = 0 ] &&
        [ "$(caveat_step book:wiped:fixed)" = limit ]
    chk $? "selftest: a smudge's wipe of the settled stitches of a spoilt page is held as the format's limit (row book:wiped:fixed, step '$(caveat_step book:wiped:fixed)') on a fixed medium alone"
    # The run signed (pc_sign): a way down it cannot read stops the scenario,
    # said; a medium it cannot read is said, and the run goes on with no
    # limit's row.
    sed 's/ATA primary master/NVMe 0 ns 1/' "$PC_DIR/fixed.log" >"$PC_DIR/nvme.log"
    grep -v '^\[Boardroom\]' "$PC_DIR/fixed.log" >"$PC_DIR/unseated.log"
    said=$(pc_sign "$PC_DIR/nvme.log"; echo "rc=$? $PC_PATH|$PC_MEDIUM")
    case "$said" in *HARNESS*"no path: seat 'NVMe 0 ns 1' is none this knows"*"rc=1 "*) true ;; *) false ;; esac
    chk $? "selftest: a run whose writes go down a way stand_path does not know stops, and says why ($(echo $said | tail -c 120))"
    said=$(pc_sign "$PC_DIR/unseated.log"; echo "rc=$? $PC_PATH|$PC_MEDIUM")
    case "$said" in *HARNESS*"medium is not known"*"rc=0 ATA sync|no medium: the Boardroom never named seat 0"*) true ;; *) false ;; esac
    chk $? "selftest: a volume whose medium the Boardroom never named is said, and its run goes on unsigned ($(echo $said | tail -c 120))"
    said=$(pc_sign "$PC_DIR/fixed.log"; echo "rc=$? $PC_PATH|$PC_MEDIUM")
    [ "$said" = "rc=0 ATA sync|fixed" ]
    chk $? "selftest: a volume on a fixed medium is signed by both ($said)"
    # A logger that answered before a departure came with a cycle in flight
    # (pc_logger_early): ended, or refused while its stick was away — C14 not
    # put; refused with its stick seated — the kernel's FAIL, with its line.
    printf '%s\n' "boot" "[TagFS] volume 0: the medium the volume lives on has left" "[QUILL] log: FAIL — an append answered -5" \
        "[TagFS] volume 0: the volume is back, in seat 1" "[QUILL] log: PASS — pc-l.log (fid 81): 400 lines" >"$PC_DIR/away.log"
    said="$(pc_logger_early "$PC_DIR/away.log" 0 1 4 1)|$(pc_logger_early "$PC_DIR/away.log" 3 1 4 1)|$(pc_logger_early "$PC_DIR/away.log" 0 1 2 1)"
    case "$said" in *HARNESS*"refused while the stick was away (serial lines 1..4"*"answered -5"*"|"*HARNESS*"ended after 1 departure"*"400 lines"*"|"*FAIL*"refused with its stick seated (serial line 3"*"answered -5") true ;; *) false ;; esac
    chk $? "selftest: a logger's early answer is said with its own line — a refusal while its stick was away and an end are C14 not put, a refusal past the return is the kernel's"
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

# The *mut scenarios (powercut_mut.sh) and what is asked of a book
# (powercut_book.sh), beside this file.
. "$(dirname "$0")/powercut_mut.sh"
. "$(dirname "$0")/powercut_book.sh"

pc_preflight
case "${1:-}" in
    noanchor|nextfid|hog|halt|race|overwrite|logger|tipin|sessions|dedup)
        run_scenario "$@" ;;
    replug)         shift; run_replug replug "$@" ;;
    machineb)       shift; run_replug machineb "$@" ;;
    spared)         shift; run_cut spared spared "$@" ;;
    undonecut)      shift; run_cut undonecut undone "$@" ;;
    engross)        shift; run_cut engross engross "$@" ;;
    noanchormut)    run_noanchormut ;;
    firstflushmut)  run_firstflushmut ;;
    lastflushmut)   run_lastflushmut ;;
    booknextfidmut) run_booknextfidmut ;;
    stitchmut)      run_stitchmut ;;
    selftest)       run_selftest ;;
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
        run_noanchormut
        run_scenario overwrite; run_scenario overwrite UEFI=on STRICT=on CORES=4 MEM=4G
        run_scenario logger; run_scenario tipin STRICT=on CORES=4 MEM=4G
        run_cut spared spared; run_scenario sessions; run_cut undonecut undone; run_cut engross engross
        run_scenario dedup
        run_replug replug CORES=4 MEM=4G; run_replug machineb
        run_firstflushmut; run_lastflushmut; run_booknextfidmut; run_stitchmut ;;
    *)
        echo "usage: $0 noanchor|nextfid|hog|halt|race|overwrite|logger|tipin|sessions|dedup [MACHINE KEYS...]"
        echo "       $0 replug|machineb|spared|undonecut|engross [MACHINE KEYS...]"
        echo "       $0 noanchormut|firstflushmut|lastflushmut|booknextfidmut|stitchmut|selftest|all"
        exit 2 ;;
esac

STAND_FINISHED=1
echo
echo "powercut: $PASS passed, $FAIL failed, $CAVEAT caveat(s) held"
if [ "$CAVEAT" != 0 ]; then
    echo "powercut: $CAVEAT caveat(s) held — each is a known defect reproduced on purpose and names the step that discharges it, or a limit the owner accepted, which no step discharges"
fi
if [ "$HARNESS" != 0 ]; then
    echo "powercut: $HARNESS harness failure(s) — this run could not talk to the"
    echo "          machine it was judging, so its verdicts are not evidence."
fi
[ "$FAIL" = 0 ] && [ "$HARNESS" = 0 ]
