# logcheck_book — what tools/logcheck.sh asks of a kernel that reads a
# format-v2 volume's book: the census of a planted book held to the kernel's
# own lines (census_book), and the stand's road on a book (stand_book).
#
# SOURCED, NEVER RUN — by logcheck.sh; everything it calls (build,
# census_boot, stand_boot, chk, ok, bad, harness, the waits) is stand.sh's or
# logcheck.sh's. Its own file for the reason logcheck_framerefuse.sh is: a
# harness of twelve thousand lines does not grow further for format v2.

# ---------------------------------------------------------------------------
# census_book: a book planted with one kind of damage, booted, and the kernel
# held to what volume-read predicts it makes of it (bookcensus.py KernelBook)
#
# The kernel reads the Spine's slots and the Tally of the newest one that
# reads (mount/book_mount.c), and writes nothing: every book it mounts stands
# under glass, and one with anything sewn past its Tally it turns away. For
# every landing below, volume-read's census — the second reader, written
# apart from the kernel's — says in numbers what the kernel's lines must say:
# refused and in which words, or mounted on which slot with how many files,
# which locked by fid, which ranges rotten, which copies read in the other's
# place, how many blocks held, how many shapes and verdicts said, which block
# two owners name, and the numbers given next.
# ---------------------------------------------------------------------------

# Every landing of volume-read's book self-test that plants something in the
# Spine or the Tally the kernel reads — its slots, its trees' nodes and their
# copies, a file's header and its Parcels, the FRAME — and two that hold the
# Spine's other slots to the census; the landings of the ribbon past the Tally
# need only one here (B23): this kernel turns every such book away in the same
# words.
CENSUS_BOOK_LANDINGS="B0 B1 B2 B3 B4 B4p B5 B6 B6b B7 B7b B7c B8 B9 B9b B12 B13 B25 B34 B35 B36 B37 B38 B38b
B39 B40 B41 B42 B42b B23"

