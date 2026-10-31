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
# and branches (selftest_book_ribbon.py), what a torn or rotten stitch costs,
# and the shapes no stitch may take; and what the medium may say that no writer
# does, each held to a bound before it is trusted
# (selftest_book_ribbon_damage.py).
CENSUS_BOOK_LANDINGS="B0 B1 B2 B3 B4 B4p B5 B6 B6b B7 B7b B7c B8 B9 B9b B12 B13 B25 B34 B35 B36 B37 B38 B38b
B38c B39 B40 B41 B42 B42b B43 B43b B43c B44 B45
B4b B6c B10 B11 B11b B11c B14 B15 B15b B15c B16 B16b B17 B17b B18 B19 B19b B19c B19d B19e B20 B20b B21 B21b B21c B22 B22b
B23 B23b B23c B23d B23e B24 B24b B26 B26b B26c B27 B27b B27c B27d B28 B28b B29 B29b B29c B29d B29e B29f B29g B29h B29i B29j B30 B30b B32
B33
B55a B55b B55c B55d B55e
B59 B59b B60 B60c B4g B60e B59d B59e B59f B59g B59h B65 B59i B59j B59k B59l
B62 B63 B64
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
lost = [[int(x) for x in m.groups()] for m in re.finditer(
    r"^\[TagFS\] ribbon DEAD_BRANCH: .*what settled is lost: at most (\d+) stitch\(es\).*no seq up to (\d+), "
    r"the largest found", log, re.M)]
say(lost == k.get("lost", []), "it counts what settled is lost and the largest seq found as %s; the census "
    "predicted %s" % (lost or "nothing", k.get("lost") or "nothing"))
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
# book the kernel writes stitches — files made, named, tagged and given up,
# tags, the Use Context — and no byte of a file's data, so every test that
# writes through TagFS written for format v1 stands aside, counted (K9): the
# stand_tally, stand_ribbon, stand_stitch, stand_stitch_edge and stand_docket
# suites are what runs, and the suites written for any format that only read.
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
#   test                                        image   padded   uefi   usb
#   stand_blockfail_flush_refused                  1        1       1     1   the flush it asks for
#   stand_flush_ticket                             1        1       1     1   the flush its ticket waits on
#   stand_medium_bell_rings_once                   1        1       1     1   the read of the volume's first block
#   stand_blockfail_two_ranges                     2        2       2     2   a read in each range
#   stand_roadstead_refusal_rings_once             0        0       1     0   an AHCI port's alone
#   stand_tally_rotten_leaf_locked                 0       18       0     0   copy 1 then copy 2: the run, then each sector
#   stand_stitch_retry_atom                        1        1       1     1   the stitch's first atom
#   stand_stitch_retry_flush                       1        1       1     1   the flush after the stitch
#   stand_blockfail_refused_writes                 4        4       4     4   two writes into each range
#   stand_stitch_retry_quire_entered               1        1       1     1   the first atom of the Quire entered
#   stand_stitch_retry_refused_twice               2        2       2     2   a stitch and its retry at the next's start
#   stand_stitch_retry_head_landed                 2        2       2     2   a stitch past its head, and its retry at the next's start
#   stand_docket_closed_damaged                    4        4       4     4   the stitch's unit, then its sector, read twice
#   stand_fresh_refused_names_nothing              1        1       1     1   the first command of a file's write
#   stand_fresh_flushes_first                      1        1       1     1   the flush of the data before the stitch
#   stand_binder_node_refused                      1        1       1     1   the Tally's first node
#   stand_binder_slot_flush_refused                1        1       1     1   the flush of the Tally's slot
#   the rows above                                24       42      25    24
#   every other test the book runs                71       71      71    71   the neutral tests, and the book's since the rows
#   total                                         95      113      96    95
#   stand_stitch_refused_everywhere                N        N       N     N   every attempt, as many as its line says
#   stand_stitch_retry_runs_out                    N        N       N     N   a stitch and every atom of the next: its line
#   stand_slip_refused_tail, stand_escheat_tail,   N        N       N     N   the reads of a Slip's tail block the Helm's
#   stand_escheat_tail_helm_held                                              service races the test for: its line
# ---------------------------------------------------------------------------

