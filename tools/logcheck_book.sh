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
# The kernel reads the Spine's slots, the Tally of the newest one that reads
# and the ribbon sewn past it (mount/book_mount.c, ribbon_collate.c,
# ribbon_transcribe.c), and the mount writes nothing — a boot that stops
# without a halt writes nothing either. For every landing below, volume-read's census — the second reader,
# written apart from the kernel's — says in numbers what the kernel's lines
# must say: refused and in which words, or mounted on which slot with how many
# files, which locked by fid, which ranges rotten, which copies read in the
# other's place — the first few of a node one by one, the rest by their count
# — how many blocks held, how many shapes and verdicts said, which runs two
# owners name and how long, the numbers given next, and how many blocks of its
# data run its block map derives free, or that it counts none where a Tally
# names a bookmark, whose ground neither reader derives; and of the ribbon,
# every line by its census code, the stitches sewn on, the last and how many
# applied, whether the medium lied, and the blocks it let go lying fallow; and
# what memory holds when the mount is done — every live file's name, size,
# tags and runs, and the Use Context — by the print the kernel gives of it.
# ---------------------------------------------------------------------------

# Every landing of volume-read's book self-test that plants something in the
# Spine or the Tally the kernel reads — its slots, its trees' nodes and their
# copies (five of them, twelve, and three), a rotten node's ground under the
# ribbon's next Quire whole and in part, a file's header and its Parcels, two
# owners of one kind on one block, the FRAME — two that hold the Spine's other
# slots to the census, and every landing of the ribbon past the Tally: each
# kind of record and the rule it is replayed by, how the ribbon ends, goes on
# and branches, what a torn or rotten stitch costs, and the shapes no stitch
# may take; and what the medium may say that no writer does, each held to a
# bound before it is trusted (selftest_book_ribbon.py).
CENSUS_BOOK_LANDINGS="B0 B1 B2 B3 B4 B4p B5 B6 B6b B7 B7b B7c B8 B9 B9b B12 B13 B25 B34 B35 B36 B37 B38 B38b
B38c B39 B40 B41 B42 B42b B43 B43b B43c B44 B45
B4b B10 B11 B11b B14 B15 B15b B15c B16 B16b B17 B17b B18 B19 B19b B19c B19d B19e B20 B20b B21 B21b B21c B22 B22b
B23 B23b B23c B23d B23e B24 B24b B26 B26b B26c B27 B27b B27c B27d B28 B28b B29 B29b B29c B29d B30 B30b B32 B33
B46 B46b B47 B47b B48 B48b B49 B49b B50 B51 B52 B53 B54"

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
                    r"[^\n]*rots in every copy", log, re.M)
have = [("FILES" if n else tree) for n, tree in rotten]
say(have == [r["tree"] for r in k["rotten"]], "it says rotten range(s) of %s; the census predicted %s"
    % (have or "none", [r["tree"] for r in k["rotten"]] or "none"))
files = [int(n) for n, tree in rotten if n]
say(files == [r["count"] for r in k["rotten"] if r["tree"] == "FILES"],
    "a rotten FILES range locks %s file(s) by its parent's count; the census predicted %s"
    % (files, [r["count"] for r in k["rotten"] if r["tree"] == "FILES"]))
said = count(r"^\[TagFS\] the \w+ node of level \d+ at blocks [0-9/]+: copy \d+ does not hold")
more = sum(int(n) for n in re.findall(r"^\[TagFS\] the \w+ node of level \d+ at blocks [0-9/]+: and (\d+) more "
                                      r"of its copies do not hold", log, re.M))
say(said + more == k["mended"], "it reads past %u copy(ies) that do not hold; the census predicted %u"
    % (said + more, k["mended"]))
say(said == k["mended_said"], "it names %u of them one by one, the rest by their count; the census predicted %u"
    % (said, k["mended_said"]))
