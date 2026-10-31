# logcheck_book_usb — the book's third road for tools/logcheck.sh: the startup
# tests that write, on a format-v2 volume on a USB stick (stand_book, and
# standusb with VOLUME_FORMAT=v2).
#
# SOURCED, NEVER RUN — by logcheck.sh, after logcheck_book.sh, whose judge of
# a book's boot (stand_book_judge) and numbers it uses; latearrival_on, the
# stand's boots and judges are logcheck.sh's and stand.sh's. Its own file so
# logcheck_book.sh stays under the thousand lines a file of this tree keeps to.

# stand_book_usb [again] — the book's third road: the volume on a USB stick
# (BIOS, four cores, xHCI from power-on), the internal disk the loader reads
# declared not this machine's (latearrival_on), so the mount takes the stick
# and every test of the book writes through it — one command at a time, each
# step a baton on the drain core, the flush answered with sense. Judged as the
# book's other roads (stand_book_judge), its reads through the port; `again`,
# the stick the boot left is booted once more and held as standusb holds a
# second boot (stand_again): the suite ran again and nothing failed.
stand_book_usb() {
    local boot="$SCRATCH/stand.book_usb.boot.img" stick="$SCRATCH/stand.book_usb.stick.img" rc extra booted
    rm -f "$boot" "$stick" "$SCRATCH"/serial.stand_book_usb.log "$SCRATCH"/serial.stand_book_usb_again.log build/boxos.img
    latearrival_on
    if ! STAND_FORMAT=v2 build_with_key VOLUME_TESTS=on STRICT=off; then
        echo "BUILD FAILED (VOLUME_TESTS=on VOLUME_FORMAT=v2, the internal disk declared not this machine's) — tail:"
        tail -25 "$SCRATCH/build.log"
        bad "stand: the book on a USB stick — VOLUME_TESTS=on VOLUME_FORMAT=v2 does not build"
        latearrival_off
        return
    fi
    if ! cp build/boxos.img "$boot" || ! cp build/boxos.img "$stick"; then
        harness "stand: the book could not be copied to the boot disk and the stick"
        latearrival_off; rm -f "$boot" "$stick"
        return
    fi
    booted=$(stand_book_census "$stick")
    extra="-drive if=none,id=stick,file=$stick,format=raw -device usb-storage,bus=xhci1.0,drive=stick,id=usbstick"
    STAND_FORMAT=v2 stand_boot stand_book_usb BOOTDISK="$boot" USB=on CORES=4 MEM=4G QEMUEXTRA="$extra"; rc=$?
    if [ "$rc" != 2 ]; then
        stand_book_judge stand_book_usb "the book on a USB stick, BIOS 4c" "$STAND_BOOK_USB_BLOCKFAILS" \
            stand_tally_rotten_leaf_locked taken "$stick" "$booted"
        grep -aqE "^\[TESTS\] the tests that write go through seat [0-9]+ \($STANDUSB_SEAT\)" \
            "$SCRATCH/serial.stand_book_usb.log"
        chk $? "stand: the book on a USB stick — the writes' road is the stick's seat: $(grep -aoE 'go through seat [0-9]+ \([^)]*\)' "$SCRATCH/serial.stand_book_usb.log" | head -1)"
    fi
    if [ "${1:-}" = again ] && [ "$rc" = 0 ]; then
        STAND_FORMAT=v2 stand_boot stand_book_usb_again BOOTDISK="$boot" USB=on CORES=4 MEM=4G QEMUEXTRA="$extra"
        [ $? = 2 ] || stand_book_again stand_book_usb_again "the book on a USB stick, BIOS 4c"
    elif [ "${1:-}" = again ]; then
        bad "stand: the book on a USB stick, second boot — not judged: the first boot never said its halt's sync wrote the stick"
    fi
    latearrival_off
    rm -f "$boot" "$stick"
}

# stand_book_again TAG WHAT — a second boot of the book a first one left: the
# suite ran again on it, nothing failed, nothing held, no panic, no defect.
stand_book_again() {
    local L="$SCRATCH/serial.$1.log" said
    said=$(stand_summary "$L")
    echo "$said" | grep -qE "^TagFS: $STAND_RUN run, [0-9]+ passed, 0 failed, [0-9]+ skipped, 0 held under a caveat"
    chk $? "stand: $2, second boot — the $STAND_RUN tests ran again on the book it left, nothing failed (${said:-no summary})"
    ! grep -aq 'KERNEL PANIC' "$L" && [ -z "$(stand_book_defects "$L")" ]
    chk $? "stand: $2, second boot — no panic, no defect of TagFS said but the one each lever of the stand draws ($(stand_book_defects "$L" | head -1 | cut -c1-120))"
    ! grep -aqE "$STAND_MIRROR_DEFECT" "$L"
    chk $? "stand: $2, second boot — every live file of the map stood as its record"
}
