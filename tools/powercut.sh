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
#               final state booted and `persist verify` asked. Red on HEAD:
#               caveat 1.7.
#   nextfid     anchor_test, then the power goes. Every prefix of the anchor's
#               sync is booted and anchor_test asked again: a fid the cut's table
#               already names, issued twice. Red on HEAD: caveat 1.6.
#   hog         one core: quilltest settle (a write, not anchored, beside a
#               hog); 5+D s later the power goes; `quilltest verify`. Red on
#               HEAD: caveat 1.7. And the design's "an anchor under the hog
#               answers": `quilltest hogged anchor_test` on the booted state.
#   halt        quilltest churn writes for ever; the power button. The machine
#               must switch off, and what churn wrote is durable behind the
#               halt's flush. A guard: no caveat.
#   race        quilltest race: 200 anchors against a churn, then the power
#               button; the halt's oracles. A guard unless measured red.
#   settlecut   the second oracle: kernels built SETTLECUT=ledger and =done,
#               each booted from a copy, anchor_test, the machine stopped
#               inside its sync; volume-read's codes against PowerCut's cut at
#               the same point of the same sync.
#   noanchormut nextfidmut tablemut flushmut
#               the fix or the fault put in on purpose: the caveat must be
#               lifted early, the durability or the ack oracle must go red.
#   selftest    the planner's arithmetic, and a record forged by one byte that
#               the record's self-check must refuse.
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
    case "$PC_ROOT" in *,*)
        echo "powercut: $PC_ROOT holds a comma, and QEMU reads a comma in a file name as the"
        echo "          end of the option — run under a TMPDIR without one"; exit 1 ;;
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
# is volume-read's. 0 until the first entry lands.
pc_record_count() {
    [ "$(od -An -t x8 -N 8 "$1" 2>/dev/null | tr -d ' ')" = "6a736677736872" ] || { echo 0; return; }
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
# tree, 4 (UNDER TEST) for one a *mut scenario mutated.
pc_build() {
    local rc
    build
    tools/buildplate verify build/boxos.img --root . >"$SCRATCH/plate.txt" 2>&1; rc=$?
    if [ "$rc" = 3 ]; then
        rm -f build/kernel.bin
        build
        tools/buildplate verify build/boxos.img --root . >"$SCRATCH/plate.txt" 2>&1; rc=$?
    fi
    [ "$rc" = "$1" ] && return 0
    # The plate's own two numbers say which: a tree that changed under the
    # build (a file edited while this ran) or an image that is not this tree.
    harness "powercut: the image's plate answers $rc where $1 was due, so nothing says it is this tree: $(grep -a 'tree ' "$SCRATCH/plate.txt" | sed 's/^ *//' | tr '\n' ';')"
    return 1
}

pc_begin() {   # pc_begin NAME KEYS...
    PC_NAME=$1; shift
    PC_KEYS="$*"
    PC_DIR="$PC_ROOT/$PC_NAME-$(printf '%s' "${PC_KEYS:-plain}" | tr ' =' '_-')"
    PC_CORES=$(printf '%s\n' $PC_KEYS | sed -n 's/^CORES=\([0-9][0-9]*\)$/\1/p' | tail -1)
    PC_CORES=${PC_CORES:-1}
    PC_PATH="unknown"
    rm -rf "$PC_DIR"
    mkdir -p "$PC_DIR/boots"
    : > "$PC_DIR/steps.jsonl"
    : > "$PC_DIR/facts.txt"
    PC_T0=$(date +%s)
    echo "== powercut $PC_NAME${PC_KEYS:+ ($PC_KEYS)} =="
}

# pc_boot DISK [KEYS...] — DISK booted as it stands, on this scenario's
# machine, to a shell that has stopped talking.
pc_boot() {
    local disk=$1; shift
    make run-stop >/dev/null 2>&1
    # shellcheck disable=SC2086
    if ! make run-bg BOOTDISK="$disk" $PC_KEYS "$@" >"$PC_DIR/run-bg.out" 2>&1; then
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
pc_logged() {
    pc_clone "$PC_DIR/base.img" "$PC_DIR/run.img" || return 1
    if ! pc_boot "$PC_DIR/run.img" POWERCUT=on POWERCUT_LOG="$PC_DIR/run.log" "$@"; then
        bad "$PC_NAME: the recorded boot never reached a shell"; return 1
    fi
    [ "$(deed_uuid "$PC_DIR/run.img")" = "$PC_BASE_UUID" ] || {
        harness "$PC_NAME: the recorded boot stands on another volume than the base"; return 1; }
    PC_PATH=$(stand_path build/serial.log "$PC_CORES")
}

# pc_step NAME CMD ACK_ERE [DURABLE [noquiet]] — ask, wait for the answer, and
# note where the record stood before, at the answer and once it went quiet.
# DURABLE is NAME:RECORD:FROM:TO for a step whose answer promises durability
# (TO 0: as many records as the record holds writes).
# noquiet: the step leaves a writer behind, and the record never goes quiet.
pc_step() {
    local name=$1 cmd=$2 ack=$3 durable=${4:-} n0 nack nq mark line
    n0=$(pc_record_count "$PC_DIR/run.log")
    mark=$(wc -l < build/serial.log)
    type_line "$cmd" || { bad "$PC_NAME: the machine never took '$cmd'"; return 1; }
    if ! wait_for_line -E build/serial.log "$ack" "$WAIT_QUIET_S" $((mark + 1)); then
        bad "$PC_NAME: '$cmd' never answered /$ack/"; return 1
    fi
    nack=$(pc_record_count "$PC_DIR/run.log")
    line=$(tail -n +$((mark + 1)) build/serial.log | tr -d '\r' | grep -aE "$ack" | head -1)
    if [ "${5:-}" = noquiet ]; then
        nq=$nack
    else
        wait_for_quiet "$PC_RECORD_QUIET_S" "$PC_DIR/run.log"
        nq=$(pc_record_count "$PC_DIR/run.log")
    fi
    python3 "$PC" step "$PC_DIR/steps.jsonl" --step "$name" --cmd "$cmd" --before "$n0" \
        --ack "$nack" --quiet "$nq" --line "$line" ${durable:+--durable "$durable"}
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
    echo "machine: ${PC_KEYS:-plain BIOS}, $PC_CORES core(s), a userspace write goes by $PC_PATH" >>"$PC_DIR/facts.txt"
    python3 "$VR" "$PC_DIR/base.img" cut "$PC_DIR/run.log" --list --json >"$PC_DIR/list.json" || {
        harness "$PC_NAME: volume-read could not list the record"; return 1; }
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

# pc_boots PICKS — the media the policy names (tools/powercut.py boots), each
# built, booted, asked the scenario's question (PC_ASK_CMD/PC_ASK_ACK, and on
# the final state PC_FINAL_CMD/PC_FINAL_ACK too), and its log kept.
pc_boots() {
    local id epoch keep tear why img="$PC_DIR/boot.img"
    python3 "$PC" boots "$PC_DIR" --picks "$1" --sample "$PC_SAMPLE_BOOTS" --seed "$PC_SEED" \
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
            pc_ask "$PC_ASK_CMD" "$PC_ASK_ACK"
            case "$why" in *"final state"*)
                [ -n "${PC_FINAL_CMD:-}" ] && pc_ask "$PC_FINAL_CMD" "$PC_FINAL_ACK" ;;
            esac
        fi
        make run-stop >/dev/null 2>&1
        cp build/serial.log "$PC_DIR/boots/$id.serial.log"
        rm -f "$img"
    done 3<"$PC_DIR/boots.txt"
}

# The caveats a scenario's verdicts may be held under: KEY=NAME, NAME a row of
# stand.sh's one table. A code no row names is a FAIL until the owner assigns
# it a step; a row that sees nothing is lifted early.
pc_caveats() {
    case "$1" in
        noanchor|noanchormut) echo "verify=noanchor" ;;
        nextfid|nextfidmut|tablemut|flushmut) echo "reissue=nextfid"; echo "code:FID_PAST_NEXT=nextfid" ;;
        hog)       echo "verify=hog" ;;
        settlecut) echo "code:FID_PAST_NEXT=settlecut" ;;
    esac
}

