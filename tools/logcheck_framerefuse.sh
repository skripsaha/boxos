# logcheck_framerefuse — the framerefuse scenario of tools/logcheck.sh: a
# format-v2 volume met by a kernel that cannot read its book, through either
# loader, and TagBoot alone on Deeds made again one way at a time.
#
# SOURCED, NEVER RUN — by logcheck.sh, which dispatches `framerefuse` to
# run_framerefuse here; everything it calls (build, chk, bad, harness, the
# waits, the typer, deed_uuid) is stand.sh's or logcheck.sh's. Its own file
# because logcheck.sh is twelve thousand lines and a scenario that grows with
# every step of format v2 should not grow it further.

# ---------------------------------------------------------------------------
# framerefuse: a format-v2 volume, met by a kernel that cannot read its book,
# is turned away whole — not a byte of its ground written (step 2.1)
#
# A Deed says what its book is written in by the stamp it carries: LAYOUT for
# format v1, FRAME for v2 (volume_deed.h). A kernel that meets a book it
# cannot read has one thing to do, at its format check (tagfs.c,
# volume_take_ground): refuse the volume before the mount's first write, and
# say so in words a person at the machine can act on.
#
# The image is this tree's, its volume made again by `create_tagfs
# --frame-only`: GEOMETRY · FRAME · BORN · BOOT · PACKED, the Spine's slots
# blank, the kernel at the data run's first block where BOOT names it — listed
# after the shell, so the image also shows the kernel is put first anyway —
# and stage2 boots it the way it boots any volume. It is booted on BIOS, q35
# (STRICT), every write it makes on the record (POWERCUT=on), typed at, and
# switched off by its power button, so the halt meets the refused volume too.
# Then the same once more on a volume with its book whole (`create_tagfs
# --v2`, its census clean before the boot): a kernel that cannot read a book
# turns a real one away exactly as it turns away an empty ring.
# Then the same again through UEFI (OVMF and TagBoot): the loader has to take
# the kernel where the BOOT stamp of a deed bearing FRAME names it — it used to
# stop at that deed, finding no layout in it (tagboot_deed.c, DeedTakeUp) — and
# the kernel it loads has to turn the volume away in the same words. Then
# TagBoot alone, on that image with its Deed made again one way at a time
# (framerefuse_deed): no BOOT, a LAYOUT beside the FRAME, a ring of one slot,
# 8 KiB blocks, BOOT before the data run — each refused in TagBoot's words
# before any kernel is loaded, the ground left as it was — and a FRAME of 32
# bytes, taken with a line. The FRAME is held to the one rule the kernel and
# volume-read hold it to (volume_deed.h, VolumeFrameCheck).
# The record is then read against the image as it stood before the boot:
# base + record must BE the image the machine left (cut --against), so the
# record holds every write; and not one of them may land in the volume's
# ground — counted by where each write lands, and again by what the
# classifier names it (cut --list), which has to be standing on the volume as
# format v2 to name anything at all.
#
# ‼ THE KERNEL BEFORE FORMAT v2 IS ASKED THE SAME, ONCE. FRAMEREFUSE_KERNEL=
# <kernel.bin> puts that kernel in the same image instead, booted the same
# way, and holds it to the refusal kernels before format v2 give ("carries no
# layout this kernel understands") and to the same silence on the medium. A
# receipt of step 2.1 taken by hand, not a gate: a tree of another day, built
# on the desk of this one, goes stale with the tools that build it.
# ---------------------------------------------------------------------------

FRAMEREFUSE_V2_SAYS="carries a format-v2 volume (its deed bears FRAME); this kernel cannot read a format-v2 book — boot a newer kernel"
FRAMEREFUSE_V1_SAYS="carries no layout this kernel understands"

# framerefuse_judge BEFORE RECORD FINAL — what the record says the machine
# wrote, against the image before the boot; one line, FRAMEREFUSE k=v ...
framerefuse_judge() {
    local list="$SCRATCH/framerefuse.list.json" against
    against=$(python3 tools/volume-read.py "$1" cut "$2" --against "$3" 2>&1)
    echo "  (cut --against: ${against#cut: })"
    case "$against" in "cut: base + "*) ;; *) echo "FRAMEREFUSE record=incomplete"; return ;; esac
    if ! python3 tools/volume-read.py "$1" cut "$2" --list --json >"$list" 2>"$list.err"; then
        echo "FRAMEREFUSE record=unreadable $(tail -1 "$list.err")"; return
    fi
    python3 - "$1" "$list" <<'PY'