# Every test of the tree runs on a book, the neutral ones among them (spec_v2
# §16.5 п.22): the 71 refusals of the tests the rows do not name were counted
# on the BIOS 4c image, boot after boot the same; the padded, UEFI and USB
# columns are derived from the rows, not yet measured — the first boot of each
# leg pins them, a neutral test that refuses otherwise on its road with them.
STAND_BOOK_BLOCKFAILS=95
STAND_BOOK_PADDED_BLOCKFAILS=113
STAND_BOOK_UEFI_BLOCKFAILS=96
# The book on a USB stick (stand_book_usb): the usb column — its seat's reads
# go through the port as on UEFI, so stand_tally_rotten_leaf_locked stands
# down there and refuses nothing, and the stick is no AHCI port for
# stand_roadstead_refusal_rings_once to refuse on.
STAND_BOOK_USB_BLOCKFAILS=95
STAND_BOOK_HALT='^\[HALT\] Storage sync complete'
# C13 (stand_docket_closed_damaged): sector 1 of a stitch the kernel sewed will
# not read at the next mount — the line names it and the record bytes it took.
STAND_BOOK_C13='^\[TagFS\] ribbon STITCH_TORN: stitch [0-9]+ is closed and damaged: sector\(s\) 1 of its [0-9]+ will not read, and [1-9][0-9]* byte\(s\) of its records with them'
STAND_BOOK_REFUSED='^\[TagFS\] the book takes no write: ([0-9]+) refusal\(s\) in a row'
STAND_BOOK_RACED='^\[TESTS\] [a-z_]+: BLOCKFAIL answered ([0-9]+) command\(s\) of the plan let go'
STAND_BOOK_PROMISE='^\[TagFS\] what this volume promises: its book takes files made, written, named, tagged and given up, the names of tags and the memory of its Use Context'
STAND_BOOK_TALLY='^\[TagFS\] a whole Tally of the book is written once the ribbon past the newest slot holds as many useful bytes as one costs'
STAND_BOOK_TESTS="stand_tally_files_read_whole stand_tally_mount_writes_nothing stand_tally_copy_one_rotten
stand_tally_rotten_leaf_locked stand_tally_writes_refused stand_tally_rot_three_paths
stand_tally_undigested_refused stand_tally_length_asked stand_tally_lower_doors_refused
stand_tally_medium_returns
stand_tally_live_without_rows stand_tally_map_holds stand_tally_map_claim_named
stand_tally_map_short_of_memory stand_blockfail_two_ranges
stand_ribbon_slip_three_roads stand_ribbon_torn_tail stand_ribbon_next_quire stand_ribbon_closed_damaged
stand_ribbon_dead_branch stand_ribbon_undone stand_ribbon_boot_slip
stand_stitch_answers_after_flush stand_stitch_use_context stand_stitch_coined stand_stitch_halt_clean
stand_stitch_retry_atom stand_stitch_retry_flush stand_stitch_refused_everywhere stand_stitch_ribbon_runs_out
stand_stitch_tally_cost stand_stitch_medium_lied stand_stitch_replug_kept stand_blockfail_refused_writes
stand_stitch_retry_quire_entered stand_stitch_retry_refused_twice stand_stitch_retry_head_landed
stand_stitch_retry_runs_out
stand_stitch_runs_on_leaves
stand_stitch_tally_cost_long stand_registry_counts_from_nothing
stand_docket_each_change_mounts_again stand_docket_merges stand_docket_data_refused stand_docket_ground_rests
stand_docket_mount_ground_rests stand_docket_claim_refused stand_docket_closed_damaged stand_docket_replug_kept
stand_docket_warrant
stand_binder_whole stand_binder_node_refused stand_binder_unslotted stand_binder_slot_torn
stand_binder_slot_flush_refused stand_binder_slot_unsure stand_binder_parcels stand_binder_heals stand_binder_rotten_named
stand_binder_slack stand_binder_ground_back stand_binder_full_back stand_binder_standing_end
stand_tip_in_parcel stand_tip_in_heals_files stand_tip_in_heals_digests stand_tip_in_emptied
stand_tip_in_quires_held stand_tip_in_rotten_past stand_tip_in_parcels_as_they_lie stand_parity_stitch_holds_no_lock
stand_parity_files_in_turn
stand_book_glean_quires stand_book_glean_copies stand_escheat_warrant stand_escheat_tail stand_escheat_tail_helm_held
stand_endpaper_owed stand_endpaper_labeled_carries stand_endpaper_mount stand_endpaper_pending
stand_endpaper_kept stand_endpaper_empty_files
stand_book_room_next_quire_clear stand_book_room_gather_one_run stand_book_room_stakes_counted
stand_book_room_own_whole stand_book_room_tally_gives_room
stand_away_other_stitches stand_away_not_landed stand_away_own_attempt stand_away_born_not_landed
stand_away_newer_slot stand_away_lied_whole stand_away_verdict"
STAND_BOOK_PORT_ASIDE='^\[TESTS\] stand_(tally_rot_three_paths|ribbon_slip_three_roads): the port.s road stands aside — '
STAND_BOOK_RIBBON_LAID='[TESTS] stand_ribbon: stitches laid past the Tally'
STAND_BOOK_RIBBON_BACK='[TESTS] stand_ribbon: what the test laid is put back'
STAND_BOOK_STITCH_LAID='[TESTS] stand_stitch: the ribbon is sewn past where it stood'
STAND_BOOK_STITCH_BACK='[TESTS] stand_stitch: the ribbon is put back as it was found'
STAND_BOOK_DEFECT='\[(TagFS|Settle)\] DEFECT'
# The stand's levers on the Tally's self-check (stand_dog_ear_tests.c,
# StandDogEarLevered): each changes the state behind every Ink between its two
# lines, and on a kernel not STRICT the Tally it asks for is refused by the
# self-check's DEFECT (settle/binder_check.c, CheckDefect) — one line, of the
# self-check's own words, between the two; a STRICT kernel stands them down. A
# lever that moves a file's ground is met by whichever check walks it first:
# an element of the Tally before, or the DIGESTS ground laid in
# (settle/binder_digests.c) — either says DogEar was given the lie.
STAND_BOOK_LEVER='[TESTS] stand_dog_ear: a lever pulled behind every Ink'
STAND_BOOK_LEVER_GO='[TESTS] stand_dog_ear: the lever let go'
STAND_BOOK_LEVER_DEFECT='^\[TagFS\] DEFECT: (the (FILES|DIGESTS) element \([0-9]+, [0-9]+\) (differs from the Tally before.s|is not in the Tally before|is in the Tally before and not in this one), and DogEar does not hold its key|DIGESTS laid in .* the ground the files took and let go since is not what DogEar says)'
STAND_BOOK_MAP_SHORT='^\[TagFS\] this volume is NOT mounted: its block map is short of memory: [1-9][0-9]* mixed window'
STAND_BOOK_MAP_MET='^\[TagFS FSCK\] the run of 3 block\(s\) from data block [0-9]+ that the tally claims meets 2 taken already'
STAND_BOOK_MAP_FREED='^\[TagFS FSCK\] the run of 1 block\(s\) from data block [0-9]+ that file [0-9]+ lets go meets 1 free already'
STAND_BOOK_MAP_PAST='^\[TagFS FSCK\] more claims and releases on the block map meet blocks so already than are named one by one \(32\)'
STAND_BOOK_MAP_COUNT='^\[TagFS FSCK\] and 10 more claim\(s\) and release\(s\) on the block map met blocks so already'