# census_book_judge NAME — the prediction against the kernel's own lines, one
# "ok|bad <sentence>" line per assertion into $SCRATCH/census.NAME.verdict.
census_book_judge() {
    python3 - "$SCRATCH/census.$1.json" "$SCRATCH/serial.$1.log" \
        >"$SCRATCH/census.$1.verdict" 2>&1 <<'CENSUS_BOOK_EOF'
import json, re, sys
pred = json.load(open(sys.argv[1]))
log = open(sys.argv[2], errors="replace").read()
k = pred["kernel"]

def say(held, text):
    print(("ok " if held else "bad ") + text)

def count(pattern):
    return len(re.findall(pattern, log, re.M))

mounted = re.search(r"is MOUNTED — (\d+) file", log)
refused = re.search(r"\[TagFS\] this volume is NOT mounted: ([^\n]*)", log)
if not k["mounts"]:
    say(mounted is None and refused is not None and k["refusal"] in refused.group(1),
        "the kernel refuses the volume — %s; the census predicted: %s"
        % (refused.group(1) if refused else ("MOUNTED" if mounted else "no refusal line"), k["refusal"]))
    sys.exit(0)
say(mounted is not None and int(mounted.group(1)) == k["files"],
    "the kernel mounted the planted book — %s file(s); the census predicted %u"
    % (mounted.group(1) if mounted else "no MOUNTED line", k["files"]))
m = re.search(r"\[TagFS\] the book stands on slot (\d+) ", log)
say(m is not None and int(m.group(1)) == k["slot"],
    "it stands on slot %s; the census predicted %u" % (m.group(1) if m else "none named", k["slot"]))
glass = count(r"^\[TagFS\] the volume may only be read: ")
say(glass == k["glass"], "it gives %u reason(s) the volume may only be read; the census predicted %u"
    % (glass, k["glass"]))
locked = sorted(int(f) for f in re.findall(r"^\[TagFS\] fid (\d+) is locked: ", log, re.M))
say(locked == k["locked"], "it locks fid(s) %s by name; the census predicted %s" % (locked, k["locked"]))
rotten = re.findall(r"^\[TagFS\] (?:rotten range fid \[[^)]*\): (\d+) file\(s\) locked — the FILES|the (\w+) range)"
                    r"[^\n]*rots in both copies", log, re.M)
have = [("FILES" if n else tree) for n, tree in rotten]
say(have == [r["tree"] for r in k["rotten"]], "it says rotten range(s) of %s; the census predicted %s"
    % (have or "none", [r["tree"] for r in k["rotten"]] or "none"))
files = [int(n) for n, tree in rotten if n]
say(files == [r["count"] for r in k["rotten"] if r["tree"] == "FILES"],
    "a rotten FILES range locks %s file(s) by its parent's count; the census predicted %s"
    % (files, [r["count"] for r in k["rotten"] if r["tree"] == "FILES"]))
mended = count(r"^\[TagFS\] the \w+ node of level \d+ at blocks [0-9/]+: copy \d does not hold")
say(mended == k["mended"], "it reads %u node(s) off their second copy; the census predicted %u" % (mended, k["mended"]))
m = re.search(r"^\[TagFS\] (\d+) block\(s\) are held: ", log, re.M)
held = int(m.group(1)) if m else 0
say(held == k["held"], "it holds %u block(s); the census predicted %u" % (held, k["held"]))
shape = count(r"^\[TagFS\] the book is written against its own shape — ")
say(shape == k["shape"], "it says %u shape(s) against the format; the census predicted %u" % (shape, k["shape"]))
torn = count(r"^\[TagFS\] Spine slot \d+ at block \d+ does not hold: ")
say(torn == k["torn_slots"], "it says %u torn slot(s); the census predicted %u" % (torn, k["torn_slots"]))
fell = count(r"^\[TagFS\] slot \d+ \(seq \d+\) names a Tally that does not read")
say(fell == k["fell_back"], "it falls back %u slot(s); the census predicted %u" % (fell, k["fell_back"]))
double = sorted(int(b) for b in re.findall(r"\[TagFS FSCK\] data block (\d+) is named by [^\n]* at once", log))
say(double == sorted(k["double"]), "it names block(s) %s twice owned; the census predicted %s"
    % (double or "none", sorted(k["double"]) or "none"))
m = re.search(r"^\[TagFS\] the next file id (\d+), tag id (\d+), bookmark (\d+)", log, re.M)
have = tuple(int(x) for x in m.groups()) if m else None
want = (k["next_fid"], k["next_tag"], k["next_bookmark"])
say(have == want, "it gives next file id, tag id and bookmark %s; the census predicted %s" % (have, want))
CENSUS_BOOK_EOF
    local rc=$?
    [ $rc = 0 ] && [ -s "$SCRATCH/census.$1.verdict" ] && return 0
    echo "bad the judge could not read $SCRATCH/census.$1.json against $SCRATCH/serial.$1.log: $(tail -1 "$SCRATCH/census.$1.verdict")" \
        > "$SCRATCH/census.$1.verdict"
}

# census_book — every landing of CENSUS_BOOK_LANDINGS planted on a fresh image,
# predicted, booted and judged.
census_book() {
    local plant line
    for plant in $CENSUS_BOOK_LANDINGS; do
        census_boot "book_$plant" "$plant" || continue
        census_book_judge "book_$plant"
        while IFS= read -r line; do
            case "$line" in
                "ok "*)  ok  "census $plant: ${line#ok }" ;;
                "bad "*) bad "census $plant: ${line#bad }" ;;
            esac
        done < "$SCRATCH/census.book_$plant.verdict"
        ! grep -aqE 'KERNEL PANIC|\[EXCEPTION\]' "$SCRATCH/serial.book_$plant.log"
        chk $? "census $plant: no panic, no fault"
    done
}