import json, sys
sys.dont_write_bytecode = True
sys.path.insert(0, "tools")
from volumeread import medium

stand = medium.StandOnDeed(medium.Medium(sys.argv[1]), None)
lo, hi = stand.start, stand.start + stand.deed["sectors"]
writes = json.load(open(sys.argv[2]))["writes"]
inside = [w for w in writes if w["lba"] < hi and w["lba"] + w["sectors"] > lo]
named = [(w, t) for w in writes for t in w["touches"] if t["kind"] != "outside"]
for w in inside[:5]:
    print("  write %u.%u, LBA %u+%u, lands in the volume's ground %u..%u"
          % (w["epoch"], w["index"], w["lba"], w["sectors"], lo, hi - 1))
for w, t in named[:5]:
    print("  write %u.%u is named %s: %s" % (w["epoch"], w["index"], t["kind"], t["what"]))
print("FRAMEREFUSE record=whole format=%s writes=%u inside=%u named=%u"
      % (stand.format, len(writes), len(inside), len(named)))
PY
}

# framerefuse_leg KERNEL SAYS LEG MODE [KEYS...] — the image made around
# KERNEL by `create_tagfs MODE` (--frame-only: no book; --v2: a whole book),
# booted with KEYS (the leg's firmware), switched off, and judged: SAYS is the
# refusal its log must carry. LEG names the leg in every line and file.
framerefuse_leg() {
    local kernel=$1 says=$2 leg=$3 mode=$4
    shift 4
    local img="$SCRATCH/framerefuse.$leg.img" before="$SCRATCH/framerefuse.$leg.before.img"
    local rec="$SCRATCH/framerefuse.$leg.record" L="$SCRATCH/serial.framerefuse.$leg.log" pid verdict uuid
    rm -f "$img" "$before" "$rec" "$L"
    cp build/boxos.img "$img" || { harness "framerefuse ($leg): could not copy build/boxos.img"; return; }
    if ! tools/create_tagfs "$mode" "$img" src/userspace/shell/shell.bin "utility,system" \
            "$kernel" "system" >"$SCRATCH/framerefuse.$leg.mkfs.log" 2>&1; then
        bad "framerefuse ($leg): create_tagfs $mode would not make the image: $(tail -1 "$SCRATCH/framerefuse.$leg.mkfs.log")"
        return
    fi
    python3 tools/volume-read.py "$img" deed >"$SCRATCH/framerefuse.$leg.deed" 2>&1
    grep -q "^format      : v2" "$SCRATCH/framerefuse.$leg.deed"
    chk $? "framerefuse ($leg): the image's Deed is format v2 ($(grep '^spine' "$SCRATCH/framerefuse.$leg.deed"))"
    if [ "$mode" = "--v2" ]; then
        python3 tools/volume-read.py "$img" check >"$SCRATCH/framerefuse.$leg.check" 2>&1
        [ "$(tail -1 "$SCRATCH/framerefuse.$leg.check")" = "check: consistent — 0 damage, 0 leak, 0 note" ]
        chk $? "framerefuse ($leg): its book is whole before the boot — $(tail -1 "$SCRATCH/framerefuse.$leg.check")"
    fi
    python3 - "$img" <<'PY'
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, "tools")
from volumeread import medium, ondisk
m = medium.Medium(sys.argv[1])
ground, _how = medium.find_ground(m, None)
d = ondisk.ReadDeed(m.sectors(ground, ondisk.DEED_SECTORS))
sys.exit(0 if d["ok"] and d["frame"] and d["boot"] and
         d["boot"]["kernel_block"] == d["frame"]["data_block"] else 1)