# stand_book_defects LOG — every defect of TagFS a boot of a book said that no
# lever of the stand drew: each line of STAND_BOOK_DEFECT outside the levers'
# lines, and inside a lever's any but the one self-check DEFECT it draws — a
# lever that drew none, or more than one, named. Empty for a boot with none.
stand_book_defects() {
    DEFECT="$STAND_BOOK_DEFECT" DRAWN="$STAND_BOOK_LEVER_DEFECT" awk -v on="$STAND_BOOK_LEVER" \
        -v off="$STAND_BOOK_LEVER_GO" '
        index($0, on) == 1  { pulled = 1; drawn = 0; what = $0; next }
        index($0, off) == 1 { if (pulled && drawn != 1) print "a lever drew " drawn " DEFECT(s) of the self-check, not one: " what
                              pulled = 0; next }
        $0 ~ ENVIRON["DEFECT"] { if (pulled && $0 ~ ENVIRON["DRAWN"] && drawn++ == 0) next; print }' "$1"
}

# stand_book_judge TAG WHAT BLOCKFAILS ASIDE PORT IMG — one boot of a book:
# ASIDE the one stand_tally test that may stand down there, or empty; PORT
# "taken" where the seat's reads go through the port, "aside" where they
# cannot; IMG the image booted, whose census volume-read predicts.
stand_book_judge() {
    local L="$SCRATCH/serial.$1.log" what=$2 want=$3 aside=$4 port=$5 img=$6 said got t r
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
    grep -aqE "$STAND_BOOK_PROMISE" "$L" && grep -aqE "$STAND_BOOK_TALLY" "$L"
    chk $? "stand: $what — the mount says what the book takes and when a Tally is written"
    ! grep -aqF "[TagFS] the volume may only be read: this kernel reads format-v2 books" "$L"
    chk $? "stand: $what — no book stands under glass for being a book"
    grep -aqE '^\[Integrity\] block [0-9]+ carries no digest in its volume.s Tally' "$L"
    chk $? "stand: $what — a block with no digest is named, its read refused"
    got=$(grep -acE '^\[TESTS\] .*, knocked at by the stand under glass, refused before a byte' "$L")
    [ "$got" = 4 ]
    chk $? "stand: $what — the four doors below the glass refuse the stand's knock ($got)"
    grep -aqE '^\[TagFS\] the volume is back, in seat [0-9]+: .* — its book as this kernel left it: ' "$L"
    chk $? "stand: $what — the book's medium came back as it was left and its book is taken up, said"
    grep -aqE '^\[TagFS\] the volume came back in seat [0-9]+ as this kernel left its book, but [1-9][0-9]* of the [0-9]+ block\(s\) of data no stitch settled do not read back as written' "$L"
    chk $? "stand: $what — a block of data no stitch settled that did not land gives the book up, said with how many"
    grep -aqE '^\[TagFS\] the volume came back in seat [0-9]+ not as this kernel left its book: ' "$L"
    chk $? "stand: $what — a medium another machine changed gives the book up, said with why"
    ! grep -aqF 'this kernel reads such a volume again from its book' "$L"
    chk $? "stand: $what — no book's medium is read again for being a book's"
    ! grep -aq 'KERNEL PANIC' "$L"
    chk $? "stand: $what — no panic"
    ! grep -aqE "$STAND_MIRROR_DEFECT" "$L"
    chk $? "stand: $what — every live file of the map stood as its record, with no row to disagree with"
    got=$(stand_book_defects "$L")
    [ -z "$got" ]
    chk $? "stand: $what — no defect of TagFS said but the one each lever of the stand draws from the Tally's self-check: no door below the glass reached by a road nobody shut, no code of format v1 asked for a LAYOUT, no cycle asked, no element of a Tally changed outside DogEar ($(echo "$got" | head -1 | cut -c1-120))"
    got=$(grep -ac '^\[BLOCKFAIL\] seat [0-9]*: ' "$L")
    t=$(grep -aoE "$STAND_BOOK_REFUSED" "$L" | sed -E "s/$STAND_BOOK_REFUSED/\1/" | awk '{ n += $1 } END { print n + 0 }')
    r=$(grep -aoE "$STAND_BOOK_RACED" "$L" | sed -E "s/$STAND_BOOK_RACED/\1/" | awk '{ n += $1 } END { print n + 0 }')
    [ "$got" = "$((want + t + r))" ] && [ "$t" -gt 0 ] && [ "$r" -gt 0 ]
    chk $? "stand: $what — BLOCKFAIL refused $got command(s), as many as the tests that arm it refuse here ($want, $t of the tests whose book then takes no write, and $r a Slip's tail block was read for by the Helm's service and the test, as their lines say)"
    grep -aqE "$STAND_BOOK_C13" "$L"
    chk $? "stand: $what — the stitch the kernel sewed, its sector 1 unreadable, is said closed and damaged with the bytes of its records lost (C13: $(grep -aoE "$STAND_BOOK_C13" "$L" | head -1 | cut -c1-120))"
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
    stand_book_usb
    STAND_FORMAT=$keep
    lockrank_judge "$SCRATCH/serial.stand_book.log $SCRATCH/serial.stand_book_padded.log $SCRATCH/serial.stand_book_uefi.log $SCRATCH/serial.stand_book_usb.log" \
        "stand, the book"
}

