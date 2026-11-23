# logcheck_crossings — the crossings scenario of tools/logcheck.sh: every
# start Gangway hands a kernel over, kept in the crossings file of the ESP
# (EFI/BoxOS/crossings, src/include/boxos_crossings.h, gangway_crossings.c,
# found through the FAT), marked landed by the
# kernel when the display takes the screen (src/kernel/core/boarding/
# crossing.c), and two starts in a row that did not land opening the panel by
# themselves. What lies on the medium is read on the host by
# tools/crossings.py, apart from both.
#
# SOURCED, NEVER RUN — by logcheck.sh, which dispatches `crossings` to
# run_crossings here; everything it calls (build, chk, harness, the waits, the
# keys) is stand.sh's, stand_typer.sh's or logcheck.sh's.

# ---------------------------------------------------------------------------
# crossings: the image's ESP is laid with the crossings file and keeps none. A
# UEFI start is crossing 1, kept in the file's first slot, and the kernel marks
# it landed — the file read on the host says so. Two starts are cut the moment
# the kernel
# says which crossing it is — before any display: neither lands, the second
# counted as the one before it missed. The fourth start opens the panel by
# itself; Start, and it lands, two misses before it and the panel's reason in
# it. On a legacy channel (the pc machine's IDE) the kernel marks the landing
# standing over the medium. A file whose head is gone, and an ESP with no
# file, are left as they are, said. The image is put back as it was after.
# ---------------------------------------------------------------------------

crossings_read() {
    python3 tools/crossings.py read "${1:-build/boxos.img}" 2>&1
}

# A UEFI start, waited for until `until` is said — and left running.
crossings_boot() {
    local tag=$1 until=$2
    shift 2
    make run-stop >/dev/null 2>&1
    if ! make run-bg UEFI=on CORES=2 MEM=2G "$@" >"$SCRATCH/crossings.$tag.run-bg" 2>&1; then
        harness "crossings ($tag): run-bg would not start: $(tail -1 "$SCRATCH/crossings.$tag.run-bg")"
        return 1
    fi
    wait_for_line -E build/serial.log "$until" "$WAIT_QUIET_S"
}

# A UEFI start cut the moment the kernel says which crossing it is: the log
# looked at every 20 ms, the machine frozen on the line and then taken away —
# the display is most of a second behind it.
crossings_cut() {
    local tag=$1 qemu i
    make run-stop >/dev/null 2>&1
    if ! make run-bg UEFI=on CORES=2 MEM=2G >"$SCRATCH/crossings.$tag.run-bg" 2>&1; then
        harness "crossings ($tag): run-bg would not start: $(tail -1 "$SCRATCH/crossings.$tag.run-bg")"
        return 1
    fi
    qemu=$(cat build/qemu.pid 2>/dev/null)
    for ((i = 0; i < WAIT_QUIET_S * 50; i++)); do
        grep -aq "this start is crossing [0-9]* on its medium" build/serial.log 2>/dev/null && break
        sleep 0.02
    done
    if ! grep -aq "this start is crossing [0-9]* on its medium" build/serial.log 2>/dev/null; then
        harness "crossings ($tag): the kernel never said which crossing it is"
        make run-stop >/dev/null 2>&1
        return 1
    fi
    [ -n "$qemu" ] && kill -STOP "$qemu" 2>/dev/null
    cp build/serial.log "$SCRATCH/serial.crossings.$tag.log"
    make run-stop >/dev/null 2>&1
    [ -n "$qemu" ] && kill -KILL "$qemu" 2>/dev/null
    return 0
}

# The sector of the crossings file's slot `k`, from what crossings.py read said.
crossings_slot() {
    echo $(( $(printf '%s\n' "$R" | sed -nE 's/^crossings: head at sector [0-9]+, [0-9]+ slots of 8 sectors from sector ([0-9]+)$/\1/p') + 8 * $1 ))
}

