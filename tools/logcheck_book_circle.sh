# logcheck_book_circle — the book the kernel writes, read back by both readers,
# for tools/logcheck.sh (book_circle), and the order its writes reached the
# medium in, held to §3.3.
#
# SOURCED, NEVER RUN — by logcheck.sh, after logcheck_book.sh, whose judge of a
# book's boot (census_book_judge) it uses; the boots, the waits and the guest's
# hands are logcheck.sh's and stand.sh's. Its own file so logcheck_book.sh
# stays under the thousand lines a file of this tree keeps to.

# ---------------------------------------------------------------------------
# book_circle: the book the kernel writes, read back by both readers
#
# What the kernel lays on a book is read again by the kernel's own reader and
# by volume-read's, written apart from it (settle/sewing.c against
# stitch.py / bookmount.py). A plain image of format v2 is booted; in the guest
# the person says a Use Context (`use circle sewn`) — a stitch with its
# UseContext, the tags the boot gave ids in Coineds — a program makes, tags,
# renames, untags and gives up files in three anchored rounds (`quilltest
# names circle`: Borns, Labeleds and a Quitclaim, each round a stitch), gives
# up a file the maker laid with blocks of its own (BOOK_CIRCLE_GROUND) and runs
# the ribbon past that Quitclaim's Quire with anchored renames (`quilltest
# ground circle`), writes a file in rounds on fresh ground — written,
# appended, written over whole and in part, cut, grown across a hole and
# written inside it, each round anchored (`quilltest data circle`, its size
# and the FNV-1a of its bytes said) — and the machine stops (`bye`), whose
# halt lays its last stitch CLEAN. That boot's disk is recorded (POWERCUT=on),
# and the order it finished the writes in is held to §3.3: the data, a flush,
# the stitch that names it, a flush (book_circle_order). The image the machine
# left is then held:
# volume-read finds it consistent and reads the written file back byte for
# byte as the guest read it; its census says what the next mount must — the
# stitches sewn on and the last, the Use Context as said, CLEAN on the last,
# the given-up file's ground let go by the ribbon and lying fallow (M-1), the
# free count — and the next boot of the same image says exactly that in its
# own lines (census_book_judge), the print of what memory holds — the files
# the program left, by name, tags and runs — included.
# ---------------------------------------------------------------------------

BOOK_CIRCLE_USE="circle,sewn"
BOOK_CIRCLE_GROUND="proca.elf"

book_circle() {
    local name=book_circle img="$SCRATCH/circle.img" json="$SCRATCH/census.book_circle.json" line rc
    local base="$SCRATCH/circle.base.img" record="$SCRATCH/circle.record"
    echo "== book circle: what the kernel writes on a book, read back by both readers =="
    rm -f build/boxos.img "$img" "$base" "$record" "$SCRATCH/serial.book_circle_write.log" "$SCRATCH/serial.$name.log"
    if ! build_with_key VOLUME_FORMAT=v2; then
        bad "circle: make VOLUME_FORMAT=v2 does not build: $(grep -aE 'ERROR|error' "$SCRATCH/build.log" | tail -1)"
        return
    fi
    cp build/boxos.img "$img"
    cp -c "$img" "$base" 2>/dev/null || cp "$img" "$base"
    make run-stop >/dev/null 2>&1
    if ! make run-bg BOOTDISK="$img" POWERCUT=on POWERCUT_LOG="$record" >"$SCRATCH/circle.run-bg" 2>&1; then
        harness "circle: run-bg would not start the book's image: $(tail -1 "$SCRATCH/circle.run-bg")"
        return
    fi
    wait_for_line build/serial.log "BoxOS Shell" "$WAIT_QUIET_S" && wait_for_prompt &&
        type_line "use circle sewn" && wait_for_prompt && type_line "quilltest names circle" &&
        wait_for_line -E build/serial.log '\[QUILL\] names: ' "$WAIT_QUIET_S" && wait_for_prompt &&
        type_line "quilltest ground circle $BOOK_CIRCLE_GROUND" &&
        wait_for_line -E build/serial.log '\[QUILL\] ground: ' "$WAIT_QUIET_S" && wait_for_prompt &&
        type_line "quilltest data circle" &&
        wait_for_line -E build/serial.log '\[QUILL\] data: ' "$WAIT_QUIET_S" && wait_for_prompt &&
        type_line "bye" && wait_for_line -E build/serial.log "$STAND_HALT_ENDS" "$WAIT_QUIET_S"
    rc=$?
    sleep 1
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.book_circle_write.log"
    grep -aqE '\[QUILL\] names: PASS' "$SCRATCH/serial.book_circle_write.log"
    chk $? "circle: the guest makes, tags, renames, untags and gives up files in three anchored rounds ($(grep -aoE '\[QUILL\] names: [A-Z]+ — [^,]*' "$SCRATCH/serial.book_circle_write.log" | head -1))"
    grep -aqE '\[QUILL\] ground: PASS' "$SCRATCH/serial.book_circle_write.log"
    chk $? "circle: the guest gives up $BOOK_CIRCLE_GROUND, blocks and all, and runs the ribbon past its Quitclaim's Quire ($(grep -aoE '\[QUILL\] ground: [A-Z]+ — [^,]*' "$SCRATCH/serial.book_circle_write.log" | head -1))"
    grep -aqE '\[QUILL\] data: PASS' "$SCRATCH/serial.book_circle_write.log"
    chk $? "circle: the guest writes a file in anchored rounds on fresh ground and reads back the bytes they wrote ($(grep -aoE '\[QUILL\] data: [A-Z]+ — [^,]*' "$SCRATCH/serial.book_circle_write.log" | head -1))"
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
say(k["sewn"] >= 300 and k["applied"] == k["sewn"] and not k["ribbon"] and not k["lied"],
    "the ribbon carries the kernel's stitches whole, past a Quire of them: %u sewn on, %u applied, %s said, lied %s"
    % (k["sewn"], k["applied"], sorted(k["ribbon"].items()) or "nothing", k["lied"]))
say(k["fallow"] > 0, "the ground of the file the guest gave up, blocks and all, is let go by the ribbon and lies "
    "fallow at the next mount (M-1): %u block(s)" % k["fallow"])
CIRCLE_EOF
    while IFS= read -r line; do
        case "$line" in
            "ok "*)  ok  "circle: ${line#ok }" ;;
            *)       bad "circle: ${line#bad }" ;;
        esac
    done < "$SCRATCH/circle.verdict"
    book_circle_data "$img" "$SCRATCH/serial.book_circle_write.log"
    book_circle_order "$base" "$record" "$img"
    rm -f "$base" "$record"
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