# ---------------------------------------------------------------------------
# book_big: a file larger than the room a machine is given, in one write
#
# The image this tree makes gives a running machine 64 MiB to work in
# (create_tagfs.h GROUND_WORKING_ROOM_BLOCKS), so a file past that needs ground
# the build does not lay: the image is copied, its volume's partition grown by
# BOOK_BIG_GROW_MIB at its end — the volume is last on the medium (Makefile) —
# and a book made on it again by create_tagfs --v2 with the shell, quilltest,
# bye and the kernel. In the guest `quilltest big big BOOK_BIG_MIB` writes the file
# in ONE write — the windows the Helm cuts it into each inked, the Docket
# merging them into the one Born a stitch lays, its digests alone 8 bytes a
# block — anchors it, reads every byte back and says the FNV-1a print, and the
# machine stops. volume-read finds the image consistent and reads the file
# back byte for byte; the Born's digests alone are more useful bytes than the
# Tally the maker laid costs, so the kernel binds the state in a Tally of its
# own (TALLY_SLACK), and `bound` binds it again where it lies byte for byte
# and holds its state to the slot before it and its ribbon (logcheck_bind.sh
# book_bind); the next boot mounts what the census predicts
# (census_book_judge, kernel = volume-read), and `quilltest big big` reads the
# file once more, every block held to the digest the ribbon carries. The
# guest says nothing while the one write and the read back run, so both are
# waited for by the quiet a long medium transfer is allowed (WAIT_QUIET_SLOW_S).
# ---------------------------------------------------------------------------

