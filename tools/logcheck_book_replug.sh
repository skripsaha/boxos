# logcheck_book_replug — replugwrite asked of a book: `VOLUME_FORMAT=v2
# logcheck.sh replugwrite`, the stick leaving while a write is in flight and
# writers wait for the file's Helm, on a format-v2 volume (spec_v2 §3.8,
# §16.5 пп.10-12, C14, C20).
#
# SOURCED, NEVER RUN — by logcheck.sh, whose replugwrite rounds, answers,
# chunks, census and medium judge it runs (replugwrite_round and the rest),
# with the two things a book does otherwise named here: what `moved` does to
# the stick on the host, and how a return is judged — REPLUGWRITE_MOVE,
# REPLUGWRITE_MOVE_SAID and REPLUGWRITE_UP, which logcheck.sh asks in place of
# format v1's own when they are set. Its own file so logcheck.sh grows by the
# call and those three names alone.
#
#   same   the stick comes back as it left. The book is taken up — its last
#          stitch where it was sewn, nothing past it but this kernel's own
#          attempt cut short, its newest slot the one it knows, and every block
#          of data no stitch settled read back as written: every waiter
#          handed the Helm, its write whole. Any other verdict is a FAIL for a
#          stick nobody touched — QEMU keeps every write it took, so a block
#          that does not read back is a judge's defect, not the medium;
#   moved  a newer Spine slot is laid on the stick from the host while it is out
#          (slot_moved_on_host) — what a machine that wrote a Tally of it
#          leaves: the book is given up, and nothing of it reaches the changed
#          medium — no waiter's byte on it.
#
# Everything after the rounds is replugwrite's: the halt through bye,
# volume-read's roll call of the stick, each round's file cut into the chunks
# awaycheck said, and no block of a waiter's stream where a book given up
# would have written it.

# slot_moved_on_host IMG — the newest slot of the stick's Spine copied into
# another slot of the ring, sealed at a seq past every one the stick holds:
# the Tally of a machine that took the stick meanwhile. Fails when the ring has
# one slot, or none holds.
slot_moved_on_host() {
    python3 - "$1" <<'PY'
import struct, sys
sys.dont_write_bytecode = True
sys.path.insert(0, "tools")
from volumeread import medium, bookread

SECTOR = 512
path = sys.argv[1]
stand = medium.StandOnDeed(medium.Medium(path), None)
f, uuid = stand.deed["frame"], stand.deed["uuid"]
spb = 8
with open(path, "r+b") as img:
    def at(i):
        return (f["spine_block"] + i * f["spine_stride"]) * spb
    def read(vlba, n):
        img.seek((stand.start + vlba) * SECTOR)
        return img.read(n * SECTOR)
    newest, top = None, 0
    for i in range(f["spine_slots"]):
        s = read(at(i), 1)
        if bookread.SectorCheck(s, uuid, at(i)):
            seq = struct.unpack_from("<Q", s, 16)[0]
            if seq > top:
                newest, top = i, seq
    if newest is None or f["spine_slots"] < 2:
        sys.exit("slot_moved_on_host: no second slot, or none holds")
    to = 1 if newest == 0 else 0
    unit = bytearray(read(at(newest), spb))
    for k in range(spb):
        sector = bytearray(unit[k * SECTOR:(k + 1) * SECTOR])
        struct.pack_into("<Q", sector, 16, top + (1 << 20))
        unit[k * SECTOR:(k + 1) * SECTOR] = bookread.SectorSeal(bytes(sector), uuid, at(to) + k)
    img.seek((stand.start + at(to)) * SECTOR)
    img.write(bytes(unit))
PY
}

# replugwritebook_up CHANGE VERDICT — how a book's return was judged: taken,
# given, or nothing for a verdict no such stick may have.
replugwritebook_up() {
    case "$1:$2" in
        same:*"the volume is back, in seat"*"its book as this kernel left it"*) echo taken ;;
        moved:*"not as this kernel left its book"*) echo given ;;
    esac
}