run_crossings() {
    echo "== crossings: every start kept in the ESP's crossings file, landed when the display takes the screen; two misses open the panel =="
    build
    cp build/boxos.img "$SCRATCH/crossings.before.img"

    local R L head s0 s4
    R=$(crossings_read)
    head=$(printf '%s\n' "$R" | sed -nE 's/^crossings: head at sector ([0-9]+), 32 slots of 8 sectors from sector [0-9]+$/\1/p')
    s0=$(crossings_slot 0) s4=$(crossings_slot 4)
    [ -n "$head" ] && [ "$s0" = $((head + 8)) ] && printf '%s\n' "$R" | grep -q "^crossings: 0 kept"
    chk $? "crossings (image): the image's ESP is laid with the crossings file, at sector ${head:-none}, and keeps none"

    crossings_boot first "\[Crossing\] crossing 1 "
    sleep 1
    make run-stop >/dev/null 2>&1
    L="$SCRATCH/serial.crossings.first.log"
    cp build/serial.log "$L"
    grep -aq "Gangway: this start is crossing 1, kept at sector $s0" "$L"
    chk $? "crossings (first): Gangway keeps this start as crossing 1, in the file's first slot (sector $s0)"
    grep -aqE "\[Crossing\] crossing 1 landed [0-9]+ ms after the kernel began — kept at sector $s0 of seat 0" "$L"
    chk $? "crossings (first): the kernel marks it landed when the display takes the screen ($(grep -a '\[Crossing\]' "$L" | tr -d '\r' | head -1 | cut -c12-70))"
    crossings_read | grep -qE "^crossing 1 slot 0 .* landed ms=[0-9]+ misses=0 panel=none kernel=held"
    chk $? "crossings (first): and the file, read on the host, holds it landed"

    crossings_cut second && crossings_cut third
    R=$(crossings_read)
    printf '%s\n' "$R" | grep -qE "^crossing 2 slot 1 .* not-landed misses=0 " &&
        printf '%s\n' "$R" | grep -qE "^crossing 3 slot 2 .* not-landed misses=1 "
    chk $? "crossings (cut): two starts cut before the display — neither landed, the second counting the first"

    if crossings_boot fourth "the panel is open by itself — the last 2 starts did not land"; then
        sleep 1
        mon_shot "$SCRATCH/crossings.panel.ppm"
        key_press ret
        wait_for_line -E build/serial.log "\[Crossing\] crossing 4 " "$WAIT_QUIET_S"
        sleep 1
    fi
    make run-stop >/dev/null 2>&1
    L="$SCRATCH/serial.crossings.fourth.log"
    cp build/serial.log "$L"
    grep -aq "Gangway: the last 2 starts did not land — the panel opens by itself" "$L" &&
        grep -aq "Gangway: the panel is open by itself — the last 2 starts did not land" "$L"
    chk $? "crossings (fourth): two misses in a row — the panel opens by itself, and the wire says why"
    grep -aqE "\[Crossing\] crossing 4 landed [0-9]+ ms" "$L"
    chk $? "crossings (fourth): Start — the fourth lands"
    crossings_read | grep -qE "^crossing 4 slot 3 .* landed ms=[0-9]+ misses=2 panel=misses "
    chk $? "crossings (fourth): kept with the two misses before it and the panel's reason"

    crossings_boot legacy "\[Crossing\] crossing 5 " QEMUEXTRA="-machine pc"
    sleep 1
    make run-stop >/dev/null 2>&1
    L="$SCRATCH/serial.crossings.legacy.log"
    cp build/serial.log "$L"
    grep -aq "seat 0: ATA primary master" "$L" &&
        grep -aqE "\[Crossing\] crossing 5 landed [0-9]+ ms after the kernel began — kept at sector $s4 of seat 0" "$L"
    chk $? "crossings (legacy): on an IDE channel the kernel marks the landing standing over the medium"

    local bare="$SCRATCH/crossings.bare.img" esp
    cp "$SCRATCH/crossings.before.img" "$bare"
    dd if=/dev/zero of="$bare" bs=512 seek="$head" count=8 conv=notrunc status=none
    crossings_boot bare "BoxOS Hub" BOOTDISK="$bare"
    sleep 3
    make run-stop >/dev/null 2>&1
    L="$SCRATCH/serial.crossings.bare.log"
    cp build/serial.log "$L"
    grep -aq "Gangway: no crossings are kept on this medium — the crossings file carries no head" "$L" &&
        ! grep -aqE "this start is crossing|\[Crossing\]" "$L"
    chk $? "crossings (bare): a file whose head is gone — none kept, said, and nothing for the kernel to mark"
    cmp -s <(dd if="$bare" bs=512 skip="$head" count=264 status=none) <(dd if=/dev/zero bs=512 count=264 status=none)
    chk $? "crossings (bare): and not a byte of the file is written"

    cp "$SCRATCH/crossings.before.img" "$bare"
    esp=$(python3 -c 'import struct, sys
m = open(sys.argv[1], "rb").read(512)
print(next(struct.unpack_from("<I", m, 446 + 16 * i + 8)[0] for i in range(4) if m[446 + 16 * i + 4] == 0xEF))' "$bare")
    build/fat --at "$esp" "$bare" rm EFI/BoxOS/crossings >/dev/null &&
        crossings_boot nofile "BoxOS Hub" BOOTDISK="$bare"
    sleep 3
    make run-stop >/dev/null 2>&1
    L="$SCRATCH/serial.crossings.nofile.log"
    cp build/serial.log "$L"
    grep -aq "Gangway: no crossings are kept on this medium — no ESP of the volume's medium carries EFI/BoxOS/crossings in one run" "$L" &&
        ! grep -aqE "this start is crossing|\[Crossing\]" "$L"
    chk $? "crossings (no file): an ESP carrying no crossings file — none kept, said, and nothing for the kernel to mark"
    rm -f "$bare"

    cp "$SCRATCH/crossings.before.img" build/boxos.img
}
