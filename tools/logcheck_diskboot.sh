# shellcheck shell=bash
# Sourced by tools/logcheck.sh: the diskboot scenario.

# ---------------------------------------------------------------------------
# diskboot: BoxOS made to start from a disk's ESP — the loader the volume
# keeps put on it, the crossings with it, and the firmware given a boot
# option naming the volume (boardroom/disk_boot.c, drivers/efi/efi_boot.c,
# Gangway's LoadOptions; utility disks).
#
# Through OVMF, with the variable store fresh: a blank stick is given a GPT,
# an ESP — one too small refused with its least size — a BoxOS partition, a
# BIOS boot partition and a volume, and made to start, first in the firmware's
# order, and through a BIOS too: the second stage in its BIOS boot partition,
# the boot sector's code over the protective MBR. A second stick,
# laid on the host as Windows lays a disk — an ESP whose FAT32 carries
# Windows' loader and its removable-media default — beside a BoxOS partition,
# is given a volume and made to start, last in the order. An entry that is no
# ESP, and a volume not on the disk, are refused. The halt ends.
#
# On the host the blank stick's ESP is a FAT32 three readers read clean, its
# loader the volume's gangway.efi as EFI/BoxOS/gangway.efi and as the
# removable-media default, its crossings file laid; the shared stick keeps
# Windows' loader and default byte for byte, BoxOS's beside them in its own
# directory, the default not written.
#
# Started again with its own disk and the blank stick, the firmware runs the
# option first in its order; Gangway says the volume it names and starts it —
# the stick's volume is the home volume, the disk's beside it — and the start
# is crossing 1 in the stick's ESP, landed. A BIOS start makes the stick start
# again and gives the firmware no option, said. And the stick alone, started
# by a BIOS (SeaBIOS) as the machine's one disk: its boot sector runs the
# second stage the kernel laid, which finds the volume through the GPT — the
# stick's volume is the home volume, and the hub greets.
# ---------------------------------------------------------------------------

# diskboot_stick NAME — the -drive and -device of the stick NAME.
diskboot_stick() {
    printf -- '-drive if=none,id=%s,file=%s,format=raw -device usb-storage,drive=%s,id=%sstick ' \
        "$1" "$SCRATCH/diskboot.$1.img" "$1" "$1"
}

# diskboot_boot TAG UEFI STICKS... — the machine run to the shell, through
# OVMF for UEFI on, its own disk the home image's copy.
diskboot_boot() {
    local tag=$1 uefi=$2 extra="" s
    shift 2
    for s; do extra+=$(diskboot_stick "$s"); done
    make run-stop >/dev/null 2>&1
    if ! make run-bg UEFI="$uefi" USB=on CORES=4 MEM=4G BOOTDISK="$SCRATCH/diskboot.home.img" QEMUEXTRA="$extra" \
            >"$SCRATCH/diskboot.$tag.run-bg" 2>&1; then
        harness "diskboot ($tag): run-bg would not start: $(tail -1 "$SCRATCH/diskboot.$tag.run-bg")"
        return 1
    fi
    wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S" && wait_for_prompt
}

diskboot_halt() {
    local tag=$1 rc=0
    type_line "bye" && wait_for_line -E build/serial.log "$STAND_HALT_ENDS" "$WAIT_QUIET_S" || rc=1
    sleep 1
    make run-stop >/dev/null 2>&1
    tr -d '\r' < build/serial.log >"$SCRATCH/serial.diskboot.$tag.log"
    return $rc
}

diskboot_said() {
    grep -aqE "$2" "$SCRATCH/serial.diskboot.$1.log"
    chk $? "diskboot: $3"
}

# diskboot_esp NAME — the first block of the stick NAME's ESP.
diskboot_esp() {
    python3 tools/disks.py read "$SCRATCH/diskboot.$1.img" --json | python3 -c 'import json, sys
print(next(p["first"] for p in json.load(sys.stdin)["parts"] if p["kind"] == "esp"))'
}

# diskboot_file NAME PATH OUT — the file PATH of the stick NAME's ESP into OUT.
diskboot_file() {
    build/fat --at "$(diskboot_esp "$1")" "$SCRATCH/diskboot.$1.img" get "$2" "$3" 2>/dev/null
}