run_replugwritebook() {
    local STAND_FORMAT=v2    # every make of the scenario, run-bg's too, the book's format (stand.sh make)
    local REPLUGWRITE_MOVE=slot_moved_on_host
    local REPLUGWRITE_MOVE_SAID="the host laid a newer Spine slot on it while it was out"
    local REPLUGWRITE_UP=replugwritebook_up
    echo "== replugwrite, a book: the stick of a format-v2 volume leaves while a write is in flight and writers wait for its Helm, and comes back as it left, then changed =="
    typer_selfcheck
    local r change name seed from rc last pid L="$SCRATCH/serial.replugwritebook.log"
    REPLUGWRITE_ABSENT=""
    latelaunch_on; build
    make run-stop >/dev/null 2>&1
    make run-bg USB=on CORES=4 MEM=4G >/dev/null 2>&1
    stick_from_boot_image \
        || { make run-stop >/dev/null 2>&1; latelaunch_off; return; }
    wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S"
    mon_raw "drive_add 0 if=none,id=stick,file=$SCRATCH/stick.img,format=raw"
    sleep 1
    mon_raw "device_add usb-storage,drive=stick,id=usbstick"
    if ! wait_for_line build/serial.log "the stand-in .PID 1. hands over" "$WAIT_QUIET_S"; then
        make run-stop >/dev/null 2>&1; cp build/serial.log "$L"; latelaunch_off
        harness "replugwrite, a book: the stick never became this machine's volume"
        return
    fi
    grep -aqE '^\[TagFS\] what this volume promises: its book takes' build/serial.log
    chk $? "replugwrite, a book: the stick's volume is a book this kernel writes"
    wait_for_quiet "$BOOT_BURST_QUIET_S"
    latelaunch_held build/serial.log
    chk $? "replugwrite, a book: every program the late launch made is held, not let run, when the launch is said"
    replugwrite_spoke_after build/serial.log
    chk $? "replugwrite, a book: the volume's shell speaks after the kernel says what it started — its banner past the hand-over"
    for r in $REPLUGWRITE_ROUNDS; do
        IFS=: read -r change name seed <<REPLUGWRITEBOOK_ROUND
$r
REPLUGWRITEBOOK_ROUND
        from=$(wc -l < build/serial.log)
        replugwrite_round "$change" "$name" "$seed"; rc=$?
        if [ "$rc" != 0 ]; then
            make run-stop >/dev/null 2>&1; cp build/serial.log "$L"; latelaunch_off
            last=$(tr -d '\r' < "$L" | grep -aE '^\[(QUILL|TagFS|Storage)\]' | tail -1 | cut -c1-160)
            if [ "$rc" = 1 ]; then
                harness "replugwrite, a book: the $change round could not be put to the machine (last: $last)"
            else
                bad "replugwrite, a book: the $change stick came back and the machine went quiet without $REPLUGWRITE_SILENT (last: $last)"
            fi
            return
        fi
        tail -n +$((from + 1)) build/serial.log > "$L.$name"
        replugwrite_judge "$L.$name" "$change" "$name" "$seed"
    done
    from=$(wc -l < build/serial.log)
    pid=$(cat build/qemu.pid)
    type_line bye && wait_for_line -E build/serial.log "$STAND_HALT_ENDS" "$WAIT_QUIET_S" $((from + 1))
    wait_until "$WAIT_QUIET_S" "the machine to switch itself off" "! kill -0 $pid 2>/dev/null"
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$L"
    latelaunch_off
    grep -aq '^\[HALT\] Storage sync complete' "$L"
    chk $? "replugwrite, a book: the machine halted through bye and its last cycle settled the book ($(grep -aE "$STAND_HALT_ENDS" "$L" | tr -d '\r' | tail -1 | cut -c1-100))"
    ! grep -aqF 'this kernel reads such a volume again from its book' "$L"
    chk $? "replugwrite, a book: no return is read again for being a book's"
    python3 tools/volume-read.py "$SCRATCH/stick.img" check --json >"$SCRATCH/replugwritebook.check.json" 2>/dev/null
    replugwrite_census "$SCRATCH/replugwritebook.check.json" "$L"
    replugwrite_medium "$SCRATCH/stick.img" "$REPLUGWRITE_ROUNDS" "$L"
    ! grep -aqE 'KERNEL PANIC|\[EXCEPTION\]|^\[NIGHTWATCH\] VERDICT: .* a defect' "$L"
    chk $? "replugwrite, a book: no panic, no exception, nothing owed proved unserved"
}

# moved_on_host IMG — the stick moved on by the host while it was out, as its
# format moves (stand_format): format v1's Ledger (ledger_moved_on_host), a
# book's newest Spine slot (slot_moved_on_host, logcheck_book_replug.sh).
moved_on_host() {
    if [ "$(stand_format)" = v2 ]; then slot_moved_on_host "$1"; else ledger_moved_on_host "$1"; fi
}

# The line the kernel says of a stick in seat 1 that came back moved on, its
# book given up (away.c, AWAY_OTHER) — by the stick's format.
moved_on_said() {
    if [ "$(stand_format)" = v2 ]; then
        echo "came back in seat 1 not as this kernel left its book"
    else
        echo "came back in seat 1 with a Ledger or a Deed this kernel did not leave on it"
    fi
}