PY
    chk $? "framerefuse ($leg): the kernel sits at the data run's first block, where BOOT names it, though listed second"
    cp "$img" "$before" || { harness "framerefuse ($leg): could not keep the image as it stood"; return; }

    make run-stop >/dev/null 2>&1
    if ! make run-bg BOOTDISK="$img" POWERCUT=on POWERCUT_LOG="$rec" STRICT=on "$@" \
            >"$SCRATCH/framerefuse.$leg.run-bg" 2>&1; then
        harness "framerefuse ($leg): run-bg would not start the image: $(tail -1 "$SCRATCH/framerefuse.$leg.run-bg")"
        return
    fi
    pid=$(cat build/qemu.pid 2>/dev/null)
    case "$(ps -o command= -p "$pid" 2>/dev/null)" in
        *"filename=$img,"*"filename=$rec,"*) ;;
        *) harness "framerefuse ($leg): the machine (pid $pid) was not started on $img with its record at $rec"
           make run-stop >/dev/null 2>&1
           return ;;
    esac
    if wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"; then
        sleep 2
        type_cmd "help"
        sleep 1
        key_press ret
        sleep 2
    fi
    mon_raw system_powerdown
    wait_until "$WAIT_QUIET_S" "the machine to switch itself off" "! kill -0 $pid 2>/dev/null"
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$L"

    uuid=$(deed_uuid "$before") || uuid=""
    [ -n "$uuid" ] && grep -aqE "^\[Deed\] seat [0-9]+: volume $uuid, " "$L"
    chk $? "framerefuse ($leg): the machine read the Deed of this very image (volume ${uuid:-unreadable})"
    grep -qF "$says" "$L"
    chk $? "framerefuse ($leg): the volume is refused in so many words — $(grep -oF "$says" "$L" | head -1)"
    ! grep -qE "MOUNTED — [0-9]+ file" "$L"
    chk $? "framerefuse ($leg): nothing is mounted off it"
    grep -q "BoxOS Shell" "$L" && typed_ok "$L"
    chk $? "framerefuse ($leg): the machine still reaches a shell, and answers a keystroke"
    ! grep -qE "PANIC|\[EXCEPTION\]|\[TAGBOOT FATAL\]" "$L"
    chk $? "framerefuse ($leg): no panic, no fault"

    verdict=$(framerefuse_judge "$before" "$rec" "$img")
    echo "$verdict" | grep -v "^FRAMEREFUSE "
    verdict=$(echo "$verdict" | grep "^FRAMEREFUSE ")
    echo "$verdict" | grep -q "record=whole"
    chk $? "framerefuse ($leg): base + the record is the image the machine left — every write it made is on the record (${verdict:-no verdict})"
    echo "$verdict" | grep -q "format=v2 .* inside=0 named=0$"
    chk $? "framerefuse ($leg): not one write lands in the volume's ground, by place and by name ($verdict)"
    rm -f "$img" "$before"
}

# framerefuse_after LEG — what the kernel of the tree says of the volume it
# turned away, after the refusal: the deed described with its FRAME, and
# neither the boot survey nor the halt taking it for the machine's volume.
framerefuse_after() {
    local leg=$1 L="$SCRATCH/serial.framerefuse.$1.log"
    ! grep -qF "$FRAMEREFUSE_V1_SAYS" "$L"
    chk $? "framerefuse ($leg): and not as a volume that says where nothing is"
    grep -qE "^\[Deed\] seat [0-9]+: format v2 — a Spine ring of 3 slot\(s\) 256 block\(s\) apart from block 256, and its data run from block 1024" "$L"
    chk $? "framerefuse ($leg): the deed is described with its FRAME"
    ! grep -q "carries the volume this machine is standing on" "$L"
    chk $? "framerefuse ($leg): the boot survey does not take the refused volume for the one the machine stands on"
    ! grep -qE "would not flush its write cache|WAS NOT MADE DURABLE" "$L"
    chk $? "framerefuse ($leg): the halt raises no durability alarm over a volume it never mounted"
    grep -aq '^\[HALT\] no volume is named on this machine — there is no medium to make durable' "$L"
    chk $? "framerefuse ($leg): and says instead that there is no medium to make durable"
}

