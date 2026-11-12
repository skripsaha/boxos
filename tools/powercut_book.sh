# powercut_book — what tools/powercut.sh asks of a book (VOLUME_FORMAT=v2):
# the scenarios of format v1's own machinery refused by name, and the
# book's matrix.
#
# SOURCED, NEVER RUN — by powercut.sh; everything it calls (run_selftest,
# run_scenario, run_noanchormut and the verdicts) is powercut.sh's,
# powercut_mut.sh's or stand.sh's. Its own file so powercut.sh stays under a
# thousand lines as the book's scenarios grow.

# VOLUME_FORMAT=v2: every image of the run is a book (the Makefile's key, held
# for every make of the run by stand.sh's make through STAND_FORMAT), and the
# record is judged by the book's census and blklog_book.py — a step committed
# by its stitch (powercut.py COMMIT_KINDS), the slot's counts in the Ledger's
# place. A scenario that stands on format v1's own machinery has no book to
# ask and is refused by name: settlecut's points of the v1 cycle, chaingrow's
# registry and table chains, and the mutations of v1's mount (nextfidmut) and
# of its cycle's pool and Ledger (poolmut, flushmut).
#
# pc_book SCENARIO [MACHINE KEYS...] — 0 once a book's own scenario or the
# book's matrix (`all`) has run, and the caller has nothing left to run; 1 for
# a scenario the common dispatch runs.
pc_book() {
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
            run_scenario overwrite; run_scenario overwrite UEFI=on STRICT=on CORES=4 MEM=4G
            run_scenario logger; run_scenario tipin STRICT=on CORES=4 MEM=4G
            run_cut spared spared; run_scenario sessions; run_cut undonecut undone; run_cut engross engross
            run_replug replug CORES=4 MEM=4G; run_replug machineb
            run_firstflushmut; run_lastflushmut; run_booknextfidmut; run_stitchmut
            return 0 ;;
        v2:overwrite|v2:logger|v2:tipin|v2:sessions)
            run_scenario "$@"; return 0 ;;
        v2:replug)     shift; run_replug replug "$@"; return 0 ;;
        v2:machineb)   shift; run_replug machineb "$@"; return 0 ;;
        v2:spared)     shift; run_cut spared spared "$@"; return 0 ;;
        v2:undonecut)  shift; run_cut undonecut undone "$@"; return 0 ;;
        v2:engross)    shift; run_cut engross engross "$@"; return 0 ;;
        v2:firstflushmut)  run_firstflushmut; return 0 ;;
        v2:lastflushmut)   run_lastflushmut; return 0 ;;
        v2:booknextfidmut) run_booknextfidmut; return 0 ;;
        v2:stitchmut)      run_stitchmut; return 0 ;;
    esac
    return 1
}

# ---------------------------------------------------------------------------
# The medium that lied (powercut_lie.py): Blot and Smudge, apart
# ---------------------------------------------------------------------------

# Booted twice of each kind (powercut.py lieboots): each pair of boots is a
# minute, and every scenario of a book has its lies.
PC_LIE_BOOTS=${PC_LIE_BOOTS:-1}
# C16's session goes on past the verdict until the ground the verdict let go
# has matured: past the Quire its first stitch lays the Undone in — a grain of
# blocks (settle/ribbon.c RibbonQuireBlocks), 256 of a 1 MiB grain, a line of
# the logger a stitch — and the lag after it (FALLOW_LIED_LAG); then
# quilltest data takes its blocks from ground that has.
PC_CHAIN_LINES=${PC_CHAIN_LINES:-300}

# pc_chain_session MODE — the session between the two boots of a chain, each
# write anchored: in MODE `lie` C17's short session B — two lines of a logger,
# two stitches, the first laying any verdict down — and in MODE `session`
# C16's — PC_CHAIN_LINES lines, then a file of data.
pc_chain_session() {
    if [ "$1" = lie ]; then
        pc_ask "quilltest log s 2" '\[QUILL\] log: (PASS|FAIL) — '
    else
        pc_ask "quilltest log s $PC_CHAIN_LINES" '\[QUILL\] log: (PASS|FAIL) — ' &&
            pc_ask "quilltest data g" '\[QUILL\] data: (PASS|FAIL) — '
    fi
}

# pc_lies — the lie's media planned from the record and judged by volume-read
# into lies.jsonl, counted apart from the power's cuts. PC_LIES=off leaves
# them out: a *mut scenario asks one verdict of the power's, and a SETTLECUT
# kernel's disk is no machine to boot a lie on.
pc_lies() {
    [ "${PC_LIES:-on}" = off ] && return 0
    python3 "$PC" lies "$PC_DIR/list.json" >"$PC_DIR/lies.json" || {
        harness "$PC_NAME: the lie's media could not be planned"; return 1; }
    python3 "$PC" checks "$PC_DIR/lies.json" "$PC_DIR/base.img" "$PC_DIR/run.log" "$PC_DIR/base.check.json" \
        --jobs "$PC_CHECK_JOBS" --lies --digests "$PC_DIR/digests.json" >"$PC_DIR/lies.jsonl" || {
        harness "$PC_NAME: the lie's media could not be judged"; return 1; }
}

