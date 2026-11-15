# logcheck_bookboot — the bookboot scenario of tools/logcheck.sh: the kernel a
# volume's BOOT names, held by Gangway to the book before the jump (the
# Lectern reads the book, Attest holds each block of the kernel to its
# digest).
#
# SOURCED, NEVER RUN — by logcheck.sh, which dispatches `bookboot` to
# run_bookboot here; everything it calls (build, chk, bad, harness, the waits)
# is stand.sh's or logcheck.sh's. Its own file because logcheck.sh is twelve
# thousand lines.

# ---------------------------------------------------------------------------
# bookboot: Gangway jumps only into a kernel the book says its blocks are the
# ones it keeps the digests of — BOOT's first, then the kernels the book names
# by role
#
# First on the host, no machine: the Lectern Gangway links, built for the host
# (tools/gangway_stand.sh), says of the image and of every landing of
# self-test's group A what volume-read's `boot` says by its own code — the slot
# stood on, the nodes read past a copy that did not hold, the ribbon, every
# kernel tried and its verdict. Then the image as `make` lays it, booted
# through OVMF: Gangway says the kernel is attested by the book, every block of
# it, and the kernel comes up to its Hub. Then landing A7: one byte of the zeros
# past the kernel's end in its last block flipped — the kernel's bytes as they
# were, so a loader that did not hold the block to the book would load it and
# the kernel would come up as on any boot (the BSS it zeroes begins where its
# bytes end); the digest mkfs took of the block, zeros and all, no longer
# matches, and no other kernel lies on the volume. Gangway has to name the
# block, stop with every kernel it tried, and never jump: no line of the
# kernel's. Then landing A9: a second kernel the book names by the tag boot,
# bound in the Tally beside build/kernel.bin, and a byte of BOOT's code
# flipped: Gangway names BOOT's block, loads the second kernel in its place,
# attests it, and the machine comes up to its Hub on it.
# ---------------------------------------------------------------------------

# bookboot_boot LEG IMG WAIT — IMG booted through OVMF until WAIT (an
# extended pattern) or the machine falls quiet; its serial log kept as
# $SCRATCH/serial.bookboot.LEG.log.
bookboot_boot() {
    local leg=$1 img=$2 wait=$3 pid
    make run-stop >/dev/null 2>&1
    if ! make run-bg BOOTDISK="$img" STRICT=on UEFI=on >"$SCRATCH/bookboot.$leg.run-bg" 2>&1; then
        harness "bookboot ($leg): run-bg would not start the image: $(tail -1 "$SCRATCH/bookboot.$leg.run-bg")"
        return 1
    fi
    pid=$(cat build/qemu.pid 2>/dev/null)
    case "$(ps -o command= -p "$pid" 2>/dev/null)" in
        *"file=$img,"*) ;;
        *) harness "bookboot ($leg): the machine (pid $pid) was not started on $img"
           make run-stop >/dev/null 2>&1
           return 1 ;;
    esac
    wait_for_line -E build/serial.log "$wait" "$WAIT_QUIET_S"
    sleep 2
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.bookboot.$leg.log"
}

# bookboot_untouched IMG — the kernel's own bytes in IMG are build/kernel.bin's,
# and its last block is not the one mkfs laid: one line, "same N" or why not.
bookboot_untouched() {
    python3 - "$1" build/kernel.bin <<'PY'
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, "tools")
from volumeread import medium, ondisk
m = medium.Medium(sys.argv[1])
ground, _how = medium.find_ground(m, None)
boot = ondisk.ReadDeed(m.sectors(ground, ondisk.DEED_SECTORS))["boot"]
n, blocks = boot["kernel_bytes"], -(-boot["kernel_bytes"] // ondisk.BLOCK)
raw = m.sectors(ground + boot["kernel_block"] * ondisk.SECTORS_PER_BLOCK, blocks * ondisk.SECTORS_PER_BLOCK)
kernel = open(sys.argv[2], "rb").read()
if raw[:n] != kernel:
    print("the kernel's bytes differ from build/kernel.bin")
elif not any(raw[n:]):
    print("the zeros past the kernel's end are all zeros")
else:
    print("same %u byte(s), and a byte past them is not zero" % n)
PY
}

