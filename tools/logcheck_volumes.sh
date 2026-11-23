# shellcheck shell=bash
# Sourced by tools/logcheck.sh: the twovolumes scenario.

# ---------------------------------------------------------------------------
# twovolumes: one house of two volumes — the machine's own, and a stick a
# person named that it never held (volume/house.c, volume_name.c, carry.c).
#
# The stick is a volume create_tagfs laid with a name and one file of the
# person's. The machine takes it in as volume 1 of the house and says its
# name; `volumes` lists both, and names the stick anew; `files` finds the
# files of both, each under the volume it lies on; `carry` moves a file the
# machine wrote onto the stick and copies the stick's file home, each as it
# was. The stick is pulled and its book held: its files are shown by nothing
# — `files` says it is not plugged in, a command given its files or a name
# only it carries is refused once, naming it, and `volumes` says so — put back
# and taken up, its files shown again; and the halt makes both durable. A
# volume is named by its name as by its number (carry, files ... on,
# volumes). volume-read finds both volumes consistent, the
# stick called by its new name, the moved file on it alone with its bytes and
# the copy home; and a second boot of the same stick says the new name and
# finds the moved file where it went.
# ---------------------------------------------------------------------------

# twovol_boot TAG — the machine run to the shell with the stick in: 0 once
# the hub answers. The stick's path is $SCRATCH/twovol.stick.img.
twovol_boot() {
    local tag=$1 stick="$SCRATCH/twovol.stick.img"
    make run-stop >/dev/null 2>&1
    if ! make run-bg USB=on CORES=4 MEM=4G \
            QEMUEXTRA="-drive if=none,id=stick,file=$stick,format=raw -device usb-storage,drive=stick,id=usbstick" \
            >"$SCRATCH/twovol.$tag.run-bg" 2>&1; then
        harness "twovolumes ($tag): run-bg would not start: $(tail -1 "$SCRATCH/twovol.$tag.run-bg")"
        return 1
    fi
    wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S" &&
        wait_for_line build/serial.log 'volume 1 of the house is mounted from seat' "$WAIT_QUIET_S" &&
        wait_for_prompt
}

# twovol_halt TAG — bye, the halt waited out, the machine stopped and its log
# kept as serial.twovolumes.TAG.log.
twovol_halt() {
    local tag=$1 rc=0
    type_line "bye" && wait_for_line -E build/serial.log "$STAND_HALT_ENDS" "$WAIT_QUIET_S" || rc=1
    sleep 1
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.twovolumes.$tag.log"
    return $rc
}

# twovol_answer LOG LINE — what the machine answered the command LINE: the
# lines after its echo up to the next prompt.
twovol_answer() {
    tr -d '\r' <"$1" | awk -v cmd="$2" '
        found && /~ / { exit }
        found { print }
        index($0, "~ " cmd) && substr($0, length($0) - length(cmd) + 1) == cmd { found = 1 }'
}

# twovol_said LOG PATTERN WHAT — the log says PATTERN (grep -E).
twovol_said() {
    grep -aqE "$2" "$1"
    chk $? "twovolumes: $3"
}