# pc_lie_boots — the lie's media powercut.py lieboots names, each booted twice
# (pc_chain); the judge reads both boots and what the session between left.
pc_lie_boots() {
    local id epoch keep tear blot smudge why
    [ -f "$PC_DIR/lies.json" ] || return 0
    python3 "$PC" lieboots "$PC_DIR" --sample "$PC_LIE_BOOTS" --seed "$PC_SEED" >"$PC_DIR/lieboots.txt" || {
        harness "$PC_NAME: no lie could be chosen to boot"; return 1; }
    while IFS="$TAB" read -r id epoch keep tear blot smudge why <&3; do
        pc_chain lie "$id" "$epoch" "$keep" "$tear" "$blot" "$smudge"
    done 3<"$PC_DIR/lieboots.txt"
}

# pc_chain MODE ID EPOCH KEEP TEAR BLOT SMUDGE — one medium built and booted
# twice, the chain of two mounts a verdict owes (§3.10 C16): the first boot
# mounts it, and in MODE `session` or `lie` runs that session
# (pc_chain_session) and is switched off by its power button — its verdict
# laid down and its writes settled — or in MODE `cut` is a SETTLECUT kernel's,
# waited for until it stops itself and cut off; volume-read judges what the
# first left (ID.after.check.json); the second boot mounts that, in MODE `cut`
# again until it stops. Each boot's log is kept as ID.serial.log and
# ID.again.serial.log, and the judge holds both to what volume-read predicts
# (powercut_judge_book.py TwoBoots).
pc_chain() {
    local mode=$1 id=$2 img="$PC_DIR/chain.img"
    local args=(--epoch "$3" --keep "$4")
    [ "$5" = - ] || args+=(--tear "$5")
    [ "$6" = - ] || args+=(--blot "$6")
    [ "$7" = - ] || args+=(--smudge "$7")
    python3 "$VR" "$PC_DIR/base.img" cut "$PC_DIR/run.log" "${args[@]}" --out "$img" >/dev/null || {
        harness "$PC_NAME: $id could not be built to boot"; return 1; }
    pc_chain_boot "$mode" "$img"
    cp build/serial.log "$PC_DIR/boots/$id.serial.log"
    python3 "$VR" "$img" check --json >"$PC_DIR/boots/$id.after.check.json" 2>/dev/null
    pc_chain_boot "$mode" "$img" again
    cp build/serial.log "$PC_DIR/boots/$id.again.serial.log"
    rm -f "$img"
}

# pc_chain_boot MODE IMG [again] — one boot of a chain (pc_chain).
pc_chain_boot() {
    local pid
    if [ "$1" = cut ]; then
        make run-stop >/dev/null 2>&1
        make run-bg BOOTDISK="$2" ${PC_KEYS[@]+"${PC_KEYS[@]}"} >"$PC_DIR/run-bg.out" 2>&1 &&
            wait_for_line -E build/serial.log "$SC_CUT_ERE" "$WAIT_QUIET_S"
        chk $? "$PC_NAME: the SETTLECUT kernel stops itself ${3:+again }before the verdict's stitch ($(grep -aoE '\[SETTLECUT\] the power went at point [0-9]+ of [0-9]+, [^,]*' build/serial.log | head -1))"
        pid=$(cat build/qemu.pid 2>/dev/null)
        [ -n "$pid" ] && kill -9 "$pid" && pc_gone "$pid"
    elif "${PC_BOOT:-pc_boot}" "$2" && [ -z "${3:-}" ] && ! grep -aq '\[TagFS\] this volume is NOT mounted' build/serial.log; then
        pc_chain_session "$1" || bad "$PC_NAME: a chain's session did not answer: $(tr -d '\r' <build/serial.log | grep -aE '^\[QUILL\]' | tail -1 | cut -c1-160)"
        pid=$(cat build/qemu.pid)
        mon_raw system_powerdown && pc_gone "$pid" || bad "$PC_NAME: a chain's first boot would not switch off by its power button"
    fi
    make run-stop >/dev/null 2>&1
}

# ---------------------------------------------------------------------------
# A book's own scenarios (spec_v2 §3.10)
# ---------------------------------------------------------------------------

# A logger's lines (apps/quilltest_book.c QuillBookLog, 48 bytes each): past
# one block — 85 fill 4 KiB — so its tail rides the stitches as Slips and then
# takes a block (C21); enough for TALLY_SLACK to owe Tallies laid in under it
# (C7–C10); and enough that a SETTLECUT kernel reaches its point inside it.
PC_LOG_LINES=${PC_LOG_LINES:-120}
PC_TIPIN_LINES=${PC_TIPIN_LINES:-1500}
PC_CUT_LINES=${PC_CUT_LINES:-3000}

# pc_said WHAT — the PASS quilltest WHAT said, or its FAIL as one.
pc_said() {
    grep -aqE "\[QUILL\] $1: PASS" build/serial.log
    chk $? "$PC_NAME: $(grep -aoE "\[QUILL\] $1: (PASS|FAIL) — [^,]*" build/serial.log | tail -1)"
}