m = re.search(r"^\[TagFS\] (\d+) block\(s\) are held: ", log, re.M)
held = int(m.group(1)) if m else 0
say(held == k["held"], "it holds %u block(s); the census predicted %u" % (held, k["held"]))
shape = count(r"^\[TagFS\] the book is written against its own shape — ")
say(shape == k["shape"], "it says %u shape(s) against the format; the census predicted %u" % (shape, k["shape"]))
torn = count(r"^\[TagFS\] Spine slot \d+ at block \d+ does not hold: ")
say(torn == k["torn_slots"], "it says %u torn slot(s); the census predicted %u" % (torn, k["torn_slots"]))
fell = count(r"^\[TagFS\] slot \d+ \(seq \d+\) names a Tally that does not read")
say(fell == k["fell_back"], "it falls back %u slot(s); the census predicted %u" % (fell, k["fell_back"]))
double = sorted([int(b), int(n)] for b, n in
                re.findall(r"\[TagFS FSCK\] data block (\d+) is named by [^\n]* at once, (\d+) block\(s\) from it", log))
say(double == sorted(k["double"]), "it names run(s) %s [start, blocks] twice owned; the census predicted %s"
    % (double or "none", sorted(k["double"]) or "none"))
m = re.search(r"^\[TagFS\] the next file id (\d+), tag id (\d+), bookmark (\d+)", log, re.M)
have = tuple(int(x) for x in m.groups()) if m else None
want = (k["next_fid"], k["next_tag"], k["next_bookmark"])
say(have == want, "it gives next file id, tag id and bookmark %s; the census predicted %s" % (have, want))
m = re.search(r"^\[TagFS\] (\d+) data blocks, (?:(\d+) free|free not counted), (\d+) files", log, re.M)
have = tuple(None if x is None else int(x) for x in m.groups()) if m else None
want = (k["data_blocks"], k["free"], k["files"])
say(have == want, "its block map derives data blocks, free and files %s; the census predicted %s" % (have, want))
m = re.search(r"^\[TagFS\] the next file id \d+, tag id \d+, bookmark \d+, seq (\d+)", log, re.M)
have = int(m.group(1)) if m else None
say(have == k["next_seq"], "it gives the next seq %s; the census predicted %u" % (have, k["next_seq"]))
CENSUS_BOOK_EOF
    local rc=$?
    [ $rc = 0 ] && census_book_judge_ribbon "$1" && [ -s "$SCRATCH/census.$1.verdict" ] && return 0
    echo "bad the judge could not read $SCRATCH/census.$1.json against $SCRATCH/serial.$1.log: $(tail -1 "$SCRATCH/census.$1.verdict")" \
        > "$SCRATCH/census.$1.verdict"
}