BOOK_BIG_MIB=72
BOOK_BIG_GROW_MIB=128
BOOK_BIG_SAID='\[QUILL\] big: PASS — [^ ]+ \(fid [0-9]+\)[^:]*: [0-9]+ bytes, print 0x[0-9A-F]+'

# book_big_said LOG — "SIZE PRINT" of LOG's PASS line, empty for none.
book_big_said() {
    grep -aoE "$BOOK_BIG_SAID" "$1" | head -1 | sed -E 's/.*: ([0-9]+) bytes, print (0x[0-9A-F]+)$/\1 \2/'
}

book_big() {
    local name=book_big img="$SCRATCH/big.img" json="$SCRATCH/census.book_big.json"
    local w="$SCRATCH/serial.book_big_write.log" parts said again got line rc
    echo "== book big: a file past the room a machine is given, in one write, read back by both readers =="
    rm -f "$img" "$w" "$SCRATCH/serial.$name.log"
    if ! build_with_key VOLUME_FORMAT=v2; then
        bad "big: make VOLUME_FORMAT=v2 does not build: $(grep -aE 'ERROR|error' "$SCRATCH/build.log" | tail -1)"
        return
    fi
    cp build/boxos.img "$img" || { harness "big: could not copy build/boxos.img"; return; }
    # The volume's partition and the ESP's, the volume's grown in the file and
    # the table written again with it.
    parts=$(python3 - "$img" "$BOOK_BIG_GROW_MIB" 2>&1 <<'BIG_EOF'
import os, struct, sys
with open(sys.argv[1], "rb") as f:
    mbr = f.read(512)
found = {}
for i in range(4):
    e = mbr[446 + 16 * i:462 + 16 * i]
    found[e[4]] = struct.unpack_from("<II", e, 8)
if 0x7F not in found or 0xEF not in found:
    sys.exit("the image's partition table names no volume (7f) or no ESP (ef)")
start, ground = found[0x7F]
ground += int(sys.argv[2]) * 2048
os.truncate(sys.argv[1], (start + ground) * 512)
# The table read, emptied for make_mbr.py to write it again: it writes only
# into an empty one, so as never to take boot code grown into it.
with open(sys.argv[1], "r+b") as f:
    f.seek(446)
    f.write(bytes(64))
print(start, ground, *found[0xEF])
BIG_EOF
)
    # shellcheck disable=SC2086
    set -- $parts
    if [ $# != 4 ] || ! python3 tools/make_mbr.py "$img" "$1,$2,7f,boot" "$3,$4,ef" >"$SCRATCH/big.mbr" 2>&1 ||
       ! tools/create_tagfs --v2 "$img" src/userspace/shell/shell.bin "utility,system" \
            src/userspace/apps/quilltest.elf "app,utility,test,system" src/userspace/utils/bye.elf "utility,system" \
            build/kernel.bin "system" \
            >"$SCRATCH/big.mkfs" 2>&1; then
        harness "big: the grown image could not be made — ${parts:-no partitions} $(tail -1 "$SCRATCH/big.mkfs" 2>/dev/null)"
        rm -f "$img"
        return
    fi
    python3 tools/volume-read.py "$img" check >"$SCRATCH/big.before" 2>&1
    [ "$(tail -1 "$SCRATCH/big.before")" = "check: consistent — 0 damage, 0 leak, 0 note" ]
    chk $? "big: a book made on $(( $2 / 2048 )) MiB of ground, whole before the boot — $(tail -1 "$SCRATCH/big.before")"
    make run-stop >/dev/null 2>&1
    if ! make run-bg BOOTDISK="$img" MEM=2G >"$SCRATCH/big.run-bg" 2>&1; then
        harness "big: run-bg would not start the grown book: $(tail -1 "$SCRATCH/big.run-bg")"
        rm -f "$img"
        return
    fi
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S" && wait_for_prompt &&
        type_line "quilltest big big $BOOK_BIG_MIB" &&
        wait_for_line -E build/serial.log '\[QUILL\] big: ' "$WAIT_QUIET_SLOW_S" && wait_for_prompt &&
        type_line "bye" && wait_for_line -E build/serial.log "$STAND_HALT_ENDS" "$WAIT_QUIET_S"
    rc=$?
    sleep 1
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$w"
    said=$(book_big_said "$w")
    [ -n "$said" ] && [ "${said% *}" = $((BOOK_BIG_MIB * 1048576)) ]
    chk $? "big: the guest writes $BOOK_BIG_MIB MiB in one write, anchors it and reads every byte back ($(grep -aoE '\[QUILL\] big: [A-Z]+ — [^,]*' "$w" | head -1))"
    [ $rc = 0 ] && grep -aq '^\[HALT\] Storage sync complete' "$w"
    chk $? "big: the machine stops, the halt syncing the volume ($(grep -aE '^\[HALT\] (Storage sync complete|.*NOT MADE DURABLE|.*DID NOT WRITE)' "$w" | head -1 | cut -c1-100))"
    python3 tools/volume-read.py "$img" check --json >"$json" 2>"$SCRATCH/big.check.err"
    chk $? "big: volume-read finds the book consistent ($(python3 -c 'import json, sys
print(json.load(open(sys.argv[1])).get("verdict", "no verdict"))' "$json" 2>/dev/null))"
    got=$(python3 tools/volume-read.py "$img" get big-b.d 2>"$SCRATCH/big.get.err" | python3 -c 'import sys
h = 0xCBF29CE484222325
n = 0
while True:
    b = sys.stdin.buffer.read(1 << 20)
    if not b:
        break
    n += len(b)
    for x in b:
        h = ((h ^ x) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
print("%u 0x%016X" % (n, h))' 2>/dev/null)
    [ -n "$said" ] && [ "$got" = "$said" ]
    chk $? "big: volume-read reads big-b.d back as the guest wrote it — ${got:-nothing} (the guest: ${said:-nothing})"
    grep -aqE "$BOOK_BIND_TALLY" "$w"
    chk $? "big: the kernel binds the state the one write left in a Tally its Born's digests owe by TALLY_SLACK ($(grep -aoE "$BOOK_BIND_TALLY" "$w" | tail -1 | cut -c8-))"
    python3 tools/volume-read.py "$img" bound >"$SCRATCH/big.bound" 2>&1 && grep -q '^bound: HELD$' "$SCRATCH/big.bound"
    chk $? "big: that Tally is bound again where it lies byte for byte, and its state is the slot before it and its ribbon's — $(grep -v '^bound: the newest' "$SCRATCH/big.bound" | head -3 | tr '\n' ' ' | cut -c1-300)"
    if ! make run-bg BOOTDISK="$img" MEM=2G >"$SCRATCH/big.again.run-bg" 2>&1; then
        harness "big: run-bg would not start the written book again: $(tail -1 "$SCRATCH/big.again.run-bg")"
        rm -f "$img"
        return
    fi
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S" && wait_for_prompt &&
        type_line "quilltest big big" && wait_for_line -E build/serial.log '\[QUILL\] big: ' "$WAIT_QUIET_SLOW_S"
    rc=$?
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.$name.log"
    [ $rc = 0 ] || bad "big: the written book never reached a shell, or quilltest never answered"
    census_book_judge "$name"
    while IFS= read -r line; do
        case "$line" in
            "ok "*)  ok  "big, the next mount: ${line#ok }" ;;
            "bad "*) bad "big, the next mount: ${line#bad }" ;;
        esac
    done < "$SCRATCH/census.$name.verdict"
    again=$(book_big_said "$SCRATCH/serial.$name.log")
    [ -n "$said" ] && [ "$again" = "$said" ]
    chk $? "big, the next mount: the guest reads big-b.d back whole, every block held to its digest — ${again:-no PASS} (written: ${said:-nothing})"
    ! grep -aqE 'KERNEL PANIC|\[EXCEPTION\]' "$w" "$SCRATCH/serial.$name.log"
    chk $? "big: no panic, no fault"
    rm -f "$img"
}

# ---------------------------------------------------------------------------
# book_stitchcut: the power goes before a stitch's last atom (C2, §3.9)
#
# A kernel built SETTLECUT=stitch stops the machine before the last atom of
# the stitch that settles the first anchor a program waited for — the one
# window of a stitch written as a write of its own (settle/sewing.c,
# StitchWindow), so what lands is a stitch torn short. A plain image of format
# v2 is booted with that kernel; in the guest `quilltest namecut cut` makes
# cut<r>.n with two tags of 3000 bytes — its Coineds take the stitch past one
# atom — and anchors it, round after round until the machine stops: the last
# round said is the file of the torn stitch. The image is held: volume-read
# finds it consistent and its census says the ribbon ends at the stitch before
# the torn one, the torn tail said; the next boot of the same image says the
# same in its own lines (census_book_judge, kernel = volume-read), and in the
# guest `quilltest namecheck cut` finds that round's file absent and a file
# made now given the fid the census gives next — the torn round's own: its
# Born lay in the atom never written, so no sector that landed names it.
# ---------------------------------------------------------------------------

BOOK_STITCHCUT_CUT='\[SETTLECUT\] the power went at point [0-9]+ of [0-9]+'

book_stitchcut() {
    local name=book_stitchcut img="$SCRATCH/stitchcut.img" json="$SCRATCH/census.book_stitchcut.json" rc line
    local w="$SCRATCH/serial.book_stitchcut_write.log" round fid after next=""
    echo "== book stitchcut: the power goes before a stitch's last atom, and both readers stop at the one before =="
    rm -f build/boxos.img "$img" "$w" "$SCRATCH/serial.$name.log"
    if ! build_with_key VOLUME_FORMAT=v2 SETTLECUT=stitch; then
        bad "stitchcut: make VOLUME_FORMAT=v2 SETTLECUT=stitch does not build: $(grep -aE 'ERROR|error' "$SCRATCH/build.log" | tail -1)"
        return
    fi
    cp build/boxos.img "$img"
    make run-stop >/dev/null 2>&1
    if ! make run-bg BOOTDISK="$img" >"$SCRATCH/stitchcut.run-bg" 2>&1; then
        harness "stitchcut: run-bg would not start the book's image: $(tail -1 "$SCRATCH/stitchcut.run-bg")"
        return
    fi
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S" && wait_for_prompt &&
        type_line "quilltest namecut cut" && wait_for_line -E build/serial.log "$BOOK_STITCHCUT_CUT" "$WAIT_QUIET_S"
    rc=$?
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$w"
    [ $rc = 0 ]
    chk $? "stitchcut: the machine stops inside the anchor's stitch ($(grep -aoE "$BOOK_STITCHCUT_CUT[^,]*" "$w" | head -1))"
    line=$(grep -aoE '\[QUILL\] namecut: round [0-9]+, cut[0-9]+\.n fid [0-9]+' "$w" | tail -1)
    round=$(echo "$line" | sed -E 's/.*round ([0-9]+),.*/\1/')
    fid=$(echo "$line" | sed -E 's/.* fid ([0-9]+)$/\1/')
    [ -n "$fid" ]
    chk $? "stitchcut: the round the stitch was torn in is said — ${line:-none}"
    python3 tools/volume-read.py "$img" check --json >"$json" 2>"$SCRATCH/stitchcut.check.err"
    chk $? "stitchcut: volume-read finds the book the cut left consistent ($(python3 -c 'import json, sys
print(json.load(open(sys.argv[1])).get("verdict", "no verdict"))' "$json" 2>/dev/null))"
    python3 - "$json" "${fid:-0}" >"$SCRATCH/stitchcut.verdict" 2>&1 <<'STITCHCUT_EOF'
import json, sys
k = json.load(open(sys.argv[1]))["kernel"]
fid = int(sys.argv[2])
def say(held, text):
    print(("ok " if held else "bad ") + text)
say(k["mounts"] and k["glass"] == 0, "the book mounts, with no reason to be only read — %s" % (k.get("refusal") or "%u reason(s)" % k["glass"]))
say(k["ribbon"].get("RIBBON_TORN", 0) == 1 and not k["lied"],
    "its census ends the ribbon at the stitch before the torn one, the torn tail said once: %s, lied %s"
    % (sorted(k["ribbon"].items()) or "nothing said", k["lied"]))
say(k["next_fid"] == fid, "and gives the next file id %u — the torn round's own, %u: its Born lay past the "
    "Coineds that took the stitch's first atom, in the atom never written, so no sector that landed names it and "
    "the id is given again (§4.5 п.8)" % (k["next_fid"], fid))
print("next %u" % k["next_fid"])
STITCHCUT_EOF
    while IFS= read -r line; do
        case "$line" in
            "ok "*)   ok  "stitchcut: ${line#ok }" ;;
            "next "*) next=${line#next } ;;
            *)        bad "stitchcut: ${line#bad }" ;;
        esac
    done < "$SCRATCH/stitchcut.verdict"
    if ! make run-bg BOOTDISK="$img" >"$SCRATCH/stitchcut.again.run-bg" 2>&1; then
        harness "stitchcut: run-bg would not start the cut book again: $(tail -1 "$SCRATCH/stitchcut.again.run-bg")"
        return
    fi
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S" && wait_for_prompt &&
        type_line "quilltest namecheck cut" && wait_for_line -E build/serial.log '\[QUILL\] namecheck: ' "$WAIT_QUIET_S"
    rc=$?
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.$name.log"
    [ $rc = 0 ] || bad "stitchcut: the cut book never reached a shell, or namecheck never answered"
    census_book_judge "$name"
    while IFS= read -r line; do
        case "$line" in
            "ok "*)  ok  "stitchcut, the next mount: ${line#ok }" ;;
            "bad "*) bad "stitchcut, the next mount: ${line#bad }" ;;
        esac
    done < "$SCRATCH/census.$name.verdict"
    line=$(grep -aoE '\[QUILL\] namecheck: PASS — rounds present: [^;]*; a file made now takes fid [0-9]+' "$SCRATCH/serial.$name.log" | head -1)
    after=$(echo "$line" | sed -E 's/.* fid ([0-9]+)$/\1/')
    ! echo "$line" | sed -E 's/.*present: ([^;]*);.*/\1/' | tr ',' '\n' | grep -qx "${round:-x}" &&
        [ -n "$after" ] && [ -n "$next" ] && [ "$after" = "$next" ]
    chk $? "stitchcut, the next mount: the torn round's file is absent, and a file made now takes the fid the census gives next (${line:-no namecheck line}; torn round ${round:-?}, fid ${fid:-?}, next ${next:-?})"
    ! grep -aqE 'KERNEL PANIC|\[EXCEPTION\]' "$w" "$SCRATCH/serial.$name.log"
    chk $? "stitchcut: no panic, no fault"
    rm -f "$img"
    buildkey_restore
}

# census_book_all — the book's half of the census: its landings, the book the
# kernel writes read back by both readers, its Tallies, cut and rotten, and
# Escheat (logcheck_book.sh, logcheck_book_circle.sh, logcheck_bind.sh,
# logcheck_escheat.sh).
census_book_all() {
    census_book
    book_circle
    book_big
    book_stitchcut
    book_bind
    book_bindcut tally
    book_bindcut slot
    book_bindcut tally second
    book_bindcut slot second
    book_bindrot B4
    book_bindrot B6c
    book_escheat B4
    book_escheat B6c
    book_escheat B4g 1
}
