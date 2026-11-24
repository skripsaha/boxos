# shellcheck shell=bash
# Sourced by tools/logcheck.sh: the install scenario.

# ---------------------------------------------------------------------------
# install: BoxOS put on a disk by the program install (utils/install; the
# kernel's changes by the person's hands, box/disk.h; a volume put away,
# storage.volume.away).
#
# Through OVMF, the variable store fresh, the image the running system:
#   whole    a blank fixed disk given the whole of BoxOS — the person's files
#            along, those of kind:skip left behind, called Home, first in the
#            firmware's order — signed for by the disk's name typed;
#   stick    a blank stick given a fresh system, called Pocket: a BIOS boot
#            partition beside its ESP, so it starts through a BIOS too;
#   beside   a stick laid as Windows lays a disk — an ESP carrying Windows'
#            loader, an NTFS partition, free space after — given BoxOS beside
#            them, called Side: the ESP shared, Windows' bytes untouched.
# Then:
#   home     the machine started again from the fixed disk alone: the
#            firmware runs the option, Home is the home volume, the person's
#            file there and the one left behind not;
#   bios     the stick started alone through SeaBIOS: Pocket is home;
#   cut      a fresh install cut while it carries the system's files — the
#            machine stopped — and started again: install says where it
#            stopped, goes on, and the volume holds every file once;
#   again    the fixed disk, Home standing on it, given BoxOS anew: Home is
#            named among what goes, put away, and the disk laid anew;
#   remove   BoxOS taken off again by `install remove`, signed for: the fixed
#            disk left an empty GPT, the firmware's entry taken away; the
#            Windows stick keeping Windows' partition and loader byte for byte,
#            BoxOS's directory and partition gone; the stick's BIOS boot
#            partition gone and BoxOS's code off its boot sector.
# ---------------------------------------------------------------------------

INSTALL_DISKS="fixed cut"
INSTALL_STICKS="pocket win"

# install_device NAME — the -drive and -device of NAME: a SATA disk on the
# next AHCI port for a disk, a USB stick for a stick.
install_device() {
    local slow=""
    case " $INSTALL_SLOW " in *" $1 "*) slow=",throttling.bps-write=$INSTALL_SLOW_BPS" ;; esac
    case " $INSTALL_DISKS " in
    *" $1 "*) printf -- '-drive if=none,id=%s,file=%s,format=raw%s -device ide-hd,drive=%s,bus=ide.%s ' \
                  "$1" "$SCRATCH/install.$1.img" "$slow" "$1" "$2" ;;
    *)        printf -- '-drive if=none,id=%s,file=%s,format=raw%s -device usb-storage,drive=%s,id=%sstick ' \
                  "$1" "$SCRATCH/install.$1.img" "$slow" "$1" "$1" ;;
    esac
}

# A disk named in INSTALL_SLOW is written at INSTALL_SLOW_BPS bytes a second
# at most — the system's 123 MB carried over half a minute, so a cut lands
# while carrying however fast the host is.
INSTALL_SLOW=""
INSTALL_SLOW_BPS=4194304

# install_boot TAG UEFI BOOTDISK DEVICES... — the machine run to the shell
# with the devices named after its boot disk.
install_boot() {
    local tag=$1 uefi=$2 boot=$3 extra="" d port=1
    shift 3
    for d; do extra+=$(install_device "$d" "$port"); port=$((port + 1)); done
    make run-stop >/dev/null 2>&1
    if ! make run-bg UEFI="$uefi" USB=on CORES=4 MEM=4G BOOTDISK="$boot" QEMUEXTRA="$extra" \
            >"$SCRATCH/install.$tag.run-bg" 2>&1; then
        harness "install ($tag): run-bg would not start: $(tail -1 "$SCRATCH/install.$tag.run-bg")"
        return 1
    fi
    wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S" && wait_for_prompt
}