# census_book_judge_ribbon NAME — the second half of the judge (above): what
# the kernel says of the ribbon it walked and of the book as memory holds it —
# codes, stitches sewn on, MEDIUM_LIED, CLEAN, the price of the check on a
# medium that lied, the fallow ground, the print — appended to the verdict.
census_book_judge_ribbon() {
    python3 - "$SCRATCH/census.$1.json" "$SCRATCH/serial.$1.log" \
        >>"$SCRATCH/census.$1.verdict" 2>&1 <<'CENSUS_BOOK_EOF'
import json, re, sys
pred = json.load(open(sys.argv[1]))
log = open(sys.argv[2], errors="replace").read()
k = pred["kernel"]

def say(held, text):
    print(("ok " if held else "bad ") + text)

def count(pattern):
    return len(re.findall(pattern, log, re.M))

if not k["mounts"]:
    sys.exit(0)
ribbon = {}
for code in re.findall(r"^\[TagFS\] ribbon ([A-Z_]+): ", log, re.M):
    ribbon[code] = ribbon.get(code, 0) + 1
say(ribbon == k["ribbon"], "it says of the ribbon %s by code; the census predicted %s"
    % (sorted(ribbon.items()) or "nothing", sorted(k["ribbon"].items()) or "nothing"))
m = re.search(r"^\[TagFS\] the ribbon past the Tally: (\d+) stitch\(es\) sewn on, the last seq (\d+), "
              r"(\d+) applied", log, re.M)
have = tuple(int(x) for x in m.groups()) if m else None
want = (k["sewn"], k["last"], k["applied"])
say(have == want, "it sews on, ends on and applies %s stitch(es); the census predicted %s" % (have, want))
lied = re.search(r"^\[TagFS\] MEDIUM_LIED: ", log, re.M) is not None
say(lied == k["lied"], "it says MEDIUM_LIED: %s; the census predicted %s" % (lied, k["lied"]))
clean = re.search(r"^\[TagFS\] the last stitch of the ribbon says CLEAN", log, re.M) is not None
say(clean == k.get("clean", False), "it reads the last stitch CLEAN: %s; the census predicted %s"
    % (clean, k.get("clean", False)))
m = re.search(r"^\[TagFS\] the medium lied: (\d+) fresh block\(s\) of the newest (\d+) stitch\(es\) are read ", log, re.M)
have = (int(m.group(1)), int(m.group(2))) if m else None
want = tuple(k["lied_check"]) if k.get("lied_check") else None
say(have == want, "on a medium that lied it says the price of the check before a block is read — %s; the census "
    "predicted %s" % ("%u block(s) of %u stitch(es)" % have if have else "no line",
                      "%u block(s) of %u stitch(es)" % want if want else "no line"))
m = re.search(r"^\[TagFS\] (\d+) block\(s\) the ribbon let go lie fallow", log, re.M)
have = int(m.group(1)) if m else 0
say(have == k["fallow"], "it lets %u block(s) lie fallow; the census predicted %u" % (have, k["fallow"]))
m = re.search(r"^\[TagFS\] the book as memory holds it: its files print 0x([0-9A-Fa-f]{8}), its Use Context "
              r"(\d+) byte\(s\) print 0x([0-9A-Fa-f]{8})", log, re.M)
have = (int(m.group(1), 16), int(m.group(2)), int(m.group(3), 16)) if m else None
want = (k["files_print"], k["use_bytes"], k["use_print"])
say(have == want, "memory holds the files — names, sizes, tags, runs — and the Use Context the census replays: "
    "%s; the census predicted %s" % ("files 0x%08X, use %u byte(s) 0x%08X" % have if have else "no print line",
                                     "files 0x%08X, use %u byte(s) 0x%08X" % want))
CENSUS_BOOK_EOF
}

# census_book_slipped NAME — the landing booted again from its planted image,
# and every live file whose last block carries Slips (bookcensus.py KernelBook
# "slipped") read in the guest by `show`: its bytes, the Slips laid over its
# base, held byte for byte to volume-read's own read of the file (Content) —
# escaped as `show` escapes what does not print.
census_book_slipped() {
    local img="$SCRATCH/census.$1.img" json="$SCRATCH/census.$1.json" names n line
    names=$(python3 -c 'import json, sys
print(" ".join(f["name"] for f in json.load(open(sys.argv[1]))["kernel"].get("slipped", [])))' "$json" 2>/dev/null)
    [ -n "$names" ] || return 0
    make run-stop >/dev/null 2>&1
    if ! make run-bg BOOTDISK="$img" >"$SCRATCH/census.$1.slipped.run-bg" 2>&1; then
        harness "census: run-bg would not start $1 again to read its Slips"
        return
    fi
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S" && wait_for_prompt
    for n in $names; do
        type_line "show $n" && wait_for_prompt
    done
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.$1.slipped.log"
    python3 - "$json" "$SCRATCH/serial.$1.slipped.log" >"$SCRATCH/census.$1.slipped.verdict" 2>&1 <<'SLIPPED_EOF'
import json, sys
k = json.load(open(sys.argv[1]))["kernel"]
log = open(sys.argv[2], errors="replace").read()
for f in k.get("slipped", []):
    want = "".join(c if 32 <= ord(c) < 127 or c in "\n\r\t" else "\\x%02x" % ord(c) for c in f["content"][:4096])
    at = log.find("~ show %s" % f["name"])
    got = log[at:].split("\n", 1)[1] if at >= 0 and "\n" in log[at:] else ""
    held = got.startswith(want)
    print("%s %s reads in the guest as volume-read reads it, its Slips over its last block (%u bytes)%s"
          % ("ok" if held else "bad", f["name"], len(f["content"]), "" if held else ": got %r" % got[:80]))
SLIPPED_EOF
    while IFS= read -r line; do
        case "$line" in
            "ok "*)  ok  "census $1: ${line#ok }" ;;
            *)       bad "census $1: ${line#bad }" ;;
        esac
    done < "$SCRATCH/census.$1.slipped.verdict"
}

