# logcheck_book_grow — a format-v2 volume taking the ground behind it, asked
# of the kernel the way grow and growcut ask it of format v1 (logcheck.sh):
#
#   VOLUME_FORMAT=v2 logcheck.sh grow     a book on a bigger medium takes the
#                         rest, after its mount has read it and before the door
#                         — its data run ends at the new far Deed, its block
#                         map counts the new ground free, volume-read reads the
#                         grown book, and the next mount finds nothing to take
#   VOLUME_FORMAT=v2 logcheck.sh growcut  C11 (spec_v2 §3.10): the power cut
#                         between the far Deed's write and the head's — built
#                         on the host from the record of the boot that grew,
#                         not emulated — leaves a volume of its old size, read
#                         whole by volume-read, and the next mount grows it
#                         from the head's own numbers
#   logcheck.sh growglass a book under glass (a FRAME longer than this kernel
#                         knows, volume-read's landing B25) on a bigger medium
#                         takes none of the ground, says why, and the boot
#                         leaves the image byte for byte as it found it; and a
#                         writable book whose Tally names a run past its far
#                         Deed (B57) takes none of it either, and says so
#
# SOURCED, NEVER RUN — by logcheck.sh; everything it calls (build_with_key,
# grow_stretch, grow_stood_on, wait_for_line, chk, ok, bad, harness) is
# stand.sh's or logcheck.sh's. Its own file for the reason logcheck_book.sh is.
#
# ‼ WHY growcut ON A BOOK IS CUT FROM A RECORD AND NOT ROLLED BACK. Format v1's
# growcut lets the growth finish, boots on, and writes the old head back on the
# host. A book's mount grows the volume before its door opens, and everything
# the boot writes after it may land in the new ground — a Quire of ribbon, a
# Tally — so a head rolled back under it leaves a book naming blocks past its
# own end: a state no power cut makes. The record (POWERCUT=on) holds the
# writes in the order the disk finished them, a flush between the far Deed and
# the head (ground_grow.c); the medium a cut between them leaves is the base
# and every write before the head's epoch, built by volume-read's `cut`.

GROW_BOOK_MORE=409600    # sectors the partition is stretched by: 200 MiB, the format-v1 scenarios' own

# grow_book_image NAME — a book built (VOLUME_FORMAT=v2, STRICT=on, the
# caller's STAND_FORMAT v2), copied to $SCRATCH/NAME.img and its partition
# stretched: "START COUNT" of the
# stretched ground, nothing when it could not be made (said).
grow_book_image() {
    local img="$SCRATCH/$1.img" said
    rm -f build/boxos.img "$img"
    if ! build_with_key VOLUME_FORMAT=v2 STRICT=on; then
        bad "$1: make VOLUME_FORMAT=v2 STRICT=on does not build: $(grep -aE 'ERROR|error' "$SCRATCH/build.log" | tail -1)"
        return 1
    fi
    cp build/boxos.img "$img" || { harness "$1: the book could not be copied to $img"; return 1; }
    said=$(grow_stretch "$img" "$GROW_BOOK_MORE") || said=""
    [ -n "$said" ] || { bad "$1: the book's image has no BoxOS partition to stretch"; rm -f "$img"; return 1; }
    echo "$said"
}

# grow_book_boot NAME IMG KEYS... — IMG booted as the boot disk (BIOS, four
# cores, STRICT) to the volume's shell, the machine stopped, the log kept as
# serial.NAME.log: 0 when the shell came up, 1 when not, 2 when no machine was
# started at all (said as the harness's).
grow_book_boot() {
    local tag=$1 img=$2; shift 2
    make run-stop >/dev/null 2>&1
    rm -f build/serial.log
    if ! make run-bg BOOTDISK="$img" STRICT=on CORES=4 MEM=4G "$@" >"$SCRATCH/runbg.$tag.log" 2>&1 ||
       [ ! -S build/qemu.mon ]; then
        harness "$tag: no machine was started — $(tail -1 "$SCRATCH/runbg.$tag.log" | tr -d '\r')"
        make run-stop >/dev/null 2>&1
        return 2
    fi
    wait_for_line -E build/serial.log 'BoxOS Hub|KERNEL PANIC' "$WAIT_QUIET_S"
    sleep 2
    make run-stop >/dev/null 2>&1
    tr -d '\r' < build/serial.log > "$SCRATCH/serial.$tag.log"
    grep -aq "BoxOS Hub" "$SCRATCH/serial.$tag.log"
}