# overwrite — C1, C15: quilltest data writes a file, appends to it, writes it
# over whole and in part, cuts it and grows it across a hole, each round
# anchored; the power goes once it is said. Every cut shows the file as one
# state a flush stood behind left it — its old 4096 bytes or its new, never a
# mix and never a rot (powercut_judge_book.py Whole, on every book's run).
pc_overwrite() {
    pc_base && pc_logged || return 1
    pc_step data "quilltest data pc" '\[QUILL\] data: (PASS|FAIL) — ' || return 1
    pc_said data
    pc_cut_power && pc_record || return 1
    PC_ASK_CMD=""; PC_FINAL_CMD=""
    pc_boots final
    pc_judge
}

# logger — C21: PC_LOG_LINES lines appended to pc-l.log one at a time, each
# anchored; the power goes once the logger said so. Every cut shows whole
# lines, never fewer than the flush before it left; every line in every cut
# after the last anchor's flush; and the file took blocks of data as its lines
# filled them and as Tallies owed an Engross, never one a line.
pc_logger() {
    pc_logged_lines "$PC_LOG_LINES" || return 1
    pc_judge --verify "logger:pc-l.log:$PC_LOG_LINES:log"
}

# tipin — C7–C10: the logger run long enough that its stitches owe Tallies,
# laid in among the nodes of the one before (TipIn); every cut inside a
# Tally's nodes, between their flush and the slot, inside the slot and after
# it is judged with the rest, and the newest Tally where the run left it is
# held to the rule of binding (volume-read bound).
pc_tipin() {
    pc_logged_lines "$PC_TIPIN_LINES" || return 1
    pc_judge --verify "logger:pc-l.log:$PC_TIPIN_LINES:log" --verify tipin
}

pc_logged_lines() {
    pc_base && pc_logged || return 1
    pc_step log "quilltest log pc $1" '\[QUILL\] log: (PASS|FAIL) — ' "pc-l.log:log:$1:$1" || return 1
    pc_said log
    pc_cut_power && pc_record || return 1
    PC_ASK_CMD=""; PC_FINAL_CMD=""
    pc_boots final
}

# sessions — C16: the logger's run, and the first of its cuts whose stitch
# landed and some of its data did not (C3, a stitch whose toll spared the
# flush of its data) booted: its mount rolls the file back, the session's first
# stitch lays the verdict down (an Undone, the tag settle:undone), the session
# goes on past the Quire it lies in and the lag, so the ground the verdict let
# go matures, and writes a file of data on ground that has, the power button;
# the census of what that session left shows the file rolled back as it was
# and wearing settle:undone, nothing rolled back again, no block of two
# owners, and both mounts are the ones predicted.
pc_sessions() {
    pc_logged_lines "$PC_LOG_LINES" || return 1
    pc_tolled
    pc_chain_first session
    pc_judge --verify "logger:pc-l.log:$PC_LOG_LINES:log" --verify chain:laid
}

# pc_chain_first MODE — the first C3 cut of the run, booted twice (pc_chain).
pc_chain_first() {
    local id epoch keep tear why
    python3 -c 'import sys; sys.path.insert(0, "tools"); import powercut_judge_book as b; b.FirstNotLanded(sys.argv[1])' \
        "$PC_DIR" >"$PC_DIR/chain.txt" || { harness "$PC_NAME: the run's cuts could not be read for a C3 one"; return 1; }
    IFS="$TAB" read -r id epoch keep tear why <"$PC_DIR/chain.txt" || return 0
    pc_chain "$1" "$id" "$epoch" "$keep" "$tear" - -
}

# The scenarios whose chain boots a cut that kept a stitch and lost data it
# names (C3, pc_chain_first): built TOLL=spare (Makefile), so their record
# holds such cuts whatever pace a loaded host lets the medium keep — the toll's
# own judgment is pc_spared's to hold, and the stand's stand_toll_* tests'.
PC_TOLLED="sessions undonecut"

# pc_toll NAME — the key NAME's kernel is built with for its toll, or nothing.
pc_toll() { case " $PC_TOLLED " in *" $1 "*) echo TOLL=spare ;; esac; }

# pc_tolled — the record's TOLL=spare kernel said, at its first stitch that
# names fresh data, that it is begun before the flush of that data.
pc_tolled() {
    tr -d '\r' <"$PC_DIR/run.serial.log" | grep -aqE '^\[TagFS\] TOLL=spare: a stitch that names fresh data is begun before'
    chk $? "$PC_NAME: the record's kernel begins every stitch that names fresh data before its flush (TOLL=spare), as it says"
}

# run_cut NAME POINT [KEYS...] — scenario NAME on a kernel built
# SETTLECUT=POINT (settle_cut.c), and TOLL=spare where pc_toll names it: the
# base, the record and every cut carry it, and the plain kernel is built back
# after.
run_cut() {
    local name=$1 point=$2 PC_LIES=off
    shift 2
    pc_begin "$name" "$@"
    # shellcheck disable=SC2046
    if ! build_with_key "$(strict_key "$@")" SETTLECUT="$point" $(pc_toll "$name"); then
        bad "$PC_NAME: SETTLECUT=$point does not build: $(tail -3 "$SCRATCH/build.log" | tr '\n' ' ')"
        return
    fi
    tools/buildplate verify build/boxos.img --root . >"$SCRATCH/plate.txt" 2>&1
    if "pc_$name"; then
        pc_say
    else
        bad "$PC_NAME: the scenario stopped before its record could be judged (logs in $PC_DIR)"
    fi
    make run-stop >/dev/null 2>&1
    pc_lockrank
    buildkey_restore || bad "$PC_NAME: the plain kernel could not be built back"
}