# diskboot_ask CMD FIRST — `disks N boot ...` typed, its yes given, FIRST (yes
# or no) to whether BoxOS starts first, and the prompt waited for.
diskboot_ask() {
    local asked=$(($(wc -l < build/serial.log) + 1))
    type_line "$1" && wait_for_line build/serial.log "go on? (yes or no)" "$WAIT_QUIET_S" "$asked" &&
        type_line yes &&
        wait_for_line build/serial.log "Start BoxOS first when the machine starts? (yes or no)" "$WAIT_QUIET_S" "$asked" &&
        type_line "$2" && wait_for_prompt
}

# diskboot_fsck IMG — the system's own FAT checker over the FAT32 in IMG: its
# last line; its exit.
diskboot_fsck() {
    local dev rc
    if command -v fsck_msdos >/dev/null 2>&1; then
        dev=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount "$1" 2>/dev/null | awk 'NR == 1 { print $1 }')
        [ -n "$dev" ] || return 1
        fsck_msdos -n "$dev" >"$SCRATCH/diskboot.fsck" 2>&1
        rc=$?
        hdiutil detach "$dev" >/dev/null 2>&1
    else
        fsck.fat -n "$1" >"$SCRATCH/diskboot.fsck" 2>&1
        rc=$?
    fi
    tail -1 "$SCRATCH/diskboot.fsck"
    return $rc
}

