# logcheck_bookboot — the bookboot scenario of tools/logcheck.sh: the kernel a
# format-v2 volume's BOOT names, held by TagBoot to the book before the jump
# (spec_v2 §16.7 пп.4, 6 — the Lectern reads the book, Attest holds each block
# of the kernel to its digest).
#
# SOURCED, NEVER RUN — by logcheck.sh, which dispatches `bookboot` to
# run_bookboot here; everything it calls (build, chk, bad, harness, the waits)
# is stand.sh's or logcheck.sh's. Its own file because logcheck.sh is twelve
# thousand lines.

# ---------------------------------------------------------------------------
# bookboot: TagBoot jumps into the kernel BOOT names only when the book says
# its blocks are the ones it keeps the digests of
#
# First on the host, no machine: the Lectern TagBoot links, built for the host
# (tools/tagboot_stand.sh), says of the image and of every landing of
# self-test's group A what volume-read's `boot` says by its own code — the slot
# stood on, the nodes read past a copy that did not hold, and the verdict.
# Then the image as `make` lays it, booted through OVMF: TagBoot says the
# kernel is attested by the book, every block of it, and the kernel comes up
# to its Hub. Then landing A7: one byte of the zeros past the kernel's end in
# its last block flipped — the kernel's bytes as they were, so a loader that
# did not hold the block to the book would load it and the kernel would come
# up as on any boot (the BSS it zeroes begins where its bytes end); the digest
# mkfs took of the block, zeros and all, no longer matches. TagBoot has to name
# the block, stop, and never jump: no line of the kernel's.
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
    echo "== bookboot: the kernel BOOT names, held to the book by TagBoot before the jump =="
    build VOLUME_FORMAT=v2 STRICT=on
    local img="$SCRATCH/bookboot.img" L said blocks last vblock untouched rc

    tools/tagboot_stand.sh build/boxos.img >"$SCRATCH/bookboot.stand" 2>&1
    rc=$?
    sed 's/^/  /' "$SCRATCH/bookboot.stand"
    [ $rc = 0 ] && grep -qE "^tagboot: 9 passed, 0 failed" "$SCRATCH/bookboot.stand"
    chk $? "bookboot: the Lectern built on the host says what volume-read boot says, on the image and every landing of group A ($(tail -1 "$SCRATCH/bookboot.stand"))"

    # The image as built.
    rm -f "$img"
    cp build/boxos.img "$img" || { harness "bookboot: could not copy build/boxos.img"; return; }
    said=$(python3 tools/volume-read.py "$img" boot 2>&1 | tail -1)
    blocks=$(echo "$said" | sed -nE 's/^boot: the kernel the deed names is attested by the book: ([0-9]+) block\(s\)$/\1/p')
    [ -n "$blocks" ]
    chk $? "bookboot (clean): volume-read says the kernel is attested by the book ($said)"
    if bookboot_boot clean "$img" "BoxOS Hub|TAGBOOT FATAL"; then
        L="$SCRATCH/serial.bookboot.clean.log"
        grep -aqF "TagBoot: the book stands on slot 0 (seq 2)" "$L" &&
            grep -aqF "TagBoot: the kernel the deed names is attested by the book: ${blocks:-?} block(s)" "$L"
        chk $? "bookboot (clean): TagBoot says the kernel is attested by the book, all ${blocks:-?} block(s) ($(grep -a 'TagBoot: the kernel the deed names' "$L" | tr -d '\r' | head -1))"
        grep -aq "TagBoot: kernel header OK" "$L" && grep -aq "BoxOS Hub" "$L"
        chk $? "bookboot (clean): and jumps — the kernel comes up to its Hub"
        ! grep -aqE "PANIC|\[EXCEPTION\]|\[TAGBOOT FATAL\]" "$L"
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
    said=$(python3 tools/volume-read.py "$img" boot 2>&1 | tail -1)
    last=$(echo "$said" | sed -nE 's/^boot: the kernel the deed names does not match the book: block ([0-9]+) \(volume block ([0-9]+)\)$/\1/p')
    vblock=$(echo "$said" | sed -nE 's/^boot: the kernel the deed names does not match the book: block ([0-9]+) \(volume block ([0-9]+)\)$/\2/p')
    [ -n "$last" ] && [ "$last" = $(( ${blocks:-0} - 1 )) ]
    chk $? "bookboot (A7): volume-read says the kernel does not match the book at its last block ($said)"
    if bookboot_boot tail "$img" "BoxOS Hub|TAGBOOT FATAL"; then
        L="$SCRATCH/serial.bookboot.tail.log"
        grep -aqF "TagBoot: the kernel the deed names does not match the book: block ${last:-?} (volume block ${vblock:-?})" "$L"
        chk $? "bookboot (A7): TagBoot names the block — $(grep -a 'TagBoot: the kernel the deed names' "$L" | tr -d '\r' | head -1)"
        grep -aqF "[TAGBOOT FATAL] the kernel the deed names does not match the book: block ${last:-?} (volume block ${vblock:-?})" "$L"
        chk $? "bookboot (A7): and stops with it — $(grep -a 'TAGBOOT FATAL' "$L" | tr -d '\r' | head -1)"
        ! grep -aqE "TagBoot: kernel header OK|TagBoot: exiting boot services|\[Deed\] seat|BoxOS Hub" "$L"
        chk $? "bookboot (A7): it never jumps: no line of the kernel's"
    fi
    rm -f "$img"
}