# pc_armed — the SETTLECUT kernel said what it is armed for.
pc_armed() {
    tr -d '\r' <build/serial.log | grep -aqE "$SC_ARMED_ERE"
    chk $? "$PC_NAME: $(grep -aoE '\[SETTLECUT\] armed[^,]*' build/serial.log | head -1)"
}

# pc_logged_cut — the logger on the recorded machine until its SETTLECUT
# kernel stops itself, and the power cut there.
pc_logged_cut() {
    pc_base && pc_logged || return 1
    pc_armed
    pc_step cut "quilltest log pc $PC_CUT_LINES" "$SC_CUT_ERE|\[QUILL\] log: (PASS|FAIL) — " || return 1
    tr -d '\r' <build/serial.log | grep -aqE "$SC_CUT_ERE"
    chk $? "$PC_NAME: the machine stops itself at its point inside the logger ($(tr -d '\r' <build/serial.log | grep -aoE '\[SETTLECUT\] the power went at point [0-9]+ of [0-9]+, [^,]*|\[QUILL\] log: (PASS|FAIL) — [^,]*' | tail -1))"
    pc_cut_power && pc_record || return 1
    PC_ASK_CMD=""; PC_FINAL_CMD=""
}

# spared — C3: SETTLECUT=spared stops the machine before the last flush of the
# first anchored stitch whose toll spared the flush of its data; the cuts of
# that epoch keep the stitch and lose data it names, and each rolls those files
# back by their splice (NOT_LANDED) — the logger's file whole lines all the same.
pc_spared() {
    pc_logged_cut || return 1
    pc_boots final
    pc_judge --verify spared --verify "lines:pc-l.log:$PC_CUT_LINES"
}

# undonecut — C16, the cut before the verdict settles: the logger's run on a
# SETTLECUT=undone kernel (no verdict in it: the point never fires), its first
# C3 cut booted — the kernel stops before the first atom of the stitch that
# lays the verdict down — and booted again: the same verdict, no block let go.
pc_undonecut() {
    pc_logged_lines "$PC_LOG_LINES" || return 1
    pc_tolled
    pc_chain_first cut
    pc_judge --verify "logger:pc-l.log:$PC_LOG_LINES:log" --verify chain:cut
}

# engross — C22: SETTLECUT=engross stops the machine before the first atom of
# the stitch that names a block an Engross laid — the logger's tail written
# into a block for the Tally it owes: the block lies free, nobody's, and the
# file is its Slips over its old base, whole lines.
pc_engross() {
    pc_logged_cut || return 1
    pc_boots final
    pc_judge --verify engross --verify "lines:pc-l.log:$PC_CUT_LINES"
}

# ---------------------------------------------------------------------------
# replug — C14: the volume a USB stick that leaves while a stitch is written
# ---------------------------------------------------------------------------
#
# The tree carries the late arrival (powercut_mut.sh): the machine passes over
# its own disk and mounts the stick the host hands it. The stick is put
# together over QMP the way the Makefile's POWERCUT_CHAIN puts the boot disk
# together — its file, the record's file, blklogwrites over both, raw on top —
# so every write and flush it takes is on the record, across a departure and a
# return alike: the node outlives the device, and the device is put back on
# it. A logger appends to it, anchored line by line, on a stick paced as a
# slow one is (pc_stick_add paced): unpaced, QEMU's stick let the logger
# settle all its cycles before the harness, reading the record once a second,
# had seen it writing and pulled the stick — which left with nothing in
# flight, and no departure after it could find any. The stick is pulled once
# the record shows it writing and put back, and pulled again until the machine
# says it left with a cycle in flight — changes no cycle settled held open
# (away.c AwayLeft) — or PC_REPLUG_TRIES departures came between cycles, or
# the logger answered before one did: either puts no C14 and is a HARNESS;
# each departure and what its return said (its own stitch cut short, M-6, or
# not) in facts.txt. C14 as QEMU's stick puts it: a departure with the cycle
# in flight, whose stitch the throttle holds and the drain at the pull lets
# through whole while a stitch is one command — so the medium holds no stitch
# of the machine's own cut short, and the return's reading of its own
# unfinished attempt (spec_v2 §16.5 п.12) is the stand's to ask
# (stand_away_own_attempt, stand_away_cut_midway). The open book judges every
# return (Perambulation: the book taken, every waiter answered); the logger
# ends and the power goes.
# Every cut of the record — before the departure, across it, after the return
# — is judged as every book's run is, and booted on the stick as the machine
# took it. QEMU keeps every write it took, so the return finds the book as the
# kernel left it: any other verdict is a FAIL.
PC_REPLUG_LINES=${PC_REPLUG_LINES:-400}
PC_REPLUG_AFTER=${PC_REPLUG_AFTER:-24}     # record entries the logger lays before the stick is pulled
PC_REPLUG_TRIES=${PC_REPLUG_TRIES:-6}      # departures tried for one that comes with a cycle in flight