# grow_book_line LOG ERE — the number of the first line of LOG ERE matches; 0
# when none does.
grow_book_line() { grep -anE "$2" "$1" | head -1 | cut -d: -f1 | grep -E '^[0-9]+$' || echo 0; }

# grow_book_numbers IMG — "SECTORS DATA_BLOCKS FREE FILES" of IMG as
# volume-read reads it: the Deed's length and its census's word for the
# kernel. Empty when it would not read.
grow_book_numbers() {
    python3 - "$1" <<'GROW_EOF' 2>/dev/null
import json, subprocess, sys
sys.path.insert(0, "tools")
from volumeread.medium import Medium, StandOnDeed
st = StandOnDeed(Medium(sys.argv[1]), None)
out = subprocess.run([sys.executable, "tools/volume-read.py", sys.argv[1], "check", "--json"],
                     capture_output=True, text=True).stdout
k = json.loads(out)["kernel"]
print("%u %s %s %s" % (st.deed["sectors"], k["data_blocks"], k["free"], k["files"]))
GROW_EOF
}

# grow_book_judged TAG WHAT START COUNT BEFORE — a boot that took the ground,
# held to it: BEFORE the image's numbers ahead of the boot (grow_book_numbers).
grow_book_judged() {
    local L="$SCRATCH/serial.$1.log" what=$2 start=$3 count=$4 before=$5 stood took mounted more db fb f0 added
    local d0 now sectors
    stood=$(grow_stood_on "$L" "$start" "$count") && stood=""
    [ -z "$stood" ]
    chk $? "$what: the machine stood on the stretched ground${stood:+ — it stood on $stood}"
    took=$(grow_book_line "$L" 'it takes the rest$')
    [ "$(grow_book_line "$L" 'the book stands on slot [0-9]+')" -gt 0 ] &&
        [ "$took" -gt "$(grow_book_line "$L" 'the book stands on slot [0-9]+')" ]
    chk $? "$what: the ground is taken after the book is read — the slot it stands on is said first (line $took)"
    mounted=$(grow_book_line "$L" 'is MOUNTED — [0-9]+ file')
    [ "$mounted" -gt "$took" ]
    chk $? "$what: and before the door opens — $(grep -aoE 'MOUNTED — [0-9]+ file\(s\)' "$L" | head -1)"
    more=$(grep -aoE 'its data run is [0-9]+ blocks, [0-9]+ more than it had' "$L" | head -1)
    grep -aqE 'the volume now runs [0-9]+ sectors; its data run is [0-9]+ blocks, [1-9][0-9]* more than it had, and its far copy has moved to sector [0-9]+' "$L"
    chk $? "$what: ${more:-no growth line}, the far copy moved"
    grep -aq 'is inside the data run now and has been erased' "$L"
    chk $? "$what: the deed that used to sit at the old end was erased"
    d0=$(echo "$before" | cut -d' ' -f2)
    db=$(grep -aoE '^\[TagFS\] [0-9]+ data blocks, [0-9]+ free, [0-9]+ files' "$L" | head -1 | awk '{print $2}')
    fb=$(grep -aoE '^\[TagFS\] [0-9]+ data blocks, [0-9]+ free, [0-9]+ files' "$L" | head -1 | awk '{print $5}')
    now=$(echo "$more" | grep -oE '[0-9]+' | head -1)
    [ -n "$db" ] && [ -n "$d0" ] && [ "$db" = "$now" ] && [ "$db" = "$((d0 + $(echo "$more" | grep -oE '[0-9]+' | tail -1)))" ]
    chk $? "$what: the mount's count is the grown data run — $db data blocks, $d0 before"
    [ -n "$fb" ] && [ "$fb" -le "$db" ]
    chk $? "$what: $fb free out of $db — a volume cannot have more free than it has"
    # The count line comes before the mount writes a byte, and the widened map
    # is all it counts (block_map_derived.c WidenPut): free as the old volume
    # had it, and every block the growth added.
    f0=$(echo "$before" | cut -d' ' -f3)
    added=$(echo "$more" | grep -oE '[0-9]+' | tail -1)
    [ -n "$f0" ] && [ -n "$added" ] && [ "$fb" = "$((f0 + added))" ]
    chk $? "$what: every block the growth added is free — $fb free, $f0 before and ${added:-?} more"
    sectors=$(grep -aoE 'the volume now runs [0-9]+ sectors' "$L" | head -1 | grep -oE '[0-9]+')
    [ -n "$sectors" ] && [ "$sectors" = "$((count / 8 * 8))" ]
    chk $? "$what: the volume runs the whole ground in blocks — $sectors of $count sectors"
    ! grep -aqE 'KERNEL PANIC|\[EXCEPTION\]|\[(TagFS|Settle)\] DEFECT' "$L"
    chk $? "$what: no panic, no fault, no defect said"
}