# pc_judge ARGS... — every oracle (tools/powercut.py judge), into
# $PC_DIR/verdicts.tsv; the report printed and kept.
pc_judge() {
    local c args=""
    for c in $(pc_caveats "$PC_NAME"); do args="$args --caveat $c"; done
    echo "took $(( $(date +%s) - PC_T0 )) s to judge; the report is $PC_DIR/report.txt" >>"$PC_DIR/facts.txt"
    # shellcheck disable=SC2086
    python3 "$PC" judge "$PC_DIR" --path "$PC_PATH" $args "$@" >"$PC_DIR/verdicts.tsv" || {
        harness "$PC_NAME: the judge could not read this run"; return 1; }
    sed '$d' "$PC_DIR/report.txt" | sed 's/^/  | /'
}

# pc_say — the verdicts in stand.sh's words, and a copy of them in the report.
pc_say() {
    local key kind name seen what
    while IFS="$TAB" read -r key kind name seen what; do
        case "$kind" in
            caveat)  caveat_judge "$name" "$seen" "$PC_NAME $key: $what" ;;
            harness) harness "$PC_NAME $key: $what" ;;
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
    pc_base && pc_logged QEMUEXTRA=-no-shutdown || return 1
    pc_step churn "quilltest churn _qt_churn" '\[QUILL\] churn: _qt_churn fid=[0-9]+ starts at seq=[0-9]+' \
        "" noquiet || return 1
    pc_halt_orderly _qt_churn:quill && pc_record || return 1
    PC_ASK_CMD="quilltest verify _qt_churn"; PC_ASK_ACK='\[QUILL\] verify: (PASS|FAIL)'; PC_FINAL_CMD=""
    pc_boots final
    pc_judge --verify quill-floor:_qt_churn:halt --final-consistent
}