# pc_qmp COMMAND-JSON... — each {"execute", "arguments"} put to the machine's
# QMP socket in turn; the first error said and 1, else the last one's return
# printed as JSON.
pc_qmp() {
    python3 - build/qemu.qmp "$@" <<'QMP_EOF'
import json, socket, sys
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
f = s.makefile("rw")
def Answer():
    while True:
        m = json.loads(f.readline())
        if "event" not in m:
            return m
Answer()
for cmd in [{"execute": "qmp_capabilities"}] + [json.loads(x) for x in sys.argv[2:]]:
    f.write(json.dumps(cmd) + "\n")
    f.flush()
    m = Answer()
    if "error" in m:
        sys.exit("%s: %s" % (cmd["execute"], m["error"].get("desc")))
print(json.dumps(m.get("return")))
QMP_EOF
}

# pc_stick_add IMG [RECORD [append|paced]] — IMG handed to the machine as a
# USB stick, its writes and flushes on RECORD when one is named: emptied first,
# or with `append` gone on from where another machine left it (blklogwrites
# log-append reads the record's superblock, which every entry rewrites — the
# sector size with it: QEMU refuses log-sector-size beside log-append, "log-append
# and log-sector-size are mutually exclusive"). `paced` takes the stick's writes
# at PC_STICK_PACE a second, as a slow stick takes them (a throttle node over the
# record, so every write it lets through is on the record in the order taken):
# its flushes and reads are not held, and a drain — the stick pulled — lets
# every write it holds through at once.
PC_STICK_PACE=${PC_STICK_PACE:-20}
pc_stick_add() {
    local top=stick-file append=false size='"log-sector-size": 512, '
    [ "${3:-}" = append ] && append=true size=
    set -- "$1" "${2:-}" "${3:-}" '{"execute": "blockdev-add", "arguments": {"driver": "file", "node-name": "stick-file", "filename": "'"$1"'", "cache": {"no-flush": true}}}'
    if [ -n "$2" ]; then
        [ "$append" = true ] || : > "$2"
        set -- "$@" '{"execute": "blockdev-add", "arguments": {"driver": "file", "node-name": "stick-logfile", "filename": "'"$2"'", "cache": {"no-flush": true}}}' \
            '{"execute": "blockdev-add", "arguments": {"driver": "blklogwrites", "node-name": "stick-log", "file": "stick-file", "log": "stick-logfile", '"$size"'"log-super-update-interval": 1, "log-append": '"$append"'}}'
        top=stick-log
    fi
    if [ "$3" = paced ]; then
        set -- "$@" '{"execute": "object-add", "arguments": {"qom-type": "throttle-group", "id": "stick-pace", "limits": {"iops-write": '"$PC_STICK_PACE"'}}}' \
            '{"execute": "blockdev-add", "arguments": {"driver": "throttle", "node-name": "stick-paced", "throttle-group": "stick-pace", "file": "'"$top"'"}}'
        top=stick-paced
    fi
    pc_qmp "${@:4}" '{"execute": "blockdev-add", "arguments": {"driver": "raw", "node-name": "stick", "file": "'"$top"'"}}' \
        >/dev/null && pc_stick_in
}

# pc_stick_in, pc_stick_out — the stick's device put into the machine and
# taken out over QMP, whose answer is read: a refusal comes back with QEMU's
# reason, and no command is taken for carried out unanswered. The monitor's
# HMP line (mon_raw) is written and its socket let go a second later with
# nothing read back (qemu-input.sh, nc -w 1): a paced replug's stick left once
# and was never put back — no line of the machine's, and QEMU's answer to the
# device_add unseen — where 42 departures over QMP since were each answered
# and put back.
pc_stick_in() {
    pc_qmp '{"execute": "device_add", "arguments": {"driver": "usb-storage", "drive": "stick", "id": "usbstick"}}' >/dev/null ||
        { harness "$PC_NAME: QEMU would not put the stick in (above)"; return 1; }
}
pc_stick_out() {
    pc_qmp '{"execute": "device_del", "arguments": {"id": "usbstick"}}' >/dev/null ||
        { harness "$PC_NAME: QEMU would not take the stick out (above)"; return 1; }
}

# pc_stick_seated — QEMU holds the stick's device (0), holds none (1), or did
# not answer (2).
pc_stick_seated() {
    local list
    list=$(pc_qmp '{"execute": "qom-list", "arguments": {"path": "/machine/peripheral"}}') || return 2
    echo "$list" | grep -q '"name": "usbstick"'
}

# pc_stick_pull MARK — the stick taken out of the machine, said by it after
# serial line MARK, and gone from QEMU too.
pc_stick_pull() {
    pc_stick_out &&
        wait_for_line build/serial.log "the medium the volume lives on has left" "$WAIT_QUIET_S" $(($1 + 1)) &&
        wait_until "$WAIT_QUIET_S" "QEMU to let go of the stick" 'pc_stick_seated; [ $? = 1 ]'
}