# grow_book_after IMG WHAT DB — volume-read reads the grown book: its Deed
# the whole ground, its census consistent, and the data run the kernel said.
grow_book_after() {
    local nums
    nums=$(grow_book_numbers "$1")
    python3 tools/volume-read.py "$1" check >/dev/null 2>&1
    chk $? "$2: volume-read finds the grown book consistent"
    [ -n "$nums" ] && [ "$(echo "$nums" | cut -d' ' -f2)" = "$3" ]
    chk $? "$2: and reads its data run as the kernel did — ${nums:-unread} (sectors, data blocks, free, files), the kernel $3"
}

run_growbook() {
    echo "== grow, a book: a volume of format v2 on a bigger medium takes the ground behind it after its book is read =="
    local img="$SCRATCH/growbook.img" said start count before db
    local STAND_FORMAT=v2    # every make of the scenario, run-bg's too, the book's format (stand.sh make)
    said=$(grow_book_image growbook) || { bad "growbook: no stretched book — ${said:-not built}"; buildkey_restore; return; }
    start=${said% *}; count=${said#* }
    before=$(grow_book_numbers "$img")
    [ -n "$before" ] || { bad "growbook: volume-read would not read the stretched book"; rm -f "$img"; buildkey_restore; return; }
    if ! grow_book_boot growbook "$img"; then
        bad "growbook: never reached a shell"
        rm -f "$img"; buildkey_restore; return
    fi
    grow_book_judged growbook "growbook" "$start" "$count" "$before"
    db=$(grep -aoE '^\[TagFS\] [0-9]+ data blocks' "$SCRATCH/serial.growbook.log" | head -1 | grep -oE '[0-9]+')
    grow_book_after "$img" "growbook" "$db"
    if grow_book_boot growbook2 "$img"; then
        grep -aq 'the volume fills the ground it was given' "$SCRATCH/serial.growbook2.log"
        chk $? "growbook: the next mount finds nothing left to take, and says so"
        [ "$(grep -aoE '^\[TagFS\] [0-9]+ data blocks' "$SCRATCH/serial.growbook2.log" | head -1 | grep -oE '[0-9]+')" = "$db" ]
        chk $? "growbook: and counts the same $db data blocks"
    else
        bad "growbook: the grown book would not boot a second time"
    fi
    rm -f "$img"
    buildkey_restore
}

# grow_book_head_epoch BASE RECORD START NEWTAIL — the epoch of the record the
# growth's head is written in, once its far Deed has been written at NEWTAIL
# in an earlier one; nothing when the record does not show that order.
grow_book_head_epoch() {
    python3 tools/volume-read.py "$1" cut "$2" --list --json 2>/dev/null | python3 -c '
import json, sys
start, tail = int(sys.argv[1]), int(sys.argv[2])
far = None
for w in json.load(sys.stdin)["writes"]:
    if w["lba"] == start + tail and far is None:
        far = w["epoch"]
    if w["lba"] == start and far is not None and w["epoch"] > far:
        print(w["epoch"])
        break' "$3" "$4"
}

run_growcutbook() {
    echo "== growcut, a book: a growth cut between its far Deed and its head is a volume of its old size, and the next mount grows it (C11) =="
    local img="$SCRATCH/growcutbook.img" base="$SCRATCH/growcutbook.base.img" record="$SCRATCH/growcutbook.record"
    local cut="$SCRATCH/growcutbook.cut.img" said start count before tail epoch nums rc
    local STAND_FORMAT=v2
    rm -f "$base" "$record" "$cut"
    said=$(grow_book_image growcutbook) || { bad "growcutbook: no stretched book — ${said:-not built}"; buildkey_restore; return; }
    start=${said% *}; count=${said#* }
    before=$(grow_book_numbers "$img")
    cp -c "$img" "$base" 2>/dev/null || cp "$img" "$base"
    grow_book_boot growcutbook1 "$img" POWERCUT=on POWERCUT_LOG="$record"; rc=$?
    if [ "$rc" != 0 ] || ! grep -aq 'the volume now runs' "$SCRATCH/serial.growcutbook1.log"; then
        bad "growcutbook: the boot that grows never grew the book — there is no growth to cut"
        rm -f "$img" "$base" "$record"; buildkey_restore; return
    fi
    said=$(python3 tools/volume-read.py "$base" cut "$record" --against "$img" 2>&1)
    chk $? "growcutbook: the record of the boot that grew accounts for the image it left — $said"
    # Every write of it named (blklog.py, blklog_book.py): the far Deed in the
    # new ground, the old one erased, and what lands past the old end after.
    rc=$(python3 tools/volume-read.py "$base" cut "$record" --list 2>/dev/null | grep -cE '(  |; )(unowned|free) ')
    [ "$rc" = 0 ]
    chk $? "growcutbook: every write of the record is named, the ground the growth took among them — $rc unowned or on free ground"
    tail=$(grep -aoE 'its far copy has moved to sector [0-9]+' "$SCRATCH/serial.growcutbook1.log" | head -1 | grep -oE '[0-9]+$')
    epoch=$(grow_book_head_epoch "$base" "$record" "$start" "${tail:-0}")
    [ -n "$epoch" ]
    chk $? "growcutbook: the record shows the far Deed at sector ${tail:-?} written and flushed before the head (head in epoch ${epoch:-none})"
    if [ -z "$epoch" ]; then rm -f "$img" "$base" "$record"; buildkey_restore; return; fi
    python3 tools/volume-read.py "$base" cut "$record" --epoch "$epoch" --keep none --out "$cut" \
        >"$SCRATCH/growcutbook.cut.out" 2>&1
    chk $? "growcutbook: the medium the cut between the two leaves is built — $(tail -1 "$SCRATCH/growcutbook.cut.out")"
    nums=$(grow_book_numbers "$cut")
    [ -n "$nums" ] && [ "$nums" = "$before" ]
    chk $? "growcutbook: and it is the volume of its old size, whole — ${nums:-unread} (sectors, data blocks, free, files), before the growth ${before:-unread}"
    rm -f "$img" "$base" "$record"
    if grow_book_boot growcutbook2 "$cut"; then
        grow_book_judged growcutbook2 "growcutbook, the cut booted" "$start" "$count" "$before"
        grep -aq 'its far copy agrees — the whole volume is present' "$SCRATCH/serial.growcutbook2.log"
        chk $? "growcutbook: the cut's mount found its far copy where its head puts it, the old one still standing"
    else
        bad "growcutbook: the medium a cut between the two writes leaves would not boot"
    fi
    rm -f "$cut"
    buildkey_restore
}

# grow_glass_frame BASE — the book under glass for its FRAME
# (B25): none of the ground is taken, and the image left byte for byte.
grow_glass_frame() {
    local img="$SCRATCH/growglass.img" sha L
    if ! python3 tools/volume-read.py "$1" self-test --emit B25 --out "$img" >"$SCRATCH/growglass.emit" 2>&1; then
        bad "growglass: the FRAME of 32 bytes (B25) could not be planted — $(tail -1 "$SCRATCH/growglass.emit")"
        rm -f "$img"; return
    fi
    sha=$(shasum -a 256 < "$img" | cut -d' ' -f1)
    grow_book_boot growglass "$img"
    L="$SCRATCH/serial.growglass.log"
    grep -aqE 'the volume may only be read: its FRAME is [0-9]+ bytes, longer than' "$L"
    chk $? "growglass: the book stands under glass for its FRAME"
    grep -aqE 'it takes none of the rest: it may only be read \(under glass\)' "$L"
    chk $? "growglass: and says the ground behind it is not taken, and why"
    ! grep -aqE 'it takes the rest$|the volume now runs [0-9]+ sectors' "$L"
    chk $? "growglass: no growth begins"
    grep -aqE 'is MOUNTED — [0-9]+ file' "$L"
    chk $? "growglass: the book is mounted, to be read"
    [ "$(shasum -a 256 < "$img" | cut -d' ' -f1)" = "$sha" ]
    chk $? "growglass: and the boot left the image byte for byte as it found it"
    ! grep -aqE 'KERNEL PANIC|\[EXCEPTION\]' "$L"
    chk $? "growglass: no panic, no fault"
    rm -f "$img"
}

# grow_glass_reach BASE — a writable book whose Tally locks a
# file for a run past the far Deed (B57): the run lies in the ground behind
# the volume, the lock holds only what of it lies inside, and none of the
# ground is taken — the census counts the run as it is named (GroundCensusNamed).
grow_glass_reach() {
    local img="$SCRATCH/growreach.img" before after L
    if ! python3 tools/volume-read.py "$1" self-test --emit B57 --out "$img" >"$SCRATCH/growreach.emit" 2>&1; then
        bad "growglass: the run past the far Deed (B57) could not be planted — $(tail -1 "$SCRATCH/growreach.emit")"
        rm -f "$img"; return
    fi
    before=$(grow_book_numbers "$img")
    grow_book_boot growreach "$img"
    L="$SCRATCH/serial.growreach.log"
    grep -aqE 'fid [0-9]+ is locked: it names ground outside the data run' "$L"
    chk $? "growglass: the Tally's file naming ground past the far Deed is locked"
    grep -aqE 'it takes none of the rest: its book names ground up to block [0-9]+, past the far Deed' "$L"
    chk $? "growglass: and the volume takes none of the ground behind it, for the book names it"
    ! grep -aqE 'it takes the rest$|the volume now runs [0-9]+ sectors|the volume may only be read: ' "$L"
    chk $? "growglass: no growth begins, and the volume is not under glass"
    grep -aqE 'is MOUNTED — [0-9]+ file' "$L"
    chk $? "growglass: the book is mounted"
    after=$(grow_book_numbers "$img")
    [ -n "$before" ] && [ "${after%% *}" = "${before%% *}" ]
    chk $? "growglass: and its Deed still runs ${before%% *} sectors after the boot — ${after%% *}"
    ! grep -aqE 'KERNEL PANIC|\[EXCEPTION\]|\[(TagFS|Settle)\] DEFECT' "$L"
    chk $? "growglass: no panic, no fault, no defect said"
    rm -f "$img"
}

# grow_glass_fallow BASE — a writable book whose ribbon let go a run past the
# far Deed (B48: a Born the rule of rot passes over, its CODEC run 2^62 blocks
# long): what of the run lies inside is fallow, and none of the ground behind
# is taken — the stage counts the run in the census's reach as the record
# names it (ribbon_transcribe.c RibbonStageFallow, RibbonStageLaid).
grow_glass_fallow() {
    local img="$SCRATCH/growfallow.img" before after L
    if ! python3 tools/volume-read.py "$1" self-test --emit B48 --out "$img" >"$SCRATCH/growfallow.emit" 2>&1; then
        bad "growglass: the run the ribbon let go past the far Deed (B48) could not be planted — $(tail -1 "$SCRATCH/growfallow.emit")"
        rm -f "$img"; return
    fi
    before=$(grow_book_numbers "$img")
    grow_book_boot growfallow "$img"
    L="$SCRATCH/serial.growfallow.log"
    grep -aqE 'it takes none of the rest: its book names ground up to block [0-9]+, past the far Deed' "$L"
    chk $? "growglass: the volume takes none of the ground behind it, for its ribbon let go a run that names it"
    ! grep -aqE 'it takes the rest$|the volume now runs [0-9]+ sectors|the volume may only be read: ' "$L"
    chk $? "growglass: no growth begins past the ribbon's run, and the volume is not under glass"
    grep -aqE 'is MOUNTED — [0-9]+ file' "$L"
    chk $? "growglass: the book with the run let go is mounted"
    after=$(grow_book_numbers "$img")
    [ -n "$before" ] && [ "${after%% *}" = "${before%% *}" ]
    chk $? "growglass: and its Deed still runs ${before%% *} sectors after the boot past the ribbon's run — ${after%% *}"
    ! grep -aqE 'KERNEL PANIC|\[EXCEPTION\]|\[(TagFS|Settle)\] DEFECT' "$L"
    chk $? "growglass: no panic, no fault, no defect said past the ribbon's run"
    rm -f "$img"
}

run_growglass() {
    echo "== growglass: a book on a bigger medium that may not take the ground behind it takes none of it =="
    local said
    local STAND_FORMAT=v2
    said=$(grow_book_image growglass.base) || { bad "growglass: no stretched book — ${said:-not built}"; buildkey_restore; return; }
    grow_glass_frame "$SCRATCH/growglass.base.img"
    grow_glass_reach "$SCRATCH/growglass.base.img"
    grow_glass_fallow "$SCRATCH/growglass.base.img"
    rm -f "$SCRATCH/growglass.base.img"
    buildkey_restore
}