pc_race() {
    pc_base && pc_logged QEMUEXTRA=-no-shutdown || return 1
    pc_step race "quilltest race _qt_churn 200" \
        '\[QUILL\] race: (PASS|FAIL) — anchored [0-9]+ of [0-9]+' "_qt_churn:quill:1:0" noquiet \
        || return 1
    grep -aq '\[QUILL\] race: PASS — anchored 200 of 200' build/serial.log
    chk $? "$PC_NAME: $(grep -aoE '\[QUILL\] race: (PASS|FAIL) — [^.]*' build/serial.log | head -1)"
    pc_halt_orderly _qt_churn:quill && pc_record || return 1
    PC_ASK_CMD="quilltest verify _qt_churn"; PC_ASK_ACK='\[QUILL\] verify: (PASS|FAIL)'; PC_FINAL_CMD=""
    pc_boots final
    pc_judge --verify quill-floor:_qt_churn:halt --final-consistent
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
}

# settlecut — the second oracle. A kernel built SETTLECUT=<point> stops the
# whole machine at that point of its first sync (src/kernel/tagfs/settle_cut),
# which leaves the medium a power cut there would leave. Its answer must be
# PowerCut's answer for the same point of the same sync.
# sc_boot_cut POINT — the POINT kernel's image, settled the only way a
# SETTLECUT kernel can be (its power button would sync, and the sync would
# stop at POINT): booted once, its mount's writes on the disk, and cut. Then
# booted again, anchor_test, and the machine stops itself inside that sync.
sc_boot_cut() {
    local pid img="$PC_DIR/$1.img"
    if ! build_with_key SETTLECUT="$1"; then
        bad "settlecut: SETTLECUT=$1 does not build: $(tail -3 "$SCRATCH/build.log" | tr '\n' ' ')"; return 1
    fi
    pc_clone build/boxos.img "$img"
    pc_boot "$img" || { bad "settlecut: the SETTLECUT=$1 kernel never reached a shell"; return 1; }
    pid=$(cat build/qemu.pid); kill -9 "$pid"; pc_gone "$pid"
    pc_boot "$img" || { bad "settlecut: the settled SETTLECUT=$1 volume never reached a shell"; return 1; }
    grep -aqE "^\[SETTLECUT\] armed: the first sync to reach point [0-9]+ of 9" build/serial.log
    chk $? "settlecut: $1 — $(grep -aoE '\[SETTLECUT\] armed: [^,]*' build/serial.log | head -1)"
    type_line anchor_test || { bad "settlecut: the machine never took anchor_test"; return 1; }
    if ! wait_for_line -E build/serial.log \
            "\[SETTLECUT\] the power went at point [0-9]+ of 9, .* in sync [0-9]+ of this boot" "$WAIT_QUIET_S"; then
        bad "settlecut: SETTLECUT=$1 never stopped the machine"; return 1
    fi
    ok "settlecut: $1 — $(grep -aoE '\[SETTLECUT\] the power went at point [0-9]+ of 9, .* in sync [0-9]+' build/serial.log)"
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
    python3 "$PC" settlecut "$PC_DIR" --path "$PC_PATH" >"$PC_DIR/verdicts.tsv" || {
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
# runs its scenario on the plain stand and asks one verdict.
mut_on() {   # mut_on FILE MARKER — FILE's edit (python on stdin) under the manifest
    mutate "$1"
    python3 - "$1" || { echo "powercut: the mutation of $1 did not apply"; exit 1; }
    mut_done "$1" "$2"
}

mut_run() {  # mut_run NAME BASE KEY SEEN WHY — the mutated tree through BASE's scenario
    pc_begin "$1"
    pc_build 4 || return
    if ! "pc_$2"; then
        bad "$1: the scenario stopped before its record could be judged (logs in $PC_DIR)"; return
    fi
    pc_expect "$3" "$4" "$5"
}

run_noanchormut() {
    mut_on src/userspace/apps/persist.c "logcheck mutation" <<'EOF'
import sys
p = sys.argv[1]; s = open(p).read()
a = "    kdbg_print(\"[PERSIST] WROTE fid=%d seq=%u magic=0x%x checksum=0x%x\","
assert s.count(a) == 1
open(p, "w").write(s.replace(a, "    anchor((uint32_t)fid); /* logcheck mutation: the write is anchored */\n" + a))
EOF
    mut_run noanchormut noanchor verify behaves \
        "persist anchoring its write, the noanchor caveat is lifted early"
    restore src/userspace/apps/persist.c; rm -f build/kernel.bin
}

run_nextfidmut() {
    mut_on src/kernel/tagfs/tagfs.c "logcheck mutation" <<'EOF'
import sys
p = sys.argv[1]; s = open(p).read()
a = "    SETTLE_CUT_AT(SETTLE_CUT_POINT_REGISTRY);"
assert s.count(a) == 1
s = s.replace(a, "    tagfs_write_ledger(); /* logcheck mutation: the ledger is written first */\n" + a)
a = "    tagfs_write_ledger();"
assert s.count(a + "\n") == 1
open(p, "w").write(s.replace(a + "\n", ""))
EOF
    mut_run nextfidmut nextfid reissue behaves "the ledger written first, no fid is issued twice"
    # First in time, but in the same epoch as the table: a medium that keeps
    # the table and loses the ledger still holds a row past next_file_id, and
    # the checker, which sees every subset, must still say so.
    pc_expect code:FID_PAST_NEXT defect \
        "the ledger written first but behind no flush of its own, a cut that keeps the table and loses it still holds a row past it"
    restore src/kernel/tagfs/tagfs.c
}

run_tablemut() {
    mut_on src/kernel/tagfs/tagfs.c "logcheck mutation" <<'EOF'
import sys
p = sys.argv[1]; s = open(p).read()
a = "    file_table_flush();"
assert s.count(a) == 1
open(p, "w").write(s.replace(a, "    /* logcheck mutation: the file table is never written */"))
EOF
    mut_run tablemut nextfid "durable:_anchor_test@anchor_test" fail \
        "the file table never written, the anchored file is not durable"
    restore src/kernel/tagfs/tagfs.c
}

run_flushmut() {
    mut_on src/kernel/tagfs/tagfs.c "logcheck mutation" <<'EOF'
import sys
p = sys.argv[1]; s = open(p).read()
a = "    int said = BoardroomFlush(g_tagfs_seat);"
assert s.count(a) == 1
open(p, "w").write(s.replace(a, "    return OK; /* logcheck mutation: the medium is never asked to flush */\n" + a))
EOF
    mut_run flushmut nextfid ack:anchor_test fail "the medium never asked to flush, the anchor's ack has none behind it"
    restore src/kernel/tagfs/tagfs.c
}

# selftest — the gate judging itself: the planner's arithmetic, then one real
# record that the self-check must accept and, forged by one byte, refuse.
run_selftest() {
    local said
    pc_begin selftest
    python3 "$PC" selftest >"$PC_DIR/planner.txt" 2>&1
    chk $? "selftest: the planner's arithmetic — $(tail -1 "$PC_DIR/planner.txt")"
    grep -v '^  ok' "$PC_DIR/planner.txt" | grep -v '^selftest:' | sed 's/^/  /'
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
}

pc_preflight
case "${1:-}" in
    noanchor|nextfid|hog|halt|race)
        run_scenario "$@" ;;
    settlecut)   shift; run_settlecut "$@" ;;
    noanchormut) run_noanchormut ;;
    nextfidmut)  run_nextfidmut ;;
    tablemut)    run_tablemut ;;
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
        run_noanchormut; run_nextfidmut; run_tablemut; run_flushmut ;;
    *)
        echo "usage: $0 noanchor|nextfid|hog|halt|race|settlecut [MACHINE KEYS...]"
        echo "       $0 noanchormut|nextfidmut|tablemut|flushmut|selftest|all"
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