# pc_stick_boot IMG [RECORD [append|paced]] — the machine booted from a clone
# of this tree's image, which the late arrival passes over, and IMG handed to it
# as its stick (pc_stick_add): booted once the volume's hub has greeted.
pc_stick_boot() {
    local record=${2:-} how=${3:-}
    make run-stop >/dev/null 2>&1
    pc_clone build/boxos.img "$PC_DIR/inside.img" || return 1
    if ! make run-bg BOOTDISK="$PC_DIR/inside.img" ${PC_KEYS[@]+"${PC_KEYS[@]}"} >"$PC_DIR/run-bg.out" 2>&1; then
        harness "$PC_NAME: run-bg would not start: $(tail -1 "$PC_DIR/run-bg.out")"; return 1
    fi
    wait_for_line build/serial.log "$NO_VOLUME_YET" "$WAIT_QUIET_S" || return 1
    pc_stick_add "$1" "$record" "$how" || { harness "$PC_NAME: QEMU would not take $1 as a stick"; return 1; }
    wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S" || return 1
    wait_for_quiet "$BOOT_BURST_QUIET_S"
}

# pc_stick_base — the base a stick: this tree's image mounted once through the
# late arrival and switched off by its power button, judged consistent.
pc_stick_base() {
    local pid
    pc_clone build/boxos.img "$PC_DIR/base.img" || return 1
    pc_stick_boot "$PC_DIR/base.img" || { bad "$PC_NAME: the base stick never became the machine's volume"; return 1; }
    pid=$(cat build/qemu.pid)
    mon_raw system_powerdown && pc_gone "$pid" || { bad "$PC_NAME: the base would not switch off by its power button"; return 1; }
    cp build/serial.log "$PC_DIR/base.serial.log"
    python3 "$VR" "$PC_DIR/base.img" check --json >"$PC_DIR/base.check.json" ||
        { bad "$PC_NAME: the base is not consistent before anything was cut"; return 1; }
    echo "base: a stick of volume $(deed_uuid "$PC_DIR/base.img"), mounted once through the late arrival and switched off by its power button; check says consistent" >>"$PC_DIR/facts.txt"
}

pc_replug() {
    local n0 nack nq mark line
    pc_stick_base || return 1
    pc_clone "$PC_DIR/base.img" "$PC_DIR/run.img" || return 1
    pc_stick_boot "$PC_DIR/run.img" "$PC_DIR/run.log" paced || { bad "$PC_NAME: the recorded stick never became the machine's volume"; return 1; }
    pc_sign build/serial.log || return 1
    n0=$(pc_record_count "$PC_DIR/run.log")
    mark=$(wc -l < build/serial.log)
    type_line "quilltest log pc $PC_REPLUG_LINES" || { bad "$PC_NAME: the machine never took the logger"; return 1; }
    pc_departures || return 1
    wait_for_line -E build/serial.log '\[QUILL\] log: (PASS|FAIL) — ' "$WAIT_QUIET_S" $((mark + 1)) ||
        { bad "$PC_NAME: the logger never answered after the return"; return 1; }
    nack=$(pc_record_count "$PC_DIR/run.log")
    pc_said log
    wait_for_quiet "$PC_RECORD_QUIET_S" "$PC_DIR/run.log"
    nq=$(pc_record_count "$PC_DIR/run.log")
    python3 "$PC" step "$PC_DIR/steps.jsonl" --step log --cmd "quilltest log pc $PC_REPLUG_LINES" --before "$n0" \
        --ack "$nack" --quiet "$nq" --durable "pc-l.log:log:$PC_REPLUG_LINES:$PC_REPLUG_LINES" \
        --line "$(tail -n +$((mark + 1)) build/serial.log | tr -d '\r' | grep -aE '\[QUILL\] log: (PASS|FAIL)' | head -1)"
    pc_cut_power && pc_record || return 1
    PC_ASK_CMD=""; PC_FINAL_CMD=""
    pc_boots final
    pc_judge --verify "logger:pc-l.log:$PC_REPLUG_LINES:log"
}

# pc_departures — the stick pulled and put back until it leaves with a cycle
# in flight, every return judged; MARK, the logger's line on the way in, left
# at the last departure's line. A logger that answered before a departure came
# with a cycle in flight lays no more entries to wait for: said at once, with
# its own line (pc_logger_early).
pc_departures() {
    local n held tries=0 from=$mark gone=0 back=0
    while :; do
        tries=$((tries + 1)) n=$(pc_record_count "$PC_DIR/run.log")
        wait_until "$WAIT_QUIET_S" "the logger to lay $PC_REPLUG_AFTER entries" \
            '[ "$(pc_record_count "$PC_DIR/run.log")" -ge $((n + PC_REPLUG_AFTER)) ] || pc_logger_answered "$from"' || return 1
        pc_logger_answered "$from" && { pc_logger_early build/serial.log "$from" "$gone" "$back" $((tries - 1)); return 1; }
        mark=$(wc -l < build/serial.log)
        pc_stick_pull "$mark" &&
            pc_stick_in &&
            wait_for_line -E build/serial.log "the volume is back, in seat|is MOUNTED|would not mount" "$WAIT_QUIET_S" $((mark + 1)) ||
            { bad "$PC_NAME: the stick did not leave and come back: $(tail -n +$((mark + 1)) build/serial.log | tr -d '\r' | grep -aE '^\[(TagFS|Storage)\]' | tail -1 | cut -c1-160)"; return 1; }
        gone=$mark back=$(tail -n +$((mark + 1)) build/serial.log | grep -anE "the volume is back, in seat|is MOUNTED|would not mount" | head -1 | cut -d: -f1)
        back=$((mark + back))
        held=$(tail -n +$((mark + 1)) build/serial.log | tr -d '\r' | grep -aoE 'its book is held open: [0-9]+ change' | head -1 | grep -oE '[0-9]+')
        line=$(tail -n +$((mark + 1)) build/serial.log | tr -d '\r' | grep -aoE '\[TagFS\] .*the volume is back, in seat.*' | head -1)
        echo "$line" | grep -q "its book as this kernel left it"
        chk $? "$PC_NAME: the open book takes the stick back as it left (departure $tries, ${held:-no} change(s) no cycle settled held open) — ${line:-no return said}"
        echo "departure $tries: the stick left with ${held:-no} change(s) no cycle settled held open; $(echo "$line" | grep -q 'its own stitch cut short' && echo 'its own stitch cut short lay past the last (M-6)' || echo 'no stitch of its own cut short past the last')" >>"$PC_DIR/facts.txt"
        [ "${held:-0}" -gt 0 ] && return 0
        [ "$tries" -lt "$PC_REPLUG_TRIES" ] ||
            { harness "$PC_NAME: none of $tries departures came with a cycle in flight — C14 was not put"; return 1; }
    done
}