run_diskboot() {
    echo "== diskboot: BoxOS made to start from a disk's ESP, and the firmware told =="
    local rc blank shared vb vs esp blocks t
    build
    rm -f build/edk2-vars.fd "$SCRATCH"/diskboot.*.img "$SCRATCH"/diskboot.*.bin
    cp build/boxos.img "$SCRATCH/diskboot.home.img"
    python3 -c "open('$SCRATCH/diskboot.blank.img', 'wb').truncate(2 << 30)"
    head -c 300000 /dev/urandom >"$SCRATCH/diskboot.bootmgfw.bin"
    head -c 200000 /dev/urandom >"$SCRATCH/diskboot.windefault.bin"
    if ! python3 tools/disks.py lay "$SCRATCH/diskboot.shared.img" 1G --part esp:200M:"EFI system partition" \
            --part boxos:600M:BoxOS >/dev/null; then
        harness "diskboot: the shared stick could not be laid"
        return
    fi
    esp=$(diskboot_esp shared)
    blocks=$(python3 tools/disks.py read "$SCRATCH/diskboot.shared.img" --json | python3 -c 'import json, sys
p = next(p for p in json.load(sys.stdin)["parts"] if p["kind"] == "esp")
print(p["last"] - p["first"] + 1)')
    if ! build/fat --at "$esp" "$SCRATCH/diskboot.shared.img" lay $((blocks * 512)) >/dev/null ||
       ! build/fat --at "$esp" "$SCRATCH/diskboot.shared.img" put EFI/Microsoft/Boot/bootmgfw.efi \
            "$SCRATCH/diskboot.bootmgfw.bin" ||
       ! build/fat --at "$esp" "$SCRATCH/diskboot.shared.img" put EFI/BOOT/BOOTX64.EFI \
            "$SCRATCH/diskboot.windefault.bin"; then
        harness "diskboot: the shared stick's ESP could not be laid"
        return
    fi

    rc=1
    if diskboot_boot first on blank shared && disks_ask "disks"; then
        blank=$(disks_number "2 GB") shared=$(disks_number "1 GB")
        if [ -n "$blank" ] && [ -n "$shared" ] &&
           disks_ask "disks $blank new" yes && disks_ask "disks $blank add efi 20M" &&
           disks_ask "disks $blank add efi 100M" yes && disks_ask "disks $blank add boxos 1800M" yes &&
           disks_ask "disks $blank add bios" yes &&
           disks_ask "volumes new $blank 1 Started" yes && vb=$(lay_laid "$blank") && [ -n "$vb" ] &&
           disks_ask "volumes new $shared 1 Shared" yes && vs=$(lay_laid "$shared") && [ -n "$vs" ] &&
           disks_ask "disks $blank boot 1 $vb" && disks_ask "disks $blank boot 0 $vs" &&
           diskboot_ask "disks $blank boot 0 $vb" yes && diskboot_ask "disks $shared boot 0 $vs" no; then
            rc=0
        fi
    fi
    diskboot_halt first || rc=1
    [ $rc = 0 ]
    chk $? "diskboot (first start): two sticks given volumes and made to start, the refusals refused, and the halt ends"

    diskboot_said first '^An EFI system partition is a FAT32, [0-9.]+ [KMG]B at the least on disk [0-9]+' \
        "an ESP too small for a FAT32 is refused, its least size said"
    diskboot_said first "^Entry 1 of disk $blank is no EFI system partition of a GPT" "an entry that is no ESP is refused"
    diskboot_said first "^Volume $vs does not stand on disk $blank" "a volume of another disk is refused"
    diskboot_said first "\\[DiskBoot\\] seat [0-9]+: BoxOS's loader, [0-9]+ bytes, put on the ESP in entry 0 as EFI/BoxOS/gangway.efi and as the removable-media default: the ESP is BoxOS's own; its crossings laid as EFI/BoxOS/crossings" \
        "the blank stick's ESP is BoxOS's own: the loader goes there as its default too, the crossings laid"
    diskboot_said first '\[EFI\] Boot[0-9A-F]{4} "BoxOS" names EFI/BoxOS/gangway.efi on GPT partition 1, first in the boot order' \
        "the firmware is given a boot option for it, first in its order"
    diskboot_said first '^Disk [0-9]+ has a BIOS boot partition in entry 2$' "the blank stick is given a BIOS boot partition"
    diskboot_said first '\[DiskBoot\] seat [0-9]+: a BIOS starts BoxOS too — stage2.bin, [0-9]+ sector\(s\), at the head of the BIOS boot partition in entry 2, and the boot sector.s code over the protective MBR, told block [0-9]+' \
        "and the BIOS loaders the volume keeps laid on it"
    diskboot_said first '^A BIOS starts it too: ' "disks says a BIOS starts it too"
    diskboot_said first "\\[DiskBoot\\] seat [0-9]+: BoxOS's loader, [0-9]+ bytes, put on the ESP in entry 0 as EFI/BoxOS/gangway.efi — the removable-media default left as it is: the ESP is shared" \
        "the shared stick's ESP is another system's too: its default is left as it is"
    diskboot_said first '\[EFI\] Boot[0-9A-F]{4} "BoxOS" names EFI/BoxOS/gangway.efi on GPT partition 1, last in the boot order' \
        "and its boot option goes last in the order, as the person said"
    ! grep -aqE 'KERNEL PANIC|\[EXCEPTION\]|DEFECT' "$SCRATCH/serial.diskboot.first.log"
    chk $? "diskboot: no panic, no fault, no defect said"

    esp=$(diskboot_esp blank)
    dd if="$SCRATCH/diskboot.blank.img" of="$SCRATCH/diskboot.esp.img" bs=512 skip="$esp" count=204800 status=none
    t=$(build/fat "$SCRATCH/diskboot.esp.img" open 2>&1) && mdir -/ -i "$SCRATCH/diskboot.esp.img" ::/ >/dev/null 2>&1 &&
        t="$t; $(diskboot_fsck "$SCRATCH/diskboot.esp.img")"
    chk $? "diskboot: the blank stick's ESP is a FAT32 the host's tool, mtools and the system's checker read clean (${t#fat: })"
    diskboot_file blank EFI/BoxOS/gangway.efi "$SCRATCH/diskboot.loader.bin" &&
        cmp -s "$SCRATCH/diskboot.loader.bin" build/gangway.efi &&
        diskboot_file blank EFI/BOOT/BOOTX64.EFI "$SCRATCH/diskboot.default.bin" &&
        cmp -s "$SCRATCH/diskboot.default.bin" build/gangway.efi
    chk $? "diskboot: its loader is the volume's gangway.efi, as EFI/BoxOS/gangway.efi and as the default"
    python3 tools/crossings.py read "$SCRATCH/diskboot.blank.img" | grep -q '^crossings: 0 kept$'
    chk $? "diskboot: and its crossings file is laid, keeping none"
    diskboot_file shared EFI/Microsoft/Boot/bootmgfw.efi "$SCRATCH/diskboot.after.bin" &&
        cmp -s "$SCRATCH/diskboot.after.bin" "$SCRATCH/diskboot.bootmgfw.bin" &&
        diskboot_file shared EFI/BOOT/BOOTX64.EFI "$SCRATCH/diskboot.after.bin" &&
        cmp -s "$SCRATCH/diskboot.after.bin" "$SCRATCH/diskboot.windefault.bin" &&
        diskboot_file shared EFI/BoxOS/gangway.efi "$SCRATCH/diskboot.loader.bin" &&
        cmp -s "$SCRATCH/diskboot.loader.bin" build/gangway.efi
    chk $? "diskboot: the shared stick keeps Windows' loader and default byte for byte, BoxOS's beside them"

    # Its own disk and the blank stick: the firmware runs the option first.
    rc=1
    diskboot_boot again on blank && disks_ask "volumes" && rc=0
    diskboot_halt again || rc=1
    [ $rc = 0 ]
    chk $? "diskboot (second start): the machine starts again with its disk and the stick, and the halt ends"
    diskboot_said again '^Gangway: the boot option names the volume [0-9a-f]{32}$' \
        "the firmware runs the boot option, and Gangway says the volume it names"
    diskboot_said again '^  0  Started \(home\) — ' "the stick's volume is the home volume"
    diskboot_said again '^  1  AHCI port 0' "and the disk's stands beside it"
    python3 tools/crossings.py read "$SCRATCH/diskboot.blank.img" | grep -qE '^crossing 1 slot 0 .* landed ms=[0-9]+ '
    chk $? "diskboot: the start is crossing 1 in the stick's ESP, landed"

    # A BIOS start: the files written again, the firmware told nothing.
    rc=1
    if diskboot_boot bios off blank && disks_ask "disks"; then
        blank=$(disks_number "2 GB") vb=$(tr -d '\r' < build/serial.log | sed -nE 's/^   entry 1  BoxOS, .* — volume ([0-9]+), Started$/\1/p' | tail -1)
        [ -n "$blank" ] && [ -n "$vb" ] && diskboot_ask "disks $blank boot 0 $vb" yes && rc=0
    fi
    diskboot_halt bios || rc=1
    [ $rc = 0 ]
    chk $? "diskboot (BIOS start): the stick made to start again, and the halt ends"
    diskboot_said bios '^This start knows no firmware to tell \(a BIOS start\): the firmware is not given a boot option$' \
        "a BIOS start gives the firmware no option, and says so"
    diskboot_said bios 'its crossings kept where they were$' "and keeps the crossings the stick holds"

    # On the host: the stick's boot sector is stage1's code, told the BIOS
    # boot partition's first block, its protective MBR entry and 0x55AA kept;
    # the partition's head is stage2.
    python3 - "$SCRATCH/diskboot.blank.img" build/stage1.bin build/stage2.bin <<'PY'
import json, struct, subprocess, sys
img, s1, s2 = sys.argv[1:]
parts = json.loads(subprocess.check_output(["python3", "tools/disks.py", "read", img, "--json"]))["parts"]
bios = next(p for p in parts if p["kind"] == "bios")
with open(img, "rb") as f:
    mbr = f.read(512)
    f.seek(bios["first"] * 512)
    head = f.read(len(open(s2, "rb").read()))
code = open(s1, "rb").read()
ok = (mbr[:432] == code[:432] and struct.unpack_from("<Q", mbr, 432)[0] == bios["first"]
      and mbr[446 + 4] == 0xEE and mbr[510:512] == b"\x55\xaa" and head == open(s2, "rb").read())
sys.exit(0 if ok else 1)
PY
    chk $? "diskboot: the stick's boot sector is stage1's code told its BIOS boot partition, the table kept, stage2 at the partition's head"

    # The stick alone, started by a BIOS as the machine's one disk.
    rc=1
    make run-stop >/dev/null 2>&1
    if make run-bg UEFI=off CORES=4 MEM=4G BOOTDISK="$SCRATCH/diskboot.blank.img" \
            >"$SCRATCH/diskboot.biosalone.run-bg" 2>&1; then
        wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S" && wait_for_prompt && rc=0
    else
        harness "diskboot (the stick alone, through a BIOS): run-bg would not start: $(tail -1 "$SCRATCH/diskboot.biosalone.run-bg")"
    fi
    diskboot_halt biosalone || rc=1
    [ $rc = 0 ]
    chk $? "diskboot (the stick alone, through a BIOS): it starts, the hub greets, and the halt ends"
    diskboot_said biosalone "^\\[TagFS\\] seat [0-9]+ carries the volume this kernel was read out of — the loader said so on its boarding pass" \
        "stage2, read off the BIOS boot partition, found the stick's volume through the GPT and named it on its boarding pass"
    diskboot_said biosalone '^\[TagFS\] volume 0 of the house is called "Started"$' \
        "the stick's volume, laid by the kernel, is the home volume of a BIOS start"
    ! grep -aqE 'KERNEL PANIC|\[EXCEPTION\]|DEFECT' "$SCRATCH/serial.diskboot.biosalone.log"
    chk $? "diskboot (the stick alone): no panic, no fault, no defect said"
    rm -f build/edk2-vars.fd
}