run_twovolumes() {
    echo "== twovolumes: one house of two volumes — named, listed, files carried, a stick pulled and put back =="
    local stick="$SCRATCH/twovol.stick.img" note="$SCRATCH/twovol.note.txt" L verdict
    build
    printf 'a note on the stick\n' >"$note"
    if ! cp build/boxos.img "$stick" ||
       ! tools/create_tagfs --name "Summer photos" "$stick" "$note" "kind:note" >"$SCRATCH/twovol.mkfs.log" 2>&1; then
        harness "twovolumes: the stick could not be laid: $(tail -1 "$SCRATCH/twovol.mkfs.log")"
        return
    fi

    # The stick is put back once QEMU let its drive go with the device — the
    # seat said empty, and two seconds — as the replug gate puts its back: a
    # drive added again under the id still held is refused by the monitor.
    local rc=1
    if twovol_boot first &&
       type_line "volumes" && wait_for_prompt &&
       type_line "volumes 1 Holiday photos" && wait_for_prompt &&
       type_line "say hello from home -> greet.txt" && wait_for_prompt &&
       type_line "tag greet.txt kind:greeting" && wait_for_prompt &&
       type_line "carry greet.txt Holiday photos" && wait_for_prompt &&
       type_line "carry twovol.note.txt 0 copy" && wait_for_prompt &&
       type_line "files kind:greeting" && wait_for_prompt &&
       type_line "files kind:note" && wait_for_prompt &&
       type_line "files kind:note on 1" && wait_for_prompt &&
       type_line "files kind:note on Holiday photos" && wait_for_prompt &&
       mon_raw "device_del usbstick" &&
       wait_for_line build/serial.log 'volume 1: the medium the volume lives on has left' "$WAIT_QUIET_S" &&
       type_line "delete them" && wait_for_prompt &&
       type_line "read greet.txt" && wait_for_prompt &&
       type_line "volumes Holiday photos" && wait_for_prompt &&
       type_line "files twovol.note.txt" && wait_for_prompt &&
       type_line "files greet" && wait_for_prompt &&
       mon_raw "drive_add 0 if=none,id=stick,file=$stick,format=raw" && sleep 1 &&
       mon_raw "device_add usb-storage,drive=stick,id=usbstick" &&
       wait_for_line build/serial.log 'volume 1: the volume is back, in seat' "$WAIT_QUIET_S" && wait_for_prompt &&
       type_line "files kind:greeting on Holiday photos" && wait_for_prompt; then
        rc=0
    fi
    twovol_halt first || rc=1
    [ $rc = 0 ]
    chk $? "twovolumes (first boot): the stick is taken in, named, files carried, pulled and put back, and the halt ends"

    L="$SCRATCH/serial.twovolumes.first.log"
    twovol_said "$L" 'taken in as volume 1 of the house' "the stick is taken in as volume 1 of the house"
    twovol_said "$L" 'volume 1 of the house is called "Summer photos"' "and called by the name its maker laid"
    twovol_said "$L" '^ +0  .*\(home\) — [0-9]+ file\(s\)' "volumes lists the home volume, said to be home"
    twovol_said "$L" '^ +1  Summer photos — 1 file\(s\)' "and the stick by its name"
    twovol_said "$L" '^ +1  Holiday photos — ' "volumes names the stick anew"
    twovol_said "$L" '^Moved 1 file\(s\) to Holiday photos' "carry moves a file the machine wrote onto the stick"
    twovol_said "$L" '^Copied 1 file\(s\) to .*' "and copies the stick's file home"
    twovol_said "$L" '^On Holiday photos:' "files says where a file lies, the stick by its name"
    twovol_said "$L" '^On .* \(home\):' "and the home volume"
    twovol_answer "$L" "files kind:note on 1" | grep -q "^On Holiday photos:" &&
        ! twovol_answer "$L" "files kind:note on 1" | grep -q "(home)"
    chk $? "twovolumes: a search narrowed to the stick finds its file alone"
    twovol_answer "$L" "files kind:note on Holiday photos" | grep -q "^twovol.note.txt " &&
        twovol_answer "$L" "files kind:note on Holiday photos" | grep -q "^On Holiday photos:"
    chk $? "twovolumes: and narrowed to it by its name"
    twovol_said "$L" 'volume 1: the medium the volume lives on has left — its book is held open' \
        "the stick pulled, its book is held open, the volume named"
    twovol_answer "$L" "delete them" | grep -q "^delete: it lies on Holiday photos, which is not plugged in — plug it in"
    chk $? "twovolumes: a command given a file of the pulled stick is refused once, naming it"
    twovol_answer "$L" "read greet.txt" |
        grep -q "^read: greet.txt lies on Holiday photos, which is not plugged in — plug it in"
    chk $? "twovolumes: and one given a name only the pulled stick carries"
    twovol_answer "$L" "volumes Holiday photos" |
        grep -qE "^ +1  Holiday photos — not plugged in: its [0-9]+ file\(s\) show again when it is"
    chk $? "twovolumes: volumes, the stick named by its name, says it is not plugged in"
    twovol_answer "$L" "files twovol.note.txt" | grep -q "^twovol.note.txt " &&
        ! twovol_answer "$L" "files twovol.note.txt" | grep -q "On Holiday photos" &&
        twovol_answer "$L" "files twovol.note.txt" | grep -q "^Holiday photos is not plugged in: its files show again"
    chk $? "twovolumes: files shows the home copy alone, and says the stick is not plugged in"
    twovol_answer "$L" "files greet" | grep -q "^Nothing here answers that" &&
        twovol_answer "$L" "files greet" | grep -q "^Holiday photos is not plugged in"
    chk $? "twovolumes: a word only the pulled stick answers is said to lie there"
    twovol_said "$L" 'volume 1: the volume is back, in seat [0-9]+: .* Its book is taken up' \
        "put back, its book is taken up"
    twovol_answer "$L" "files kind:greeting on Holiday photos" | grep -q "^greet.txt "
    chk $? "twovolumes: and its files are shown again"
    ! grep -aqE 'KERNEL PANIC|\[EXCEPTION\]|DEFECT' "$L"
    chk $? "twovolumes: no panic, no fault, no defect said"

    verdict=$(python3 tools/volume-read.py build/boxos.img check 2>&1 | tail -1)
    [ "$verdict" = "check: consistent — 0 damage, 0 leak, 0 note" ]
    chk $? "twovolumes: volume-read finds the home volume consistent ($verdict)"
    verdict=$(python3 tools/volume-read.py "$stick" check 2>&1 | tail -1)
    [ "$verdict" = "check: consistent — 0 damage, 0 leak, 0 note" ]
    chk $? "twovolumes: and the stick ($verdict)"
    python3 tools/volume-read.py "$stick" deed 2>/dev/null | grep -qx "name        : Holiday photos"
    chk $? "twovolumes: the stick's book keeps the name it was given"
    python3 tools/volume-read.py "$stick" get greet.txt --out "$SCRATCH/twovol.greet" >/dev/null 2>&1 &&
        [ "$(cat "$SCRATCH/twovol.greet")" = "hello from home" ]
    chk $? "twovolumes: the moved file lies on the stick with its bytes"
    ! python3 tools/volume-read.py build/boxos.img list 2>/dev/null | grep -q " greet.txt "
    chk $? "twovolumes: and is gone from home"
    python3 tools/volume-read.py build/boxos.img get twovol.note.txt --out "$SCRATCH/twovol.copied" >/dev/null 2>&1 &&
        cmp -s "$SCRATCH/twovol.copied" "$note"
    chk $? "twovolumes: the copy lies home with the stick file's bytes"
    python3 tools/volume-read.py "$stick" list 2>/dev/null | grep -q " twovol.note.txt "
    chk $? "twovolumes: and the stick keeps its own"

    if twovol_boot again && type_line "files kind:greeting" && wait_for_prompt; then :; fi
    twovol_halt again
    chk $? "twovolumes (second boot): the same stick is taken in again, and the halt ends"
    L="$SCRATCH/serial.twovolumes.again.log"
    twovol_said "$L" 'volume 1 of the house is called "Holiday photos"' "the second boot says the stick's new name"
    twovol_said "$L" '^On Holiday photos:' "and finds the moved file on it"
}