# pc_logger_answered MARK — the logger said its verdict past serial line MARK.
pc_logger_answered() {
    tail -n +$(($1 + 1)) build/serial.log | grep -aqE '\[QUILL\] log: (PASS|FAIL) — '
}

# pc_logger_early LOG FROM GONE BACK N — the logger's answer past serial line
# FROM of LOG, come before any of N departures came with a cycle in flight,
# said with its own line. A PASS laid every line with no cycle in flight to
# take: C14 was not put, a HARNESS. A FAIL printed while the stick was away —
# past GONE, the line the last departure began at, and before BACK, the line
# of its return — is the rule: a book whose medium has left refuses every
# write until it is back (away.c AwayLeft), and that departure came between
# cycles; a HARNESS naming the window. Any other FAIL was refused with its
# stick seated: the kernel's FAIL, with its line.
pc_logger_early() {
    local at line
    at=$(tail -n +$(($2 + 1)) "$1" | tr -d '\r' | grep -anE '\[QUILL\] log: (PASS|FAIL) — ' | head -1)
    line=${at#*:} at=$(($2 + ${at%%:*}))
    case "$line" in
        *"log: PASS"*)
            harness "$PC_NAME: the logger ended after $5 departure(s), none with a cycle in flight — C14 was not put ($line)" ;;
        *)
            if [ "$3" -gt 0 ] && [ "$at" -gt "$3" ] && [ "$at" -lt "$4" ]; then
                harness "$PC_NAME: the logger was refused while the stick was away (serial lines $3..$4, departure $5, which came between cycles: a book refuses every write until its medium is back) — C14 was not put ($line)"
            else
                bad "$PC_NAME: the logger was refused with its stick seated (serial line $at, $([ "$3" -gt 0 ] && echo "past the return at line $4" || echo "before any departure")) — $line"
            fi ;;
    esac
}

# run_replug NAME [KEYS...] — NAME (replug, machineb) on the tree with the late
# arrival, put back after.
run_replug() {
    local PC_BOOT=pc_stick_boot name=$1
    shift
    pc_begin "$name" "$@"
    pc_latearrival_on
    if pc_build 4 && "pc_$name"; then
        pc_say
    else
        bad "$PC_NAME: the scenario stopped before its record could be judged (logs in $PC_DIR)"
    fi
    make run-stop >/dev/null 2>&1
    pc_lockrank
    restore src/kernel/tagfs/tagfs.c
}

# ---------------------------------------------------------------------------
# machineb — C20: the stick written on machine B while machine A holds its book
# ---------------------------------------------------------------------------
#
# Machine A records the stick (pc_stick_boot) and a logger appends to it; the
# stick is pulled, and A — its book open, waiting — is paused and saved whole
# (QMP migrate to a file) and stopped. Machine B, another boot of the same
# tree, takes the stick, its record going on from where A's left it
# (blklogwrites log-append), writes two cycles of its own and is switched off
# by its power button. A is then brought back exactly where it was paused
# (migrate-incoming: a machine saved paused comes back paused, and goes on at
# `cont`), and the stick handed back to it: what B wrote is not the book A
# left (Perambulation, OTHER) — given up and mounted whole, nothing of A's open
# book sewn onto B's ribbon. Every cut of the one record, A's, B's and A's
# again, is judged as every book's run is, both loggers' files whole lines
# and as many as their flushes made durable. QEMU saves no machine whose CPU
# it cannot carry across (STRICT's invtsc): the scenario runs on the plain one.
PC_MACHINEB_LINES=${PC_MACHINEB_LINES:-20}
PC_MACHINEB_CYCLES=2

# pc_machine_status STATUS — QEMU's run state is STATUS.
pc_machine_status() {
    pc_qmp '{"execute": "query-status"}' | grep -q "\"status\": \"$1\""
}