# install_plug NAME SECTORS — the stick NAME plugged in while the machine
# runs, after its firmware chose what to start from, and the volume on it
# taken up: a stick that starts BoxOS by itself is not started instead.
install_plug() {
    local at=$(($(wc -l < build/serial.log) + 1))
    mon_raw "drive_add 0 if=none,id=$1,file=$SCRATCH/install.$1.img,format=raw" && sleep 1 &&
        mon_raw "device_add usb-storage,drive=$1,id=${1}stick" &&
        wait_for_line -E build/serial.log "\\[USB disk [0-9]+\\] usb[0-9]+ .*: $2 sectors" "$WAIT_QUIET_S" "$at" &&
        wait_for_line -E build/serial.log "the volume on seat [0-9]+ is MOUNTED" "$WAIT_QUIET_S" "$at" &&
        wait_for_prompt
}

install_halt() {
    local tag=$1 rc=0
    type_line "bye" && wait_for_line -E build/serial.log "$STAND_HALT_ENDS" "$WAIT_QUIET_S" || rc=1
    sleep 1
    make run-stop >/dev/null 2>&1
    tr -d '\r' < build/serial.log >"$SCRATCH/serial.install.$tag.log"
    return $rc
}

install_said() {
    grep -aqE "$2" "$SCRATCH/serial.install.$1.log"
    chk $? "install ($1): $3"
}

# install_sign FROM — the name the last signing asked for after line FROM,
# typed back.
install_sign() {
    local asked=$1 name
    wait_for_line build/serial.log "Type its name to sign for it — " "$WAIT_QUIET_S" "$asked" || return 1
    name=$(tail -n +"$asked" build/serial.log | tr -d '\r' | sed -nE 's/.*Type its name to sign for it — (.*):.*/\1/p' | tail -1)
    [ -n "$name" ] && type_line "$name"
}

# install_done FROM — the installation ended after line FROM, and "start it
# now?" answered no (a stick is not asked).
install_done() {
    local asked=$1
    wait_for_line -E build/serial.log "BoxOS is installed on|The installation stopped|cannot go there" "$WAIT_QUIET_S" "$asked" &&
        tail -n +"$asked" build/serial.log | grep -aq "BoxOS is installed on" || return 1
    if wait_for_line build/serial.log "Start it now? (yes or no)" 5 "$asked"; then type_line no; fi
    wait_for_prompt
}

# install_volume IMG NAME — volume-read's word on the BoxOS volume of IMG: it
# holds, and is called NAME.
install_volume() {
    local verdict
    verdict=$(python3 tools/volume-read.py "$1" check 2>&1 | tail -1)
    [ "$verdict" = "check: consistent — 0 damage, 0 leak, 0 note" ] &&
        python3 tools/volume-read.py "$1" deed 2>/dev/null | grep -qx "name        : $2"
}