# ---------------------------------------------------------------------------
# stand_book: the startup tests on a format-v2 volume
#
# The image is made in format v2 (VOLUME_FORMAT=v2) under VOLUME_TESTS=on and
# booted three times. BIOS, four cores, its seat an ATA disk; then the padded
# landing of it (self-test --emit B4p: the image's files and forty more, whose
# FILES tree is a root over two leaves) the same way, for the one test that
# needs a leaf below a root to rot (stand_tally_rotten_leaf_locked); then the
# image again under UEFI, STRICT, four cores, its seat an AHCI port — the seat
# whose reads go through the port, parked on each answer, so the rotten block
# is refused on the port's road too (stand_tally_rot_three_paths), and every
# door below the glass is knocked at in a kernel that stops at a defect. On a
# book the kernel writes nothing, so every test that writes through TagFS —
# every test written for any format — stands aside, counted (K9): the
# stand_tally suite is what runs, and the suites written for any format that
# only read.
#
# Judged: the STAND_RUN tests this tree registers walked, nothing failed,
# nothing held; every stand_tally test ran — none stood down but the leaf on
# an image whose FILES is one leaf; the port's road of the rotten block taken
# on the AHCI port and said to stand aside on the ATA disk, never skipped in
# silence; the line that says the volume stands under glass; a block with no
# digest named; the four doors below the glass knocked at and refused; the
# book's medium coming back read again; no panic, no mirror defect and no
# defect of TagFS or Settle said — a door below the glass reached by a road
# nobody shut, code of format v1 asking for a LAYOUT; BLOCKFAIL refusing as
# many reads as the tests that arm it refuse on each boot; and the halt
# saying there was nothing to make durable.
#
#   test                                        image   padded   uefi
#   stand_blockfail_flush_refused                  1        1       1   the flush it asks for
#   stand_flush_ticket                             1        1       1   the flush its ticket waits on
#   stand_medium_bell_rings_once                   1        1       1   the read of the volume's first block
#   stand_blockfail_two_ranges                     2        2       2   a read in each range
#   stand_roadstead_refusal_rings_once             0        0       1   an AHCI port's alone
#   stand_tally_rotten_leaf_locked                 0       18       0   copy 1 then copy 2: the run, then each sector
#   total                                          5       23       6
# ---------------------------------------------------------------------------

STAND_BOOK_BLOCKFAILS=5
STAND_BOOK_PADDED_BLOCKFAILS=23
STAND_BOOK_UEFI_BLOCKFAILS=6
STAND_BOOK_HALT='^\[HALT\] the volume on seat [0-9]+ stands under glass: this kernel wrote nothing to it'
STAND_BOOK_TESTS="stand_tally_files_read_whole stand_tally_mount_writes_nothing stand_tally_copy_one_rotten
stand_tally_rotten_leaf_locked stand_tally_writes_refused stand_tally_rot_three_paths
stand_tally_undigested_refused stand_tally_length_asked stand_tally_lower_doors_refused
stand_tally_medium_returns
stand_tally_live_without_rows stand_blockfail_two_ranges"
STAND_BOOK_PORT_ASIDE='^\[TESTS\] stand_tally_rot_three_paths: the port.s road stands aside — '
STAND_BOOK_DEFECT='\[(TagFS|Settle)\] DEFECT'

# stand_book_judge TAG WHAT BLOCKFAILS ASIDE PORT — one boot of a book: ASIDE
# the one stand_tally test that may stand down there, or empty; PORT "taken"
# where the seat's reads go through the port, "aside" where they cannot.
stand_book_judge() {
    local L="$SCRATCH/serial.$1.log" what=$2 want=$3 aside=$4 port=$5 said got t
    said=$(stand_summary "$L")
    echo "$said" | grep -qE "^TagFS: $STAND_RUN run, [0-9]+ passed, 0 failed, [0-9]+ skipped, 0 held under a caveat"
    chk $? "stand: $what — the $STAND_RUN tests this tree registers ran on the book, nothing failed (${said:-no summary})"
    grep -aqE '^\[TESTS\] [0-9]+ test\(s\) stood aside: this volume is format v2' "$L"
    chk $? "stand: $what — the tests the book is not for are counted aside, in so many words ($(grep -aoE '[0-9]+ test\(s\) stood aside: this volume is format v2[^—]*' "$L" | head -1))"
    for t in $STAND_BOOK_TESTS; do
        if [ "$t" = "$aside" ]; then
            grep -aqF "[TESTS] $t ran and stood down: " "$L"
            chk $? "stand: $what — $t stands down here, saying why"
        else
            ! grep -aqF "[TESTS] $t ran and stood down: " "$L" && ! grep -aqF "[TESTS] $t failed" "$L"
            chk $? "stand: $what — $t ran"
        fi
    done
    if [ "$port" = taken ]; then
        ! grep -aqE "$STAND_BOOK_PORT_ASIDE" "$L"
        chk $? "stand: $what — the rotten block is refused on the port's road too"
    else
        grep -aqE "$STAND_BOOK_PORT_ASIDE" "$L"
        chk $? "stand: $what — the port's road of the rotten block stands aside, said ($(grep -aoE 'the port.s road stands aside — [^.]*' "$L" | head -1))"
    fi
    grep -aqF "[TagFS] the volume may only be read: this kernel reads format-v2 books and writes none of them" "$L"
    chk $? "stand: $what — the book stands under glass, said"
    grep -aqE '^\[Integrity\] block [0-9]+ carries no digest in its volume.s Tally' "$L"
    chk $? "stand: $what — a block with no digest is named, its read refused"
    got=$(grep -acE '^\[TESTS\] .*, knocked at by the stand under glass, refused before a byte' "$L")
    [ "$got" = 4 ]
    chk $? "stand: $what — the four doors below the glass refuse the stand's knock ($got)"
    grep -aqE '^\[TagFS\] the format-v2 volume came back in seat [0-9]+: this kernel reads such a volume again from its book' "$L"
    chk $? "stand: $what — the book's medium came back and is read again, said"
    ! grep -aq 'KERNEL PANIC' "$L"
    chk $? "stand: $what — no panic"
    ! grep -aqE "$STAND_MIRROR_DEFECT" "$L"
    chk $? "stand: $what — every live file of the map stood as its record, with no row to disagree with"
    ! grep -aqE "$STAND_BOOK_DEFECT" "$L"
    chk $? "stand: $what — no defect of TagFS said: no door below the glass reached by a road nobody shut, no code of format v1 asked for a LAYOUT, no cycle asked ($(grep -aE "$STAND_BOOK_DEFECT" "$L" | head -1 | cut -c1-120))"
    got=$(grep -ac '^\[BLOCKFAIL\] seat [0-9]*: ' "$L")
    [ "$got" = "$want" ]
    chk $? "stand: $what — BLOCKFAIL refused $got read(s), as many as the tests that arm it refuse here ($want)"
    grep -aqE "$STAND_BOOK_HALT" "$L"
    chk $? "stand: $what — the halt through bye says the volume was given nothing to make durable"
}