# census_book — every landing of CENSUS_BOOK_LANDINGS planted on a fresh image,
# predicted, booted and judged.
census_book() {
    local plant line
    for plant in $CENSUS_BOOK_LANDINGS; do
        census_boot "book_$plant" "$plant" || continue
        census_book_judge "book_$plant"
        census_book_slipped "book_$plant"
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
# book the kernel writes stitches of names and memory and no change to a
# file, so every test that writes through TagFS written for any format stands
# aside, counted (K9): the stand_tally, stand_ribbon and stand_stitch suites
# are what runs, and the suites written for any format that only read.
#
# Judged: the STAND_RUN tests this tree registers walked, nothing failed,
# nothing held; every book test ran — none stood down but the leaf on an
# image whose FILES is one leaf; the port's road of the rotten block taken
# on the AHCI port and said to stand aside on the ATA disk, never skipped in
# silence; the lines that say what the volume promises and that no Tally is
# written, and the one refusal of a change to a file; a block with no digest
# named; the four doors below the glass knocked at and refused; the book's
# medium coming back read again; no panic, no mirror defect and no defect of
# TagFS or Settle said — a door below the glass reached by a road nobody
# shut, code of format v1 asking for a LAYOUT; BLOCKFAIL refusing as many
# commands as the tests that arm it refuse on each boot — the refusals of the
# tests whose book then takes no write as many as their lines say; the halt
# laying its stitch and syncing the volume; and the block map derived from the
# owners: a heap refusing it a page refuses the mount once, the mixed windows
# named, a claim on taken blocks and a release of a free one named as they
# are inserted — past the bound, counted, said once when the counting begins
# and the count when the map is let go — and the boot's mount saying the data
# blocks, free and files volume-read's census predicts of the image booted,
# every mount after it the same free — but those a test of the ribbon makes
# while its own stitches lie on the medium.
#
#   test                                        image   padded   uefi
#   stand_blockfail_flush_refused                  1        1       1   the flush it asks for
#   stand_flush_ticket                             1        1       1   the flush its ticket waits on
#   stand_medium_bell_rings_once                   1        1       1   the read of the volume's first block
#   stand_blockfail_two_ranges                     2        2       2   a read in each range
#   stand_roadstead_refusal_rings_once             0        0       1   an AHCI port's alone
#   stand_tally_rotten_leaf_locked                 0       18       0   copy 1 then copy 2: the run, then each sector
#   stand_stitch_retry_atom                        1        1       1   the stitch's first atom
#   stand_stitch_retry_flush                       1        1       1   the flush after the stitch
#   stand_blockfail_refused_writes                 4        4       4   two writes into each range
#   total                                         11       29      12
#   stand_stitch_refused_everywhere                N        N       N   every attempt, as many as its line says
#   stand_stitch_retry_quire_entered               N        N       N   the first stitch of a Quire: its line, 1
#   stand_stitch_retry_refused_twice               N        N       N   a stitch and its retry: its line, 2
# ---------------------------------------------------------------------------

STAND_BOOK_BLOCKFAILS=11
STAND_BOOK_PADDED_BLOCKFAILS=29
STAND_BOOK_UEFI_BLOCKFAILS=12
STAND_BOOK_HALT='^\[HALT\] Storage sync complete'
STAND_BOOK_REFUSED='^\[TagFS\] the book takes no write: ([0-9]+) refusal\(s\) in a row'
STAND_BOOK_PROMISE='^\[TagFS\] what this volume promises: its book takes the names of tags and the memory of its Use Context'
STAND_BOOK_NO_TALLY='^\[TagFS\] no Tally is written by this kernel: the ribbon is read whole at every mount'
STAND_BOOK_FILES_REFUSED='^\[TagFS\] this kernel writes the names of tags and the volume.s memory into a format-v2 book, and not yet a change to a file'
STAND_BOOK_TESTS="stand_tally_files_read_whole stand_tally_mount_writes_nothing stand_tally_copy_one_rotten
stand_tally_rotten_leaf_locked stand_tally_writes_refused stand_tally_rot_three_paths
stand_tally_undigested_refused stand_tally_length_asked stand_tally_lower_doors_refused
stand_tally_medium_returns
stand_tally_live_without_rows stand_tally_map_holds stand_tally_map_claim_named
stand_tally_map_short_of_memory stand_blockfail_two_ranges
stand_ribbon_slip_three_roads stand_ribbon_torn_tail stand_ribbon_next_quire stand_ribbon_closed_damaged
stand_ribbon_dead_branch stand_ribbon_undone
stand_stitch_answers_after_flush stand_stitch_use_context stand_stitch_coined stand_stitch_halt_clean
stand_stitch_retry_atom stand_stitch_retry_flush stand_stitch_refused_everywhere stand_stitch_ribbon_runs_out
stand_stitch_tally_cost stand_stitch_medium_lied stand_stitch_replug_loses stand_blockfail_refused_writes
stand_stitch_retry_quire_entered stand_stitch_retry_refused_twice stand_stitch_runs_on_leaves
stand_stitch_tally_cost_long stand_stitch_refused_names_nothing stand_registry_counts_from_nothing"
STAND_BOOK_PORT_ASIDE='^\[TESTS\] stand_(tally_rot_three_paths|ribbon_slip_three_roads): the port.s road stands aside — '
STAND_BOOK_RIBBON_LAID='[TESTS] stand_ribbon: stitches laid past the Tally'
STAND_BOOK_RIBBON_BACK='[TESTS] stand_ribbon: what the test laid is put back'
STAND_BOOK_STITCH_LAID='[TESTS] stand_stitch: the ribbon is sewn past where it stood'
STAND_BOOK_STITCH_BACK='[TESTS] stand_stitch: the ribbon is put back as it was found'
STAND_BOOK_DEFECT='\[(TagFS|Settle)\] DEFECT'
STAND_BOOK_MAP_SHORT='^\[TagFS\] this volume is NOT mounted: its block map is short of memory: [1-9][0-9]* mixed window'
STAND_BOOK_MAP_MET='^\[TagFS FSCK\] the run of 3 block\(s\) from data block [0-9]+ that the tally claims meets 2 taken already'
STAND_BOOK_MAP_FREED='^\[TagFS FSCK\] the run of 1 block\(s\) from data block [0-9]+ that file [0-9]+ lets go meets 1 free already'
STAND_BOOK_MAP_PAST='^\[TagFS FSCK\] more claims and releases on the block map meet blocks so already than are named one by one \(32\)'
STAND_BOOK_MAP_COUNT='^\[TagFS FSCK\] and 10 more claim\(s\) and release\(s\) on the block map met blocks so already'

# stand_book_judge TAG WHAT BLOCKFAILS ASIDE PORT IMG — one boot of a book:
# ASIDE the one stand_tally test that may stand down there, or empty; PORT
# "taken" where the seat's reads go through the port, "aside" where they
# cannot; IMG the image booted, whose census volume-read predicts.
stand_book_judge() {
    local L="$SCRATCH/serial.$1.log" what=$2 want=$3 aside=$4 port=$5 img=$6 said got t
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
    got=$(grep -acE "$STAND_BOOK_PORT_ASIDE" "$L")
    if [ "$port" = taken ]; then
        [ "$got" = 0 ]
        chk $? "stand: $what — the rotten block is refused, and the Slips are read, on the port's road too"
    else
        [ "$got" = 2 ]
        chk $? "stand: $what — the port's road of the rotten block and of the Slips stands aside, said ($got: $(grep -aoE 'the port.s road stands aside — [^.]*' "$L" | head -1))"
    fi
    grep -aqE "$STAND_BOOK_PROMISE" "$L" && grep -aqE "$STAND_BOOK_NO_TALLY" "$L"
    chk $? "stand: $what — the mount says what the book takes and that no Tally is written"
    got=$(grep -acE "$STAND_BOOK_FILES_REFUSED" "$L")
    [ "$got" -ge 1 ]
    chk $? "stand: $what — a change to a file is refused in so many words ($got)"
    ! grep -aqF "[TagFS] the volume may only be read: this kernel reads format-v2 books" "$L"
    chk $? "stand: $what — no book stands under glass for being a book"
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
    t=$(grep -aoE "$STAND_BOOK_REFUSED" "$L" | sed -E "s/$STAND_BOOK_REFUSED/\1/" | awk '{ n += $1 } END { print n + 0 }')
    [ "$got" = "$((want + t))" ] && [ "$t" -gt 0 ]
    chk $? "stand: $what — BLOCKFAIL refused $got command(s), as many as the tests that arm it refuse here ($want, and $t of the tests whose book then takes no write, as their lines say)"
    grep -aqE "$STAND_BOOK_HALT" "$L"
    chk $? "stand: $what — the halt through bye lays its stitch and syncs the volume"
    got=$(grep -acE "$STAND_BOOK_MAP_SHORT" "$L")
    [ "$got" = 1 ]
    chk $? "stand: $what — a heap refusing the block map its page refuses the mount once, naming the mixed windows ($got: $(grep -aoE 'its block map is short of memory: [^—]*' "$L" | head -1))"
    grep -aqE "$STAND_BOOK_MAP_MET" "$L" && grep -aqE "$STAND_BOOK_MAP_FREED" "$L"
    chk $? "stand: $what — a claim on taken blocks and a release of a free one are named on the map's insertion"
    got=$(grep -acE "$STAND_BOOK_MAP_PAST" "$L")
    [ "$got" = 1 ] && grep -aqE "$STAND_BOOK_MAP_COUNT" "$L"
    chk $? "stand: $what — past the 32 named, the rest are said to be counted once, when they begin ($got), and their count (the stand's 2 + 40 less 32) when the map is let go"
    want=$(python3 tools/volume-read.py "$img" check --json 2>/dev/null | python3 -c 'import json, sys
k = json.load(sys.stdin)["kernel"]
print("%s %s %s" % (k["data_blocks"], k["free"], k["files"]))' 2>/dev/null)
    got=$(awk -v laid="$STAND_BOOK_RIBBON_LAID" -v back="$STAND_BOOK_RIBBON_BACK" \
              -v sewing="$STAND_BOOK_STITCH_LAID" -v sewn_back="$STAND_BOOK_STITCH_BACK" \
              'index($0, laid) == 1 || index($0, sewing) == 1 { sewn = 1 }
               index($0, back) == 1 || index($0, sewn_back) == 1 { sewn = 0 } !sewn' "$L" |
          grep -aE '^\[TagFS\] [0-9]+ data blocks, [0-9]+ free, [0-9]+ files' |
          sed -E 's/^\[TagFS\] ([0-9]+) data blocks, ([0-9]+) free, ([0-9]+) files.*/\1 \2 \3/')
    [ -n "$want" ] && [ "$(echo "$got" | head -1)" = "$want" ] &&
        [ "$(echo "$got" | cut -d' ' -f1-2 | sort -u)" = "${want% *}" ]
    chk $? "stand: $what — the boot's mount derives the data blocks, free and files volume-read's census predicts of the image, and every mount after it the same free — a leaf a test rots locks files, and holds their blocks ($(echo "$got" | sort | uniq -c | tr -s ' \n' ' '); predicted ${want:-nothing})"
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
        stand_tally_rotten_leaf_locked aside build/boxos.img
    if python3 tools/volume-read.py build/boxos.img self-test --emit B4p --out "$img" \
            >"$SCRATCH/stand.book_padded.emit" 2>&1; then
        stand_boot stand_book_padded BOOTDISK="$img" CORES=4 MEM=4G
        [ $? = 2 ] || stand_book_judge stand_book_padded "the padded book, BIOS 4c" \
            "$STAND_BOOK_PADDED_BLOCKFAILS" "" aside "$img"
    else
        bad "stand: the padded book could not be planted — $(tail -1 "$SCRATCH/stand.book_padded.emit")"
    fi
    rm -f "$img" build/boxos.img
    if build_with_key VOLUME_TESTS=on STRICT=on; then
        stand_boot stand_book_uefi UEFI=on STRICT=on CORES=4 MEM=4G
        [ $? = 2 ] || stand_book_judge stand_book_uefi "the book, UEFI 4c on an AHCI port, STRICT" \
            "$STAND_BOOK_UEFI_BLOCKFAILS" stand_tally_rotten_leaf_locked taken build/boxos.img
    else
        echo "BUILD FAILED (VOLUME_TESTS=on STRICT=on VOLUME_FORMAT=v2) — tail:"; tail -25 "$SCRATCH/build.log"
        bad "stand: the book — VOLUME_TESTS=on STRICT=on VOLUME_FORMAT=v2 does not build"
    fi
    STAND_FORMAT=$keep
    lockrank_judge "$SCRATCH/serial.stand_book.log $SCRATCH/serial.stand_book_padded.log $SCRATCH/serial.stand_book_uefi.log" \
        "stand, the book"
}

# ---------------------------------------------------------------------------
# book_circle: the book the kernel writes, read back by both readers
#
# What the kernel lays on a book is read again by the kernel's own reader and
# by volume-read's, written apart from it (settle/sewing.c against
# stitch.py / bookmount.py). A plain image of format v2 is booted; in the guest
# the person says a Use Context (`use circle sewn`) — a stitch with its
# UseContext, the tags the boot gave ids in Coineds — and stops the machine
# (`bye`), whose halt lays its last stitch CLEAN. The image the machine left is
# then held: volume-read finds it consistent; its census says what the next
# mount must — the stitches sewn on and the last, the Use Context as said,
# CLEAN on the last — and the next boot of the same image says exactly that in
# its own lines (census_book_judge), the print of what memory holds included.
# ---------------------------------------------------------------------------

BOOK_CIRCLE_USE="circle,sewn"

book_circle() {
    local name=book_circle img="$SCRATCH/circle.img" json="$SCRATCH/census.book_circle.json" line rc
    echo "== book circle: what the kernel writes on a book, read back by both readers =="
    rm -f build/boxos.img "$img" "$SCRATCH/serial.book_circle_write.log" "$SCRATCH/serial.$name.log"
    if ! build_with_key VOLUME_FORMAT=v2; then
        bad "circle: make VOLUME_FORMAT=v2 does not build: $(grep -aE 'ERROR|error' "$SCRATCH/build.log" | tail -1)"
        return
    fi
    cp build/boxos.img "$img"
    make run-stop >/dev/null 2>&1
    if ! make run-bg BOOTDISK="$img" >"$SCRATCH/circle.run-bg" 2>&1; then
        harness "circle: run-bg would not start the book's image: $(tail -1 "$SCRATCH/circle.run-bg")"
        return
    fi
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S" && wait_for_prompt &&
        type_line "use circle sewn" && wait_for_prompt && type_line "bye" &&
        wait_for_line -E build/serial.log "$STAND_HALT_ENDS" "$WAIT_QUIET_S"
    rc=$?
    sleep 1
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.book_circle_write.log"
    [ $rc = 0 ] && grep -aq '^\[HALT\] Storage sync complete' "$SCRATCH/serial.book_circle_write.log"
    chk $? "circle: the guest says a Use Context and stops, the halt syncing the volume ($(grep -aE '^\[HALT\] (Storage sync complete|.*NOT MADE DURABLE|.*DID NOT WRITE)' "$SCRATCH/serial.book_circle_write.log" | head -1 | cut -c1-100))"
    python3 tools/volume-read.py "$img" check --json >"$json" 2>"$SCRATCH/circle.check.err"
    chk $? "circle: volume-read finds the book the kernel wrote consistent ($(python3 -c 'import json, sys
print(json.load(open(sys.argv[1])).get("verdict", "no verdict"))' "$json" 2>/dev/null))"
    python3 - "$json" "$BOOK_CIRCLE_USE" >"$SCRATCH/circle.verdict" 2>&1 <<'CIRCLE_EOF'
import json, sys
sys.path.insert(0, "tools")
from volumeread import bookread
k = json.load(open(sys.argv[1]))["kernel"]
said = sys.argv[2].encode()
def say(held, text):
    print(("ok " if held else "bad ") + text)
say(k["mounts"] and k["glass"] == 0, "the book mounts, with no reason to be only read — %s" % (k.get("refusal") or "%u reason(s)" % k["glass"]))
say(k["use_bytes"] == len(said) and k["use_print"] == bookread.Crc32c(said),
    "its census replays the Use Context the guest said, %u byte(s) print 0x%08X — wants %u, 0x%08X"
    % (k["use_bytes"], k["use_print"], len(said), bookread.Crc32c(said)))
say(k["sewn"] >= 2 and k["applied"] == k["sewn"] and not k["ribbon"] and not k["lied"],
    "the ribbon carries the kernel's stitches whole: %u sewn on, %u applied, %s said, lied %s"
    % (k["sewn"], k["applied"], sorted(k["ribbon"].items()) or "nothing", k["lied"]))
CIRCLE_EOF
    while IFS= read -r line; do
        case "$line" in
            "ok "*)  ok  "circle: ${line#ok }" ;;
            *)       bad "circle: ${line#bad }" ;;
        esac
    done < "$SCRATCH/circle.verdict"
    if ! make run-bg BOOTDISK="$img" >"$SCRATCH/circle.again.run-bg" 2>&1; then
        harness "circle: run-bg would not start the written book again: $(tail -1 "$SCRATCH/circle.again.run-bg")"
        return
    fi
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S"
    rc=$?
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.$name.log"
    [ $rc = 0 ] || bad "circle: the written book never reached a shell"
    census_book_judge "$name"
    while IFS= read -r line; do
        case "$line" in
            "ok "*)  ok  "circle, the next mount: ${line#ok }" ;;
            "bad "*) bad "circle, the next mount: ${line#bad }" ;;
        esac
    done < "$SCRATCH/census.$name.verdict"
    grep -aq '^\[TagFS\] the last stitch of the ribbon says CLEAN' "$SCRATCH/serial.$name.log"
    chk $? "circle, the next mount: it reads the halt's last stitch CLEAN"
    rm -f "$img"
}