# pc_machine_save STATE — the machine, the stick pulled and its nodes let go
# of, paused and saved whole to STATE; QEMU itself then stopped by kill -9.
pc_machine_save() {
    local pid
    pc_qmp '{"execute": "blockdev-del", "arguments": {"node-name": "stick"}}' \
        '{"execute": "blockdev-del", "arguments": {"node-name": "stick-log"}}' \
        '{"execute": "blockdev-del", "arguments": {"node-name": "stick-logfile"}}' \
        '{"execute": "blockdev-del", "arguments": {"node-name": "stick-file"}}' \
        '{"execute": "stop"}' '{"execute": "migrate", "arguments": {"uri": "exec:cat > '"$1"'"}}' >/dev/null &&
        wait_until "$WAIT_QUIET_S" "the machine to be saved" \
            'pc_qmp "{\"execute\": \"query-migrate\"}" | grep -q "\"status\": \"completed\""' || return 1
    pid=$(cat build/qemu.pid)
    kill -9 "$pid"
    pc_gone "$pid"
}

# pc_machine_restore STATE DISK — the machine saved in STATE brought back on
# its own DISK and keys, and let run on from where it was paused.
pc_machine_restore() {
    make run-bg BOOTDISK="$2" ${PC_KEYS[@]+"${PC_KEYS[@]}"} "$(pc_extra "-incoming defer")" \
        >"$PC_DIR/run-bg.out" 2>&1 &&
        pc_qmp '{"execute": "migrate-incoming", "arguments": {"uri": "exec:cat '"$1"'"}}' >/dev/null &&
        wait_until "$WAIT_QUIET_S" "the saved machine to be loaded" 'pc_machine_status paused' &&
        pc_qmp '{"execute": "cont"}' >/dev/null &&
        wait_until "$WAIT_QUIET_S" "the saved machine to run again" 'pc_machine_status running'
}

pc_machineb() {
    local mark pid line state="$PC_DIR/a.state"
    pc_stick_base || return 1
    pc_clone "$PC_DIR/base.img" "$PC_DIR/run.img" || return 1
    pc_stick_boot "$PC_DIR/run.img" "$PC_DIR/run.log" || { bad "$PC_NAME: machine A never took the stick"; return 1; }
    pc_sign build/serial.log || return 1
    pc_step log "quilltest log pc $PC_MACHINEB_LINES" '\[QUILL\] log: (PASS|FAIL) — ' \
        "pc-l.log:log:$PC_MACHINEB_LINES:$PC_MACHINEB_LINES" || return 1
    pc_said log
    mark=$(wc -l < build/serial.log)
    pc_stick_pull "$mark" && pc_machine_save "$state" ||
        { harness "$PC_NAME: machine A could not let the stick go and be saved with its book open"; return 1; }
    cp build/serial.log "$PC_DIR/a.serial.log"
    # Its own disk, out of the way of the clone machine B boots from.
    mv "$PC_DIR/inside.img" "$PC_DIR/a.inside.img"
    echo "machine A: the stick pulled after its logger's $PC_MACHINEB_LINES lines, its book held open, saved paused" >>"$PC_DIR/facts.txt"
    pc_stick_boot "$PC_DIR/run.img" "$PC_DIR/run.log" append || { bad "$PC_NAME: machine B never took the stick"; return 1; }
    pc_step logb "quilltest log b $PC_MACHINEB_CYCLES" '\[QUILL\] log: (PASS|FAIL) — ' \
        "b-l.log:log:$PC_MACHINEB_CYCLES:$PC_MACHINEB_CYCLES" || return 1
    pc_said log
    pid=$(cat build/qemu.pid)
    mon_raw system_powerdown && pc_gone "$pid" ||
        { bad "$PC_NAME: machine B would not switch off by its power button"; return 1; }
    cp build/serial.log "$PC_DIR/b.serial.log"
    echo "machine B: the same tree booted on the stick, $PC_MACHINEB_CYCLES cycles of its own, switched off by its power button" >>"$PC_DIR/facts.txt"
    make run-stop >/dev/null 2>&1
    pc_machine_restore "$state" "$PC_DIR/a.inside.img" && pc_stick_add "$PC_DIR/run.img" "$PC_DIR/run.log" append ||
        { harness "$PC_NAME: machine A could not be brought back and handed the stick: $(tail -1 "$PC_DIR/run-bg.out")"; return 1; }
    rm -f "$state"
    wait_for_line -E build/serial.log "the volume is back, in seat|not as this kernel left its book|would not mount" "$WAIT_QUIET_S" ||
        { bad "$PC_NAME: machine A never said what it found on the stick"; return 1; }
    wait_for_line build/serial.log "is MOUNTED" "$WAIT_QUIET_S"
    wait_for_quiet "$BOOT_BURST_QUIET_S"
    line=$(tr -d '\r' < build/serial.log | grep -aoE '\[TagFS\] .*(the volume is back, in seat|not as this kernel left its book).*' | head -1)
    echo "$line" | grep -q "not as this kernel left its book" && grep -aq "is MOUNTED" build/serial.log
    chk $? "$PC_NAME: machine A takes the stick machine B wrote as another's — its open book given up and the volume mounted whole: ${line:-no return said}"
    pc_cut_power && pc_record || return 1
    PC_ASK_CMD=""; PC_FINAL_CMD=""
    pc_boots final
    pc_judge --verify "lines:pc-l.log:$PC_MACHINEB_LINES" --verify "lines:b-l.log:$PC_MACHINEB_CYCLES"
}