# stand_book — the book's three boots (above).
stand_book() {
    local img="$SCRATCH/stand.book_padded.img" keep=${STAND_FORMAT:-}
    rm -f build/boxos.img "$img" "$SCRATCH"/serial.stand_book.log "$SCRATCH"/serial.stand_book_padded.log \
          "$SCRATCH"/serial.stand_book_uefi.log
    STAND_FORMAT=v2
    if ! build_with_key VOLUME_TESTS=on STRICT=off; then
        echo "BUILD FAILED (VOLUME_TESTS=on VOLUME_FORMAT=v2) — tail:"; tail -25 "$SCRATCH/build.log"
        bad "stand: the book — VOLUME_TESTS=on VOLUME_FORMAT=v2 does not build"
        STAND_FORMAT=$keep
        return
    fi
    stand_boot stand_book CORES=4 MEM=4G
    [ $? = 2 ] || stand_book_judge stand_book "the book, BIOS 4c" "$STAND_BOOK_BLOCKFAILS" \
        stand_tally_rotten_leaf_locked aside
    if python3 tools/volume-read.py build/boxos.img self-test --emit B4p --out "$img" \
            >"$SCRATCH/stand.book_padded.emit" 2>&1; then
        stand_boot stand_book_padded BOOTDISK="$img" CORES=4 MEM=4G
        [ $? = 2 ] || stand_book_judge stand_book_padded "the padded book, BIOS 4c" \
            "$STAND_BOOK_PADDED_BLOCKFAILS" "" aside
    else
        bad "stand: the padded book could not be planted — $(tail -1 "$SCRATCH/stand.book_padded.emit")"
    fi
    rm -f "$img" build/boxos.img
    if build_with_key VOLUME_TESTS=on STRICT=on; then
        stand_boot stand_book_uefi UEFI=on STRICT=on CORES=4 MEM=4G
        [ $? = 2 ] || stand_book_judge stand_book_uefi "the book, UEFI 4c on an AHCI port, STRICT" \
            "$STAND_BOOK_UEFI_BLOCKFAILS" stand_tally_rotten_leaf_locked taken
    else
        echo "BUILD FAILED (VOLUME_TESTS=on STRICT=on VOLUME_FORMAT=v2) — tail:"; tail -25 "$SCRATCH/build.log"
        bad "stand: the book — VOLUME_TESTS=on STRICT=on VOLUME_FORMAT=v2 does not build"
    fi
    STAND_FORMAT=$keep
    lockrank_judge "$SCRATCH/serial.stand_book.log $SCRATCH/serial.stand_book_padded.log $SCRATCH/serial.stand_book_uefi.log" \
        "stand, the book"
}