# book_circle_data IMG LOG — the file the guest wrote in rounds (LOG's
# `[QUILL] data: PASS` line: its name, its size and the FNV-1a of its bytes)
# read back from IMG by volume-read: every byte the kernel wrote on fresh
# ground, under the runs and digests its stitches name, as the guest read it.
book_circle_data() {
    local img=$1 log=$2 said name size print got
    said=$(grep -aoE '\[QUILL\] data: PASS — [^ ]+ \(fid [0-9]+\).*: [0-9]+ bytes, print 0x[0-9A-F]+' "$log" | head -1)
    name=$(printf '%s\n' "$said" | sed -E 's/.*PASS — ([^ ]+) .*/\1/')
    size=$(printf '%s\n' "$said" | sed -E 's/.*: ([0-9]+) bytes, print .*/\1/')
    print=$(printf '%s\n' "$said" | sed -E 's/.*print (0x[0-9A-F]+).*/\1/')
    got=$(python3 tools/volume-read.py "$img" get "$name" 2>"$SCRATCH/circle.get.err" | python3 -c 'import sys
b = sys.stdin.buffer.read()
h = 0xCBF29CE484222325
for x in b:
    h = ((h ^ x) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
print("%u 0x%016X" % (len(b), h))' 2>/dev/null)
    [ -n "$said" ] && [ "$got" = "$size $print" ]
    chk $? "circle: volume-read reads ${name:-the written file} back as the guest read it — $got (the guest: $size $print)"
}

# book_circle_order BASE RECORD IMG — the order the disk finished the writes
# of the boot that turned BASE into IMG, from the record blklogwrites kept of
# it (POWERCUT=on), held to §3.3 as §16.2 п.7 puts it for this kernel: data →
# FLUSH#1 → the stitch's atoms → FLUSH. Every stitch the ribbon of IMG carries
# is read by volume-read's own reader (bookmount.py); the boot's are those
# whose sectors the record wrote. The last write of every block of fresh data
# their Born and Shaped records name, made before the stitch's first atom,
# stands behind a flush the atom does not; the last atom of a stitch stands
# behind a flush the next stitch's first does not; and every stitch the boot
# sewed says DATA_FLUSHED. The record is first held to account for IMG, byte
# for byte, or nothing it says means anything.
book_circle_order() {
    local base=$1 record=$2 img=$3 said line
    said=$(python3 tools/volume-read.py "$base" cut "$record" --against "$img" 2>&1)
    if [ $? != 0 ]; then
        harness "circle: the record of the write boot does not account for the image it left — ${said:-no answer}"
        return
    fi
    said=$(python3 tools/volume-read.py "$base" cut "$record" --list 2>/dev/null | grep -cE '(  |; )unowned ')
    [ "$said" = 0 ]
    chk $? "circle: every write of the record is named by blklog_book.py — $said unowned"
    python3 - "$img" "$record" >"$SCRATCH/circle.order" 2>&1 <<'ORDER_EOF'
import bisect, sys
sys.path.insert(0, "tools")
from volumeread import blklog, stitch, tally
from volumeread.bookground import BookGround
from volumeread.bookmount import BookMount
from volumeread.medium import Medium
from volumeread.ondisk import SECTORS_PER_BLOCK

def say(held, text):
    print(("ok " if held else "bad ") + text)

def Fresh(u):
    """The blocks of fresh data a stitch names: its Born and Shaped records'
    DATA runs but SHARED ones (§2.7, stitch.Fresh)."""
    out = []
    for kind, _ign, body, _at in u.records:
        if kind not in (stitch.BORN, stitch.SHAPED):
            continue
        d = stitch.RecordDecode(kind, body, u.seq)
        runs = d["runs"] if kind == stitch.BORN else [x for _a, _m, rs in d["splices"] for x in rs]
        for x in runs:
            if not x.hole and tally.EXT_SHARED not in stitch.Extensions(x.ext):
                out.extend(range(x.start, x.start + x.count))
    return out

medium = Medium(sys.argv[1])
log = blklog.BlkLog(sys.argv[2], medium.SizeSectors())
writes = [w for w in log.Writes() if not w.discard]
touched = {}
for i, w in enumerate(writes):
    for s in range(w.lba, w.lba + w.sectors):
        touched.setdefault(s, []).append(i)
g = BookGround(medium)
m = BookMount(g)
if m.refusal or m.ribbon is None:
    say(False, "the book the boot left does not mount: %s" % (m.refusal or "no ribbon"))
    sys.exit(0)
sewn = older = with_data = blocks = 0
torn, unflagged, unwritten, unflushed, crossed = [], [], [], [], []
before = None
for u in m.ribbon.stitches:
    at = [touched[g.base + p.Vlba()][-1] for p in u.places if g.base + p.Vlba() in touched]
    if not at:
        older += 1
        before = None
        continue
    if not u.Sound() or len(at) != len(u.places):
        torn.append(u.seq)
        continue
    sewn += 1
    first, last = min(at), max(at)
    if not u.head.flags & stitch.DATA_FLUSHED:
        unflagged.append(u.seq)
    if before is not None and writes[before[1]].epoch >= writes[first].epoch:
        crossed.append((before[0], u.seq))
    before = (u.seq, last)
    fresh = Fresh(u)
    with_data += bool(fresh)
    blocks += len(fresh)
    for b in fresh:
        lasts = []
        for k in range(SECTORS_PER_BLOCK):
            seen = touched.get(g.base + b * SECTORS_PER_BLOCK + k, [])
            j = bisect.bisect_left(seen, first)
            lasts.append(seen[j - 1] if j else None)
        if None in lasts:
            unwritten.append((u.seq, b))
        elif writes[max(lasts)].epoch >= writes[first].epoch:
            unflushed.append((u.seq, b))
tail = before is not None and writes[before[1]].epoch < len(log.epochs) - 1
say(sewn > 0 and with_data > 0 and not torn,
    "the record holds every sector of %u stitch(es) the boot sewed, %u naming %u block(s) of fresh data "
    "(%u laid before it; %s not whole) over %u write(s) and %u flush(es)"
    % (sewn, with_data, blocks, older, torn or "none", len(writes), len(log.flush_seqs)))
say(not unwritten, "every block of fresh data a stitch names was written before the stitch's first atom "
    "(%u not: %s)" % (len(unwritten), unwritten[:4]))
say(not unflushed, "and stands behind a flush that atom does not — data, then FLUSH#1, then the stitch "
    "(%u block(s) not: %s)" % (len(unflushed), unflushed[:4]))
say(not crossed and tail, "the atoms of every stitch stand behind a flush the next stitch's first does not, "
    "and the last stitch's behind one (%u crossed: %s; the last behind a flush: %s)" % (len(crossed), crossed[:4], tail))
say(not unflagged, "every stitch the boot sewed says DATA_FLUSHED (%u do not: %s)" % (len(unflagged), unflagged[:4]))
ORDER_EOF
    while IFS= read -r line; do
        case "$line" in
            "ok "*)  ok  "circle, the order: ${line#ok }" ;;
            *)       bad "circle, the order: ${line#bad }" ;;
        esac
    done < "$SCRATCH/circle.order"
}