run_install() {
    echo "== install: BoxOS put on a disk — the whole of it, a stick, beside Windows; started from each; cut and gone on with; laid anew =="
    local rc at fixed pocket win cut esp head
    build
    rm -f build/edk2-vars.fd "$SCRATCH"/install.*.img "$SCRATCH"/install.*.bin
    cp build/boxos.img "$SCRATCH/install.home.img"
    python3 -c "import sys; [open(p, 'wb').truncate(int(n) << 30) for p, n in zip(sys.argv[1::2], sys.argv[2::2])]" \
        "$SCRATCH/install.fixed.img" 6 "$SCRATCH/install.pocket.img" 3 "$SCRATCH/install.cut.img" 5
    head -c 300000 /dev/urandom >"$SCRATCH/install.bootmgfw.bin"
    if ! python3 tools/disks.py lay "$SCRATCH/install.win.img" 2G --part esp:100M:"EFI system partition" \
            --part windows:600M:"Basic data partition" --fill; then
        harness "install: the Windows stick could not be laid"
        return
    fi
    if ! python3 - "$SCRATCH/install.win.img" <<'PY'
import json, subprocess, sys
img = sys.argv[1]
for p in json.loads(subprocess.check_output(["python3", "tools/disks.py", "read", img, "--json"]))["parts"]:
    if p["kind"] == "windows":
        with open(img, "r+b") as f:
            f.seek(p["first"] * 512)
            f.write(b"\xeb\x52\x90NTFS    " + bytes(499) + b"\x55\xaa")
PY
    then
        harness "install: the Windows stick's NTFS head could not be laid"
        return
    fi
    esp=$(python3 tools/disks.py read "$SCRATCH/install.win.img" --json | python3 -c 'import json, sys
p = next(p for p in json.load(sys.stdin)["parts"] if p["kind"] == "esp")
print(p["first"], (p["last"] - p["first"] + 1) * 512)')
    if ! build/fat --at "${esp% *}" "$SCRATCH/install.win.img" lay "${esp#* }" >/dev/null ||
       ! build/fat --at "${esp% *}" "$SCRATCH/install.win.img" put EFI/Microsoft/Boot/bootmgfw.efi "$SCRATCH/install.bootmgfw.bin"; then
        harness "install: the Windows stick's ESP could not be laid"
        return
    fi
    cp "$SCRATCH/install.win.img" "$SCRATCH/install.win.before"

    # ── whole, stick, beside ──────────────────────────────────────────────
    rc=1
    if install_boot first on "$SCRATCH/install.home.img" fixed pocket win && disks_ask "disks"; then
        fixed=$(disks_number "6 GB") pocket=$(disks_number "3 GB") win=$(disks_number "2 GB")
        if [ -n "$fixed" ] && [ -n "$pocket" ] && [ -n "$win" ] &&
           disks_ask "say hello from home -> mine.txt" && disks_ask "tag mine.txt kind:mine" &&
           disks_ask "say left behind -> skip.txt" && disks_ask "tag skip.txt kind:skip"; then
            at=$(($(wc -l < build/serial.log) + 1))
            type_line "install $fixed whole mine without kind:skip first called Home" && install_sign "$at" &&
                install_done "$at" &&
                at=$(($(wc -l < build/serial.log) + 1)) &&
                type_line "install $pocket whole fresh called Pocket" && install_sign "$at" && install_done "$at" &&
                at=$(($(wc -l < build/serial.log) + 1)) &&
                type_line "install $win beside fresh called Side" &&
                wait_for_line build/serial.log "Go on? (yes or no)" "$WAIT_QUIET_S" "$at" && type_line yes &&
                install_done "$at" && disks_ask "disks" && rc=0
        fi
    fi
    install_halt first || rc=1
    [ $rc = 0 ]
    chk $? "install (first start): a whole disk, a stick and a disk beside Windows given BoxOS, and the halt ends"
    install_said first '^Everything on .* goes:$' "a whole disk says what goes on it before it is signed for"
    install_said first '^  ! Windows is on this disk: turn off its Fast Startup' "beside Windows, the person is warned of Fast Startup"
    install_said first '^  laying the volume and the system on it\.\.\.$' "the steps are told as they go"
    install_said first '^    [0-9]+ of [0-9]+ file\(s\)$' "and the files carried, as they go"
    install_said first '^BoxOS is installed on .*, as the volume "Home"\.$' "the whole disk is installed, called Home"
    install_said first '^The stick starts BoxOS on any machine, through UEFI or a BIOS\.$' "the stick says it starts any machine"
    install_said first '\[EFI\] Boot[0-9A-F]{4} "BoxOS" names EFI/BoxOS/gangway.efi on GPT partition [0-9]+, first in the boot order' \
        "the fixed disk is first in the firmware's order"
    install_said first '\[DiskBoot\] seat [0-9]+: a BIOS starts BoxOS too' "the stick is given the BIOS loaders"
    install_said first 'the removable-media default left as it is: the ESP is shared' "beside Windows, the ESP is shared and its default left"
    ! grep -aqE 'KERNEL PANIC|\[EXCEPTION\]|DEFECT' "$SCRATCH/serial.install.first.log"
    chk $? "install (first start): no panic, no fault, no defect said"

    install_volume "$SCRATCH/install.fixed.img" Home
    chk $? "install: the fixed disk's volume holds, called Home"
    python3 tools/volume-read.py "$SCRATCH/install.fixed.img" list 2>/dev/null | grep -q " mine.txt " &&
        ! python3 tools/volume-read.py "$SCRATCH/install.fixed.img" list 2>/dev/null | grep -q " skip.txt " &&
        python3 tools/volume-read.py "$SCRATCH/install.fixed.img" list 2>/dev/null | grep -q " volumes.box "
    chk $? "install: the person's file went along, the one of kind:skip stayed behind, the system's utilities with them"
    python3 tools/disks.py read "$SCRATCH/install.fixed.img" --json | python3 -c 'import json, sys
t = json.load(sys.stdin)
kinds = [p["kind"] for p in t["parts"]]
esp = next(p for p in t["parts"] if p["kind"] == "esp")
sys.exit(0 if t["state"] == "gpt" and kinds == ["esp", "boxos"] and (esp["last"] - esp["first"] + 1) * 512 >= 500000000 else 1)'
    chk $? "install: the fixed disk holds a GPT, an ESP of 512 MB and BoxOS's partition"
    install_volume "$SCRATCH/install.pocket.img" Pocket &&
        python3 tools/disks.py read "$SCRATCH/install.pocket.img" --json | python3 -c 'import json, sys
sys.exit(0 if [p["kind"] for p in json.load(sys.stdin)["parts"]] == ["bios", "esp", "boxos"] else 1)'
    chk $? "install: the stick holds a BIOS boot partition, an ESP and its volume Pocket"
    install_volume "$SCRATCH/install.win.img" Side && disks_same_bytes "$SCRATCH/install.win.before" "$SCRATCH/install.win.img" >/dev/null 2>&1 ||
        python3 - "$SCRATCH/install.win.before" "$SCRATCH/install.win.img" <<'PY'
import hashlib, json, subprocess, sys
before, after = sys.argv[1:]
win = next(p for p in json.loads(subprocess.check_output(["python3", "tools/disks.py", "read", before, "--json"]))["parts"]
           if p["kind"] == "windows")
digest = lambda img: hashlib.sha256(open(img, "rb").read()[win["first"] * 512:(win["last"] + 1) * 512]).digest()
sys.exit(0 if digest(before) == digest(after) else 1)
PY
    chk $? "install: BoxOS stands beside Windows, called Side, Windows' partition byte for byte as it was"
    esp=$(python3 tools/disks.py read "$SCRATCH/install.win.img" --json | python3 -c 'import json, sys
print(next(p["first"] for p in json.load(sys.stdin)["parts"] if p["kind"] == "esp"))')
    build/fat --at "$esp" "$SCRATCH/install.win.img" get EFI/Microsoft/Boot/bootmgfw.efi "$SCRATCH/install.after.bin" 2>/dev/null &&
        cmp -s "$SCRATCH/install.after.bin" "$SCRATCH/install.bootmgfw.bin" &&
        build/fat --at "$esp" "$SCRATCH/install.win.img" get EFI/BoxOS/gangway.efi "$SCRATCH/install.loader.bin" 2>/dev/null &&
        cmp -s "$SCRATCH/install.loader.bin" build/gangway.efi
    chk $? "install: the shared ESP keeps Windows' loader byte for byte, BoxOS's in its own directory beside it"

    # ── home: the fixed disk alone ────────────────────────────────────────
    rc=1
    install_boot home on "$SCRATCH/install.fixed.img" && disks_ask "files kind:mine" && rc=0
    install_halt home || rc=1
    [ $rc = 0 ]
    chk $? "install (the fixed disk alone): the machine starts from it, and the halt ends"
    install_said home '^\[TagFS\] volume 0 of the house is called "Home"$' "the firmware runs the option, and Home is the home volume"
    install_said home '^mine\.txt ' "the person's file is there"

    # ── bios: the stick alone, through SeaBIOS ────────────────────────────
    rc=1
    install_boot bios off "$SCRATCH/install.pocket.img" && rc=0
    install_halt bios || rc=1
    [ $rc = 0 ]
    chk $? "install (the stick alone, through a BIOS): it starts, and the halt ends"
    install_said bios '^\[TagFS\] volume 0 of the house is called "Pocket"$' "the stick's volume is the home volume of a BIOS start"

    # ── cut: stopped while the system's files are carried, gone on with ───
    rc=1
    INSTALL_SLOW=cut
    if install_boot cut on "$SCRATCH/install.home.img" cut && disks_ask "disks"; then
        cut=$(disks_number "5 GB")
        at=$(($(wc -l < build/serial.log) + 1))
        [ -n "$cut" ] && type_line "install $cut whole fresh last called Cut" && install_sign "$at" &&
            wait_for_line -E build/serial.log '^    [0-9]+ of [0-9]+ file\(s\)' "$WAIT_QUIET_S" "$at" && rc=0
    fi
    make run-stop >/dev/null 2>&1
    INSTALL_SLOW=""
    tr -d '\r' < build/serial.log >"$SCRATCH/serial.install.cutfirst.log"
    if [ $rc = 0 ]; then
        rc=1
        if install_boot cutagain on "$SCRATCH/install.home.img" cut; then
            at=$(($(wc -l < build/serial.log) + 1))
            type_line "install" &&
                wait_for_line build/serial.log "Go on with it? (yes or no)" "$WAIT_QUIET_S" "$at" && type_line yes &&
                install_done "$at" && rc=0
        fi
        install_halt cutagain || rc=1
    fi
    [ $rc = 0 ]
    chk $? "install (cut): stopped while carrying, started again, gone on with to its end, and the halt ends"
    install_said cutagain '^An installation of BoxOS on .* stopped while carrying the system.s files\.$' \
        "started again, install says where the installation stopped"
    local twice
    twice=$(python3 tools/volume-read.py "$SCRATCH/install.cut.img" list 2>/dev/null | awk '/^  [^ ]/ { print $1 }' | sort | uniq -d)
    install_volume "$SCRATCH/install.cut.img" Cut && [ -z "$twice" ]
    chk $? "install (cut): the volume holds, every file of it once${twice:+ — twice: $twice}"

    # ── again: the fixed disk laid anew, Home put away; then removed ──────
    # The first round put the fixed disk first in the firmware's order, and
    # the sticks start BoxOS by themselves now: the variables begun afresh, and
    # the sticks plugged in once the machine stands on the home image.
    rm -f build/edk2-vars.fd
    rc=1
    if install_boot again on "$SCRATCH/install.home.img" fixed && install_plug pocket 6291456 &&
       install_plug win 4194304 && disks_ask "disks"; then
        fixed=$(disks_number "6 GB") pocket=$(disks_number "3 GB") win=$(disks_number "2 GB")
        at=$(($(wc -l < build/serial.log) + 1))
        [ -n "$fixed" ] && type_line "install $fixed whole fresh first called Again" && install_sign "$at" &&
            install_done "$at" && cp "$SCRATCH/install.fixed.img" "$SCRATCH/install.fixed.again" &&
            at=$(($(wc -l < build/serial.log) + 1)) &&
            type_line "install remove $fixed" && install_sign "$at" &&
            wait_for_line build/serial.log "BoxOS is taken off" "$WAIT_QUIET_S" "$at" && wait_for_prompt &&
            at=$(($(wc -l < build/serial.log) + 1)) &&
            type_line "install remove $win" && install_sign "$at" &&
            wait_for_line build/serial.log "BoxOS is taken off" "$WAIT_QUIET_S" "$at" && wait_for_prompt &&
            at=$(($(wc -l < build/serial.log) + 1)) &&
            type_line "install remove $pocket" && install_sign "$at" &&
            wait_for_line build/serial.log "BoxOS is taken off" "$WAIT_QUIET_S" "$at" && wait_for_prompt && rc=0
    fi
    install_halt again || rc=1
    [ $rc = 0 ]
    chk $? "install (again): the fixed disk given BoxOS anew, BoxOS taken off three disks, and the halt ends"
    install_said again '^  BoxOS, .* — the BoxOS volume Home, [0-9]+ file\(s\)$' "the volume standing on it is named among what goes"
    install_said again '^\[TagFS\] volume [0-9]+ is put away: let go of' "and it is put away before the disk is laid anew"
    install_volume "$SCRATCH/install.fixed.again" Again
    chk $? "install (again): the fixed disk's volume holds, called Again"

    install_said again '^What stays, as it is:$' "taking BoxOS off the Windows stick says what stays"
    install_said again '^  Windows data, .* — NTFS$' "and names Windows' partition among it"
    install_said again '\[EFI\] Boot[0-9A-F]{4}, which named EFI/BoxOS/gangway.efi on GPT partition [0-9]+, is taken away' \
        "the firmware's entry for BoxOS is taken away"
    python3 tools/disks.py read "$SCRATCH/install.fixed.img" --json | python3 -c 'import json, sys
t = json.load(sys.stdin)
sys.exit(0 if t["state"] == "gpt" and not t["parts"] else 1)'
    chk $? "install (remove): the fixed disk is left an empty GPT — its ESP held nothing but BoxOS's"
    python3 - "$SCRATCH/install.win.before" "$SCRATCH/install.win.img" <<'PY'
import hashlib, json, subprocess, sys
before, after = sys.argv[1:]
read = lambda img: json.loads(subprocess.check_output(["python3", "tools/disks.py", "read", img, "--json"]))["parts"]
win = next(p for p in read(before) if p["kind"] == "windows")
digest = lambda img: hashlib.sha256(open(img, "rb").read()[win["first"] * 512:(win["last"] + 1) * 512]).digest()
sys.exit(0 if digest(before) == digest(after) and [p["kind"] for p in read(after)] == ["esp", "windows"] else 1)
PY
    chk $? "install (remove): the Windows stick keeps its ESP and Windows' partition byte for byte, BoxOS's partition gone"
    esp=$(python3 tools/disks.py read "$SCRATCH/install.win.img" --json | python3 -c 'import json, sys
print(next(p["first"] for p in json.load(sys.stdin)["parts"] if p["kind"] == "esp"))')
    build/fat --at "$esp" "$SCRATCH/install.win.img" get EFI/Microsoft/Boot/bootmgfw.efi "$SCRATCH/install.after.bin" 2>/dev/null &&
        cmp -s "$SCRATCH/install.after.bin" "$SCRATCH/install.bootmgfw.bin" &&
        ! build/fat --at "$esp" "$SCRATCH/install.win.img" get EFI/BoxOS/gangway.efi "$SCRATCH/install.loader.bin" 2>/dev/null
    chk $? "install (remove): the shared ESP keeps Windows' loader byte for byte, BoxOS's directory gone"
    python3 - "$SCRATCH/install.pocket.img" <<'PY'
import json, subprocess, sys
img = sys.argv[1]
t = json.loads(subprocess.check_output(["python3", "tools/disks.py", "read", img, "--json"]))
mbr = open(img, "rb").read(512)
sys.exit(0 if not t["parts"] and mbr[:440] == bytes(440) and mbr[446 + 4] == 0xEE else 1)
PY
    chk $? "install (remove): the stick is left an empty GPT, its BIOS boot partition gone and BoxOS's code off its boot sector"
    ! grep -aqE 'KERNEL PANIC|\[EXCEPTION\]|DEFECT' "$SCRATCH"/serial.install.*.log
    chk $? "install: no panic, no fault, no defect said in any start"
    rm -f build/edk2-vars.fd
}