run_bookboot() {
    echo "== bookboot: the kernel BOOT names, held to the book by Gangway before the jump =="
    build STRICT=on
    local img="$SCRATCH/bookboot.img" L said blocks last vblock untouched rc

    tools/gangway_stand.sh build/boxos.img >"$SCRATCH/bookboot.stand" 2>&1
    rc=$?
    sed 's/^/  /' "$SCRATCH/bookboot.stand"
    [ $rc = 0 ] && grep -qE "^gangway: 16 passed, 0 failed" "$SCRATCH/bookboot.stand"
    chk $? "bookboot: the Lectern built on the host says what volume-read boot says, on the image and every landing of group A ($(tail -1 "$SCRATCH/bookboot.stand"))"

    # The image as built.
    rm -f "$img"
    cp build/boxos.img "$img" || { harness "bookboot: could not copy build/boxos.img"; return; }
    said=$(python3 tools/volume-read.py "$img" boot 2>&1 | tail -1)
    blocks=$(echo "$said" | sed -nE 's/^boot: the kernel the deed names is attested by the book: ([0-9]+) block\(s\)$/\1/p')
    [ -n "$blocks" ]
    chk $? "bookboot (clean): volume-read says the kernel is attested by the book ($said)"
    if bookboot_boot clean "$img" "BoxOS Hub|GANGWAY FATAL"; then
        L="$SCRATCH/serial.bookboot.clean.log"
        grep -aqF "Gangway: the book stands on slot 0 (seq 2)" "$L" &&
            grep -aqF "Gangway: the kernel the deed names is attested by the book: ${blocks:-?} block(s)" "$L"
        chk $? "bookboot (clean): Gangway says the kernel is attested by the book, all ${blocks:-?} block(s) ($(grep -a 'Gangway: the kernel the deed names' "$L" | tr -d '\r' | head -1))"
        grep -aq "Gangway: kernel header OK" "$L" && grep -aq "BoxOS Hub" "$L"
        chk $? "bookboot (clean): and jumps — the kernel comes up to its Hub"
        ! grep -aqE "PANIC|\[EXCEPTION\]|\[GANGWAY FATAL\]" "$L"
        chk $? "bookboot (clean): no panic, no fault"
    fi

    # Landing A7: a byte of the zeros past the kernel's end in its last block.
    rm -f "$img"
    if ! python3 tools/volume-read.py build/boxos.img self-test --emit A7 --out "$img" \
            >"$SCRATCH/bookboot.emit" 2>&1; then
        bad "bookboot (A7): self-test would not write the landing out: $(tail -1 "$SCRATCH/bookboot.emit")"
        return
    fi
    untouched=$(bookboot_untouched "$img")
    case "$untouched" in same*) true ;; *) false ;; esac
    chk $? "bookboot (A7): the kernel's own bytes are build/kernel.bin's, and past them its last block is not as mkfs laid it ($untouched)"
    python3 tools/volume-read.py "$img" boot >"$SCRATCH/bookboot.A7.boot" 2>&1
    said=$(grep -E "^boot: the kernel the deed names does not match the book: " "$SCRATCH/bookboot.A7.boot" | head -1)
    last=$(echo "$said" | sed -nE 's/^boot: the kernel the deed names does not match the book: block ([0-9]+) \(volume block ([0-9]+)\)$/\1/p')
    vblock=$(echo "$said" | sed -nE 's/^boot: the kernel the deed names does not match the book: block ([0-9]+) \(volume block ([0-9]+)\)$/\2/p')
    stop=$(tail -1 "$SCRATCH/bookboot.A7.boot" | sed 's/^boot: //')
    [ -n "$last" ] && [ "$last" = $(( ${blocks:-0} - 1 )) ] && case "$stop" in "no kernel on this volume may be jumped into: "*) true ;; *) false ;; esac
    chk $? "bookboot (A7): volume-read says the kernel does not match the book at its last block, and no kernel may be jumped into ($said; $stop)"
    if bookboot_boot tail "$img" "BoxOS Hub|GANGWAY FATAL"; then
        L="$SCRATCH/serial.bookboot.tail.log"
        grep -aqF "Gangway: the kernel the deed names does not match the book: block ${last:-?} (volume block ${vblock:-?})" "$L"
        chk $? "bookboot (A7): Gangway names the block — $(grep -a 'Gangway: the kernel the deed names' "$L" | tr -d '\r' | head -1)"
        grep -aqF "[GANGWAY FATAL] ${stop:-?}" "$L"
        chk $? "bookboot (A7): and stops with every kernel it tried — $(grep -a 'GANGWAY FATAL' "$L" | tr -d '\r' | head -1)"
        ! grep -aqE "Gangway: kernel header OK|Gangway: exiting boot services|\[Deed\] seat|BoxOS Hub" "$L"
        chk $? "bookboot (A7): it never jumps: no line of the kernel's"
    fi

    # Landing A9: a second kernel kept in the boot role in the Tally, BOOT's code flipped.
    rm -f "$img"
    if ! python3 tools/volume-read.py build/boxos.img self-test --emit A9 --out "$img" \
            >"$SCRATCH/bookboot.emit" 2>&1; then
        bad "bookboot (A9): self-test would not write the landing out: $(tail -1 "$SCRATCH/bookboot.emit")"
        return
    fi
    python3 tools/volume-read.py "$img" boot >"$SCRATCH/bookboot.A9.boot" 2>&1
    said=$(grep -E "^boot: the kernel the deed names does not match the book: " "$SCRATCH/bookboot.A9.boot" | head -1 | sed 's/^boot: //')
    spare=$(tail -1 "$SCRATCH/bookboot.A9.boot" | sed -nE 's/^boot: (kernel fid [0-9]+ is attested by the book: [0-9]+ block\(s\))$/\1/p')
    [ -n "$said" ] && [ -n "$spare" ]
    chk $? "bookboot (A9): volume-read says BOOT's kernel does not match the book and the second kernel is attested (${said:-none}; ${spare:-none})"
    if bookboot_boot spare "$img" "BoxOS Hub|GANGWAY FATAL"; then
        L="$SCRATCH/serial.bookboot.spare.log"
        grep -aqF "Gangway: ${said:-?}" "$L"
        chk $? "bookboot (A9): Gangway names BOOT's block — $(grep -a 'Gangway: the kernel the deed names' "$L" | tr -d '\r' | head -1)"
        grep -aqF "Gangway: ${spare:-?}" "$L"
        chk $? "bookboot (A9): and attests the kernel the book names by role — $(grep -aE 'Gangway: kernel fid [0-9]+ is attested' "$L" | tr -d '\r' | head -1)"
        grep -aq "Gangway: kernel header OK" "$L" && grep -aq "BoxOS Hub" "$L"
        chk $? "bookboot (A9): and jumps into it — the kernel comes up to its Hub"
        ! grep -aqE "PANIC|\[EXCEPTION\]|\[GANGWAY FATAL\]" "$L"
        chk $? "bookboot (A9): no panic, no fault"
    fi
    rm -f "$img"
}
