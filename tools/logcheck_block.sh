# shellcheck shell=bash
# Sourced by tools/logcheck.sh: the block4096 scenario.

# ---------------------------------------------------------------------------
# block4096: a medium addressed in 4096-byte blocks, from the firmware to a
# volume written to and booted again (boardroom.h, BOARDROOM_LOGICAL_MOST).
#
# The image is laid out for a 4Kn medium (make MEDIUM_BLOCK=4096): its MBR
# counts in 4096-byte blocks and its ESP's FAT is of 4096-byte sectors. It is
# put on a USB stick QEMU addresses in 4096-byte blocks — the one 4Kn medium
# QEMU's firmware and this kernel both reach; its IDE and AHCI disks are 512
# alone, and a SATA 4Kn drive's IDENTIFY is held in the startup suite (book,
# ata_block_sizes) — and booted through UEFI from it, a blank disk beside it.
# Gangway finds the volume in the stick's blocks, the kernel seats the stick
# with its block and reads its table in it, mounts the volume and writes to
# it — programs started, the gardener keeping its record — every write whole
# blocks (no ERR_UNALIGNED said); the halt makes it durable, volume-read
# reading the stick in its blocks finds it consistent, and a second boot of
# the same stick mounts it again and keeps the one garden.plants it had.
# ---------------------------------------------------------------------------

BLOCK4096_STICK="-device usb-storage,bus=xhci1.0,drive=stick,id=usbstick,logical_block_size=4096,physical_block_size=4096,bootindex=0"

# block4096_boot TAG — the stick booted through UEFI, run to the shell, two
# programs started and the machine halted: 0 once the halt ended.
block4096_boot() {
    local tag=$1 stick="$SCRATCH/block4096.stick.img" blank="$SCRATCH/block4096.blank.img"
    make run-stop >/dev/null 2>&1
    if ! make run-bg MEDIUM_BLOCK=4096 UEFI=on USB=on CORES=2 MEM=2G BOOTDISK="$blank" \
            QEMUEXTRA="-drive if=none,id=stick,file=$stick,format=raw $BLOCK4096_STICK" \
            >"$SCRATCH/block4096.$tag.run-bg" 2>&1; then
        harness "block4096 ($tag): run-bg would not start: $(tail -1 "$SCRATCH/block4096.$tag.run-bg")"
        return 1
    fi
    wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S" && wait_for_prompt &&
        type_line "info" && wait_for_prompt && type_line "info" && wait_for_prompt &&
        type_line "bye" && wait_for_line -E build/serial.log "$STAND_HALT_ENDS" "$WAIT_QUIET_S"
    local rc=$?
    sleep 1
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.block4096.$tag.log"
    return $rc
}

# block4096_held TAG LOG — what every boot of the stick is held to.
block4096_held() {
    local tag=$1 L=$2
    grep -aq "Gangway: disk found, block_size=4096" "$L"
    chk $? "block4096 ($tag): Gangway reads the volume in the stick's 4096-byte blocks"
    grep -aqE "\[USB disk [0-9]+\] addressed in 4096-byte blocks" "$L"
    chk $? "block4096 ($tag): the kernel seats the stick with its block"
    grep -aqE "\[Ground\] seat [0-9]+: its table is written in 4096-byte blocks" "$L"
    chk $? "block4096 ($tag): and reads its table in it"
    grep -aqE "\[TagFS\] the volume on seat [0-9]+ is MOUNTED" "$L"
    chk $? "block4096 ($tag): the volume on it is mounted"
    ! grep -aqE "ERR_UNALIGNED|not whole [0-9]+-byte blocks" "$L"
    chk $? "block4096 ($tag): no command went to the stick but whole blocks of it"
    ! grep -aqE 'KERNEL PANIC|\[EXCEPTION\]' "$L"
    chk $? "block4096 ($tag): no panic, no fault"
}

# block4096_volume TAG — the stick read back in its blocks: consistent, and
# the gardener's one record.
block4096_volume() {
    local tag=$1 stick="$SCRATCH/block4096.stick.img" verdict plants
    verdict=$(python3 tools/volume-read.py --block 4096 "$stick" check 2>&1 | tail -1)
    [ "$verdict" = "check: consistent — 0 damage, 0 leak, 0 note" ]
    chk $? "block4096 ($tag): volume-read, reading the stick in its blocks, finds the volume consistent ($verdict)"
    plants=$(python3 tools/volume-read.py --block 4096 "$stick" list 2>/dev/null | grep -c " garden.plants ")
    [ "$plants" = 1 ]
    chk $? "block4096 ($tag): the gardener keeps one record on it, found by its beginning ($plants)"
}

run_block4096() {
    echo "== block4096: a medium addressed in 4096-byte blocks — booted through UEFI, written, booted again =="
    rm -f "$SCRATCH/block4096.stick.img" "$SCRATCH/block4096.blank.img"
    if ! build_with_key MEDIUM_BLOCK=4096; then
        bad "block4096: make MEDIUM_BLOCK=4096 does not build: $(grep -aE 'ERROR|error' "$SCRATCH/build.log" | tail -1)"
        return
    fi
    grep -aq "FAT32 ESP of 4096-byte sectors" "$SCRATCH/build.log" &&
        grep -aqE "p1: LBA [0-9]+\.\.[0-9]+ of 4096-byte blocks" "$SCRATCH/build.log"
    chk $? "block4096: the image's ESP is a FAT of 4096-byte sectors and its MBR counts in 4096-byte blocks"
    cp build/boxos.img "$SCRATCH/block4096.stick.img" &&
        dd if=/dev/zero of="$SCRATCH/block4096.blank.img" bs=1048576 count=16 status=none ||
        { harness "block4096: the stick and the blank disk could not be made"; return; }

    block4096_boot first
    chk $? "block4096 (first boot): the stick boots to the shell, two programs run and the halt ends"
    block4096_held "first boot" "$SCRATCH/serial.block4096.first.log"
    block4096_volume "first boot"

    block4096_boot again
    chk $? "block4096 (second boot): the same stick boots to the shell again"
    block4096_held "second boot" "$SCRATCH/serial.block4096.again.log"
    block4096_volume "second boot"
    rm -f "$SCRATCH/block4096.stick.img" "$SCRATCH/block4096.blank.img"
}