# framerefuse_deed IMG EDIT — the image's Deed, both its copies, made again
# with one thing changed and sealed again, so a loader believes it and has
# only what it says to judge: noboot (no BOOT), both (a LAYOUT beside the
# FRAME), slots=N (the Spine ring's slots), block=N (GEOMETRY's block),
# bootout (BOOT one block before the data run), long (a FRAME of 32 bytes,
# its tail zero).
framerefuse_deed() {
    python3 - "$1" "$2" <<'PY'
import struct, sys
sys.dont_write_bytecode = True
sys.path.insert(0, "tools")
from volumeread import medium, ondisk

img, edit = sys.argv[1], sys.argv[2]
ground, _how = medium.find_ground(medium.Medium(img), None)
span = ondisk.DEED_SECTORS * ondisk.SECTOR
with open(img, "r+b") as f:
    f.seek(ground * ondisk.SECTOR)
    d = ondisk.ReadDeed(f.read(span))
    if not d["ok"] or not d["frame"] or not d["boot"] or not d["geometry"]:
        sys.exit("the image's head Deed is not the FRAME and BOOT create_tagfs --frame-only lays down")
    stamps, frame, boot = dict(d["stamps"]), dict(d["frame"]), dict(d["boot"])
    geo = dict(d["geometry"])
    if edit == "noboot":
        del stamps[ondisk.STAMP_BOOT]
    elif edit == "both":
        stamps[ondisk.STAMP_LAYOUT] = bytes(ondisk.VOLUME_LAYOUT.size)
    elif edit.startswith("slots="):
        frame["spine_slots"] = int(edit[6:])
    elif edit.startswith("block="):
        geo["block_bytes"] = int(edit[6:])
        stamps[ondisk.STAMP_GEOMETRY] = ondisk.VOLUME_GEOMETRY.struct.pack(
            *(geo[k] for k in ondisk.VOLUME_GEOMETRY.names))
    elif edit == "bootout":
        boot["kernel_block"] = frame["data_block"] - 1
        stamps[ondisk.STAMP_BOOT] = ondisk.VOLUME_BOOT.struct.pack(*(boot[k] for k in ondisk.VOLUME_BOOT.names))
    elif edit != "long":
        sys.exit("no edit called %s" % edit)
    body = ondisk.VOLUME_FRAME.struct.pack(*(frame[k] for k in ondisk.VOLUME_FRAME.names))
    stamps[ondisk.STAMP_FRAME] = body.ljust(32 if edit == "long" else len(body), b"\0")
    packed = b""
    for kind, payload in stamps.items():
        packed += struct.pack("<HH", kind, len(payload)) + payload
        packed = packed.ljust((len(packed) + 3) & ~3, b"\0")
    for at in (0, d["tail_sector"]):
        f.seek((ground + at) * ondisk.SECTOR)
        raw = bytearray(f.read(span))
        pb = struct.unpack_from("<H", raw, 8)[0]
        raw[pb:] = packed.ljust(span - pb, b"\0")
        struct.pack_into("<H", raw, 10, len(packed))
        f.seek((ground + at) * ondisk.SECTOR)
        f.write(ondisk.DeedSeal(raw))
PY
}

# framerefuse_tagboot EDIT SAYS [PANIC] — the framerefuse image with its Deed
# made again by framerefuse_deed EDIT, booted through UEFI. TagBoot has to say
# SAYS. With PANIC, it has to stop there with PANIC, load no kernel, and leave
# the volume's ground byte for byte as it found it (it holds no write to the
# medium at all; this is the proof that it did not need one). Without, the
# Deed is one TagBoot takes: it says SAYS and loads the kernel BOOT names, and
# the leg ends there — what that kernel makes of the volume is the legs above.
framerefuse_tagboot() {
    local edit=$1 says=$2 panic=${3:-} leg="uefi, deed $1"
    local img="$SCRATCH/framerefuse.tagboot.img" before="$SCRATCH/framerefuse.tagboot.before.img"
    local L="$SCRATCH/serial.framerefuse.tagboot.$1.log" pid same
    rm -f "$img" "$before" "$L"
    cp build/boxos.img "$img" || { harness "framerefuse ($leg): could not copy build/boxos.img"; return; }
    if ! tools/create_tagfs --frame-only "$img" src/userspace/shell/shell.bin "utility,system" \
            build/kernel.bin "system" >"$SCRATCH/framerefuse.tagboot.mkfs.log" 2>&1; then
        bad "framerefuse ($leg): create_tagfs --frame-only would not make the image: $(tail -1 "$SCRATCH/framerefuse.tagboot.mkfs.log")"
        return
    fi
    if ! framerefuse_deed "$img" "$edit" >"$SCRATCH/framerefuse.tagboot.deed.log" 2>&1; then
        harness "framerefuse ($leg): the Deed could not be made again: $(tail -1 "$SCRATCH/framerefuse.tagboot.deed.log")"
        return
    fi
    cp "$img" "$before" || { harness "framerefuse ($leg): could not keep the image as it stood"; return; }

    make run-stop >/dev/null 2>&1
    if ! make run-bg BOOTDISK="$img" STRICT=on UEFI=on >"$SCRATCH/framerefuse.tagboot.run-bg" 2>&1; then
        harness "framerefuse ($leg): run-bg would not start the image: $(tail -1 "$SCRATCH/framerefuse.tagboot.run-bg")"
        return
    fi
    pid=$(cat build/qemu.pid 2>/dev/null)
    case "$(ps -o command= -p "$pid" 2>/dev/null)" in
        *"file=$img,"*) ;;
        *) harness "framerefuse ($leg): the machine (pid $pid) was not started on $img"
           make run-stop >/dev/null 2>&1
           return ;;
    esac
    if [ -n "$panic" ]; then
        wait_for_line build/serial.log "TAGBOOT FATAL" "$WAIT_QUIET_S"
    else
        wait_for_line build/serial.log "TagBoot: kernel at volume block" "$WAIT_QUIET_S"
    fi
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$L"

    grep -aqF "TagBoot: $says" "$L"
    chk $? "framerefuse ($leg): TagBoot says $says"
    if [ -z "$panic" ]; then
        grep -aq "^TagBoot: the deed names the kernel" "$L"
        chk $? "framerefuse ($leg): and takes the kernel where the deed's BOOT names it"
        rm -f "$img" "$before"
        return
    fi
    grep -aqF "[TAGBOOT FATAL] $panic" "$L"
    chk $? "framerefuse ($leg): and stops: $panic"
    ! grep -aq "^TagBoot: kernel at volume block" "$L"
    chk $? "framerefuse ($leg): no kernel is loaded off the volume"
    same=$(python3 - "$before" "$img" <<'PY'
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, "tools")
from volumeread import medium, ondisk
m = medium.Medium(sys.argv[1])
ground, _how = medium.find_ground(m, None)
d = ondisk.ReadDeed(m.sectors(ground, ondisk.DEED_SECTORS))
a, b = open(sys.argv[1], "rb"), open(sys.argv[2], "rb")
left, at = d["sectors"] * ondisk.SECTOR, ground * ondisk.SECTOR
a.seek(at); b.seek(at)
while left > 0:
    n = min(left, 1 << 20)
    if a.read(n) != b.read(n):
        print("changed"); sys.exit()
    left -= n
print("same %u sectors" % d["sectors"])
PY
)
    case "$same" in same*) true ;; *) false ;; esac
    chk $? "framerefuse ($leg): the volume's ground is as it was before the boot (${same:-unread})"
    rm -f "$img" "$before"
}

run_framerefuse() {
    echo "== framerefuse: a format-v2 volume is refused whole by a kernel that cannot read its book, through either loader =="
    build STRICT=on
    if [ -n "${FRAMEREFUSE_KERNEL:-}" ]; then
        echo "  (the A/B receipt: $FRAMEREFUSE_KERNEL — $(tools/buildplate say "$FRAMEREFUSE_KERNEL" 2>&1 | head -1))"
        framerefuse_leg "$FRAMEREFUSE_KERNEL" "$FRAMEREFUSE_V1_SAYS" bios --frame-only
        return
    fi
    framerefuse_leg build/kernel.bin "$FRAMEREFUSE_V2_SAYS" bios --frame-only
    framerefuse_after bios
    framerefuse_leg build/kernel.bin "$FRAMEREFUSE_V2_SAYS" book --v2
    framerefuse_after book
    framerefuse_leg build/kernel.bin "$FRAMEREFUSE_V2_SAYS" uefi --frame-only UEFI=on
    local L="$SCRATCH/serial.framerefuse.uefi.log"
    grep -aqE "^TagBoot: volume on ground at sector [0-9]+, a format-v2 book with its data run from block 1024" "$L"
    chk $? "framerefuse (uefi): TagBoot reads the deed's FRAME, not a layout ($(grep -a '^TagBoot: volume on ground' "$L" | tr -d '\r' | head -1))"
    grep -aq "^TagBoot: the deed names the kernel" "$L"
    chk $? "framerefuse (uefi): and takes the kernel where the deed's BOOT names it"
    framerefuse_after uefi

    framerefuse_tagboot noboot "volume on ground at sector" "this volume's deed names no kernel"
    framerefuse_tagboot both "this deed bears both LAYOUT and FRAME, its book in two formats at once" \
        "the volume's deed cannot be read by this loader"
    framerefuse_tagboot slots=1 \
        "this deed's FRAME does not hold: its Spine ring has fewer than two slots, which is no ring" \
        "the volume's deed cannot be read by this loader"
    framerefuse_tagboot block=8192 \
        "this format-v2 volume was laid out in 8192-byte blocks and this loader speaks 4096" \
        "the volume's deed cannot be read by this loader"
    framerefuse_tagboot bootout "this deed's BOOT names blocks outside its data run" \
        "the volume's deed cannot be read by this loader"
    framerefuse_tagboot long "this deed's FRAME is 32 bytes: it is longer than the FRAME this reader knows"
}
