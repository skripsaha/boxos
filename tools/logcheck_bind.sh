# shellcheck shell=bash
# logcheck_bind.sh — the kernel's Tallies of a book, held by both
# readers (sourced by tools/logcheck.sh beside logcheck_book.sh; run by the
# census scenario).
#
#   book_bind     a logger's stitches taken in by the Tallies TALLY_SLACK owes:
#                 the newest Tally bound again where it lies byte for byte and
#                 the state it is of read off the slot before it and its ribbon
#                 (volume-read bound), the ribbon past it no longer than a
#                 Tally costs, and the next mount standing on it
#   book_bindcut  the power going before a Tally's flush (C8) or inside its
#                 slot (C9): the slot before stands, the nodes no slot names are
#                 nobody's, and both readers say the same of what is left

# ---------------------------------------------------------------------------
# book_bind
#
# A plain image is booted; in the guest the state book_circle
# leaves is made first — a Use Context said (`use bind sewn`), files made,
# tagged, renamed, untagged and given up (`quilltest names bind`), a file the
# maker laid given up blocks and all (`quilltest ground bind`), a file written
# in rounds on fresh ground, over a hole and inside it (`quilltest data bind`)
# — so the Tallies after it carry every tree full; then `quilltest log bind N`
# appends N lines to a file one at a time, each anchored — a logger, a stitch a
# line — and every byte is read back; the machine stops (`bye`). Its ribbon
# past the newest slot grows by the stitches' useful bytes, and once those are
# as many as a Tally costs (TALLY_SLACK) the kernel writes a whole Tally of the
# state after the last stitch settled and names it by the slot of the least
# seq (settle/binder.h). The image is then held: volume-read finds it
# consistent; `bound` binds the newest Tally again where it lies and holds it
# byte for byte (binder.Circle — the one rule of packing three writers keep,
# §16.1 п.7), and holds its state — files, runs and births, tags, the Use
# Context, the digests, the numbers given next, where the ribbon goes on — to
# the state the slot before it and its ribbon replayed to the stitch it names
# hold; the useful bytes of the ribbon past its `at` are no more than a Tally
# costs, a stitch aside. The next boot stands on the kernel's slot and says
# exactly what the census predicts (census_book_judge).
# ---------------------------------------------------------------------------

BOOK_BIND_LINES=6000
BOOK_BIND_SAID='\[QUILL\] log: PASS — [^ ]+ \(fid [0-9]+\): [0-9]+ lines'
BOOK_BIND_TALLY='^\[TagFS\] Tally [0-9]+ is written over slot [0-9]+: the state after stitch [0-9]+'

book_bind() {
    local name=book_bind img="$SCRATCH/bind.img" json="$SCRATCH/census.book_bind.json"
    local w="$SCRATCH/serial.book_bind_write.log" rc tallies line
    echo "== book bind: a logger's stitches taken in by the kernel's Tallies, held to the one rule of binding =="
    rm -f build/boxos.img "$img" "$w" "$SCRATCH/serial.$name.log"
    if ! make >"$SCRATCH/build.log" 2>&1; then
        bad "bind: make does not build: $(grep -aE 'ERROR|error' "$SCRATCH/build.log" | tail -1)"
        return
    fi
    cp build/boxos.img "$img"
    make run-stop >/dev/null 2>&1
    if ! make run-bg BOOTDISK="$img" >"$SCRATCH/bind.run-bg" 2>&1; then
        harness "bind: run-bg would not start the book's image: $(tail -1 "$SCRATCH/bind.run-bg")"
        return
    fi
    wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S" && wait_for_prompt &&
        type_line "use bind sewn" && wait_for_prompt && type_line "quilltest names bind" &&
        wait_for_line -E build/serial.log '\[QUILL\] names: ' "$WAIT_QUIET_S" && wait_for_prompt &&
        type_line "quilltest ground bind $BOOK_CIRCLE_GROUND" &&
        wait_for_line -E build/serial.log '\[QUILL\] ground: ' "$WAIT_QUIET_S" && wait_for_prompt &&
        type_line "quilltest data bind" &&
        wait_for_line -E build/serial.log '\[QUILL\] data: ' "$WAIT_QUIET_S" && wait_for_prompt &&
        type_line "quilltest log bind $BOOK_BIND_LINES" &&
        wait_for_line -E build/serial.log '\[QUILL\] log: ' "$WAIT_QUIET_SLOW_S" && wait_for_prompt &&
        type_line "bye" && wait_for_line -E build/serial.log "$STAND_HALT_ENDS" "$WAIT_QUIET_S"
    rc=$?
    sleep 1
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$w"
    grep -aq '\[QUILL\] names: PASS' "$w" && grep -aq '\[QUILL\] ground: PASS' "$w" &&
        grep -aq '\[QUILL\] data: PASS' "$w"
    chk $? "bind: the guest leaves the state book_circle leaves — names, a file given up blocks and all, data on fresh ground ($(grep -aoE '\[QUILL\] (names|ground|data): [A-Z]+' "$w" | tr '\n' ' '))"
    grep -aqE "$BOOK_BIND_SAID" "$w"
    chk $? "bind: the guest appends $BOOK_BIND_LINES lines one at a time, each anchored, and reads every byte back ($(grep -aoE '\[QUILL\] log: [A-Z]+ — [^,]*' "$w" | head -1))"
    tallies=$(grep -acE "$BOOK_BIND_TALLY" "$w")
    [ "$tallies" -ge 2 ]
    chk $? "bind: the kernel writes the Tallies the logger's ribbon owes by TALLY_SLACK — $tallies ($(grep -aoE "$BOOK_BIND_TALLY" "$w" | tail -1 | cut -c8-))"
    [ $rc = 0 ] && grep -aq '^\[HALT\] Storage sync complete' "$w"
    chk $? "bind: the machine stops, the halt syncing the volume ($(grep -aE '^\[HALT\] (Storage sync complete|.*NOT MADE DURABLE|.*DID NOT WRITE)' "$w" | head -1 | cut -c1-100))"
    python3 tools/volume-read.py "$img" check --json >"$json" 2>"$SCRATCH/bind.check.err"
    chk $? "bind: volume-read finds the book consistent ($(python3 -c 'import json, sys
print(json.load(open(sys.argv[1])).get("verdict", "no verdict"))' "$json" 2>/dev/null))"
    python3 tools/volume-read.py "$img" bound >"$SCRATCH/bind.bound" 2>&1
    rc=$?
    [ $rc = 0 ] && grep -q '^bound: HELD$' "$SCRATCH/bind.bound"
    chk $? "bind: the newest Tally is bound again where it lies byte for byte, and its state is the slot before it and its ribbon's — $(grep -v '^bound: the newest' "$SCRATCH/bind.bound" | head -3 | tr '\n' ' ' | cut -c1-300)"
    line=$(grep -E '^bound: the newest slot' "$SCRATCH/bind.bound" | head -1)
    python3 - "$line" "$tallies" "$BOOK_BIND_LINES" >"$SCRATCH/bind.slack" 2>&1 <<'BIND_EOF'
import re, sys
m = re.search(r"Tally (\d+), after stitch (\d+): (\d+) block\(s\) a copy, the next reckoned at (\d+) byte\(s\) "
              r"of a whole (\d+); (\d+) useful byte\(s\) of ribbon past its at, (\d+) stitch", sys.argv[1])
if not m:
    print("bad the bound line names no Tally: %s" % sys.argv[1])
    sys.exit(0)
tally, k, blocks, cost, whole, useful, sewn = map(int, m.groups())
# A stitch of the logger is a few hundred bytes at the most; the Tally falls
# owed at the stitch whose bytes reach the cost, and is written a step a turn.
print(("ok " if tally > 1 and useful <= cost + 4096 else "bad ") +
      "the ribbon past the newest slot's at holds %u useful byte(s), no more than the %u a Tally of %u block(s) a "
      "copy costs and a stitch (Tally %u, after stitch %u, %u stitch(es) past it)" % (useful, cost, blocks, tally,
                                                                                   k, sewn))
# The next Tally lays in the leaves the logger's stitches past `at` changed and
# the path above them, both copies, and the slot (§4.2) — its file's leaf and a
# leaf of DIGESTS — not the whole book: TALLY_SLACK by that cost owes a Tally a
# couple of hundred stitches apart (§12.1), not a whole book's bytes apart.
print(("ok " if 0 < cost < whole else "bad ") +
      "the next Tally is reckoned at %u byte(s), the leaves the stitches past `at` changed and the path above "
      "them, against %u for the whole book (§4.2, §16.5 п.16)" % (cost, whole))
# The arithmetic of it: TALLY_SLACK (1) times that cost over the useful bytes
# a stitch of the logger carries is how many stitches apart its Tallies fall —
# the logger's lines owe at least half as many Tallies as that spacing gives
# them, and never fewer than one each thousand stitches.
tallies, lines = int(sys.argv[2]), int(sys.argv[3])
apart = cost * sewn // useful if useful and sewn else 0
print(("ok " if 0 < apart <= 1000 and 2 * tallies * apart >= lines else "bad ") +
      "the logger's Tallies fall %u stitch(es) apart by TALLY_SLACK — %u useful byte(s) a stitch against a cost of "
      "%u — and its %u line(s) owe at least %u of them: %u written" % (apart, useful // sewn if sewn else 0, cost,
                                                                     lines, lines // (2 * apart) if apart else 0,
                                                                     tallies))
BIND_EOF
    while IFS= read -r line; do
        case "$line" in
            "ok "*) ok  "bind: ${line#ok }" ;;
            *)      bad "bind: ${line#bad }" ;;
        esac
    done <"$SCRATCH/bind.slack"
    if ! make run-bg BOOTDISK="$img" >"$SCRATCH/bind.again.run-bg" 2>&1; then
        harness "bind: run-bg would not start the written book again: $(tail -1 "$SCRATCH/bind.again.run-bg")"
        return
    fi
    wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S"
    rc=$?
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.$name.log"
    [ $rc = 0 ] || bad "bind: the written book never reached a shell"
    census_book_judge "$name"
    while IFS= read -r line; do
        case "$line" in
            "ok "*)  ok  "bind, the next mount: ${line#ok }" ;;
            "bad "*) bad "bind, the next mount: ${line#bad }" ;;
        esac
    done <"$SCRATCH/census.$name.verdict"
    ! grep -aqE '^\[TagFS\] the book stands on slot [0-9]+ \(seq [0-9]+\): its Tally of seq 1,' "$SCRATCH/serial.$name.log"
    chk $? "bind, the next mount: it stands on a Tally the kernel wrote ($(grep -aoE '^\[TagFS\] the book stands on slot [^:]*: its Tally of seq [0-9]+' "$SCRATCH/serial.$name.log" | head -1))"
    ! grep -aqE 'KERNEL PANIC|\[EXCEPTION\]' "$w" "$SCRATCH/serial.$name.log"
    chk $? "bind: no panic, no fault"
    rm -f "$img"
}

# ---------------------------------------------------------------------------
# book_bindcut
#
# A kernel built SETTLECUT=tally stops the machine before the flush of the
# nodes of the first Tally of a state carrying the anchor a program waited for
# — the nodes written, no slot naming them (C8); one built SETTLECUT=slot stops
# it with the first half of that Tally's slot written (C9). A plain image
# is booted with that kernel and `quilltest log cut N` appends and
# anchors until the machine stops. The image is held: volume-read finds it
# consistent — the nodes no slot names are nobody's, so its free count is the
# kernel's — the newest slot that holds is the one before the Tally (a torn
# slot said as torn), and the next boot stands on it and says what the census
# predicts (census_book_judge), the stitches the logger's anchors waited for
# all there.
#
# `book_bindcut POINT second` cuts a Tally laid in after one this mount wrote
# (§16.5 п.17): `quilltest crowd N 0 unanchored` makes files enough for a Tally
# and takes no anchor, the cadence settles them and writes that Tally — waited
# for by its line — and only then does the logger take the boot's first
# anchor, the one the cut watches. So the Tally the cut stops goes on in the
# pair of Tally-Quires the one before it laid in — its nodes past that one's in
# a Quire a slot still names. Both readers stand on the slot of the last Tally
# the boot said it wrote, and hold that Quire whole: the census consistent, the
# free count the kernel's. An anchored crowd took the boot's first anchor
# itself: on a book its files and its anchor settle in one cycle of the
# window's (SETTLE_WINDOW_US), and the cut stopped the first Tally of the boot.
# ---------------------------------------------------------------------------

BOOK_BINDCUT_CUT='\[SETTLECUT\] the power went at point [0-9]+ of [0-9]+'
BOOK_BINDCUT_LINES=20000
BOOK_BINDCUT_CROWD=6000
BOOK_BINDCUT_TALLY='^\[TagFS\] Tally [0-9]+ is written over slot [0-9]+'

# book_bindcut_before POINT SECOND — the scenario up to the cut: the logger
# alone, or an unanchored crowd, the Tally the cadence writes of it, and then
# the logger. 0 once the machine stopped.
book_bindcut_before() {
    local said from
    if [ -n "$2" ]; then
        from=$(($(wc -l < build/serial.log) + 1))
        type_line "quilltest crowd $BOOK_BINDCUT_CROWD 0 unanchored" &&
            wait_for_line -E build/serial.log "\[QUILL\] crowd: (PASS|FAIL)|$BOOK_BINDCUT_CUT" "$WAIT_QUIET_SLOW_S" ||
            return 1
        grep -aqE "$BOOK_BINDCUT_CUT" build/serial.log && return 0
        # The prompt comes back before the Tally's lines, which then follow it
        # on its line: the shell is ready once a line past the crowd's verdict
        # begins with it, whatever is written after it. The Tally is one the
        # cadence wrote of the crowd's files — begun once the crowd was asked,
        # never one of the boot's before it.
        said=$(grep -anE '\[QUILL\] crowd: (PASS|FAIL)' build/serial.log | tail -1 | cut -d: -f1)
        wait_for_line build/serial.log '^~ ' "$WAIT_QUIET_SLOW_S" "$((said + 1))" &&
            wait_for_line -E build/serial.log "$BOOK_BINDCUT_TALLY|$BOOK_BINDCUT_CUT" "$WAIT_QUIET_SLOW_S" "$from" ||
            return 1
        grep -aqE "$BOOK_BINDCUT_CUT" build/serial.log && return 0
    fi
    type_line "quilltest log cut $BOOK_BINDCUT_LINES" &&
        wait_for_line -E build/serial.log "$BOOK_BINDCUT_CUT" "$WAIT_QUIET_SLOW_S"
}

book_bindcut() {
    local point=$1 second=${2:-} name="book_bindcut_$1${2:+_$2}" img="$SCRATCH/bindcut.img"
    local json="$SCRATCH/census.book_bindcut_$1${2:+_$2}.json" w="$SCRATCH/serial.book_bindcut_$1${2:+_$2}_write.log"
    local rc line slot=0 before
    echo "== book bindcut $point${second:+ ($second)}: the power goes inside a Tally, and both readers stand on the slot before it =="
    rm -f build/boxos.img "$img" "$w" "$SCRATCH/serial.$name.log"
    if ! build_with_key SETTLECUT="$point"; then
        bad "bindcut $point: make SETTLECUT=$point does not build: $(grep -aE 'ERROR|error' "$SCRATCH/build.log" | tail -1)"
        return
    fi
    cp build/boxos.img "$img"
    make run-stop >/dev/null 2>&1
    if ! make run-bg BOOTDISK="$img" >"$SCRATCH/bindcut.run-bg" 2>&1; then
        harness "bindcut $point: run-bg would not start the book's image: $(tail -1 "$SCRATCH/bindcut.run-bg")"
        return
    fi
    wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S" && wait_for_prompt && book_bindcut_before "$point" "$second"
    rc=$?
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$w"
    [ $rc = 0 ]
    chk $? "bindcut $point: the machine stops inside the anchor's Tally ($(grep -aoE "$BOOK_BINDCUT_CUT, [^,]*" "$w" | head -1))"
    if [ -n "$second" ]; then
        before=$(awk -v cut="$BOOK_BINDCUT_CUT" '$0 ~ cut { exit } { print }' "$w" | grep -aE "$BOOK_BINDCUT_TALLY" | tail -1)
        slot=$(echo "$before" | grep -oE 'slot [0-9]+' | grep -oE '[0-9]+')
        [ -n "$slot" ]
        chk $? "bindcut $point: a Tally of this mount was written before the one the cut stopped, which goes on in its pair (${before:-none said})"
        slot=${slot:-0}
    fi
    python3 tools/volume-read.py "$img" check --json >"$json" 2>"$SCRATCH/bindcut.check.err"
    chk $? "bindcut $point: volume-read finds the book the cut left consistent — the nodes no slot names nobody's ($(python3 -c 'import json, sys
print(json.load(open(sys.argv[1])).get("verdict", "no verdict"))' "$json" 2>/dev/null))"
    python3 - "$json" "$point" "$slot" >"$SCRATCH/bindcut.verdict" 2>&1 <<'BINDCUT_EOF'
import json, sys
k = json.load(open(sys.argv[1]))["kernel"]
want = int(sys.argv[3])
def say(held, text):
    print(("ok " if held else "bad ") + text)
say(k["mounts"] and k["glass"] == 0, "the book mounts, with no reason to be only read — %s"
    % (k.get("refusal") or "%u reason(s)" % k["glass"]))
say(k["slot"] == want, "its census stands on slot %u, the one before the Tally the cut stopped (%s): slot %u"
    % (want, "its slot never written" if sys.argv[2] == "tally" else "its slot torn", k["slot"]))
BINDCUT_EOF
    while IFS= read -r line; do
        case "$line" in
            "ok "*) ok  "bindcut $point: ${line#ok }" ;;
            *)      bad "bindcut $point: ${line#bad }" ;;
        esac
    done <"$SCRATCH/bindcut.verdict"
    if ! make run-bg BOOTDISK="$img" >"$SCRATCH/bindcut.again.run-bg" 2>&1; then
        harness "bindcut $point: run-bg would not start the cut book again: $(tail -1 "$SCRATCH/bindcut.again.run-bg")"
        return
    fi
    wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S"
    rc=$?
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.$name.log"
    [ $rc = 0 ] || bad "bindcut $point: the cut book never reached a shell"
    census_book_judge "$name"
    while IFS= read -r line; do
        case "$line" in
            "ok "*)  ok  "bindcut $point, the next mount: ${line#ok }" ;;
            "bad "*) bad "bindcut $point, the next mount: ${line#bad }" ;;
        esac
    done <"$SCRATCH/census.$name.verdict"
    ! grep -aqE 'KERNEL PANIC|\[EXCEPTION\]' "$w" "$SCRATCH/serial.$name.log"
    chk $? "bindcut $point: no panic, no fault"
    rm -f "$img"
    buildkey_restore
}

# ---------------------------------------------------------------------------
# book_bindrot PLANT
#
# A node no copy of which reads, named by the kernel's Tally as it lies
# (§16.4 п.5). The landing PLANT (B4: a FILES leaf rotten in both copies, a
# file whose header the leaf before held locked by name with it; B6b: and the
# last DIGESTS leaf rotten too) is planted in a fresh image; in the guest
# `quilltest log rot N` sews until the kernel writes a Tally over it. The image
# is then held: volume-read finds it consistent; `bound` binds the newest Tally
# again byte for byte — the rotten node named by its old pointer at its old
# separator, its count the old parent's, the locked file's items as they read —
# and holds its state to the planted Tally's and the ribbon's; the census reads
# the same rotten ranges, the same files locked and the same blocks held as
# before the Tally; and the next boot says what the census predicts.
# ---------------------------------------------------------------------------

BOOK_BINDROT_LINES=12000

book_bindrot() {
    local plant=$1 name="book_bindrot_$1" img="$SCRATCH/bindrot.img" before="$SCRATCH/bindrot.before.json"
    local json="$SCRATCH/census.book_bindrot_$1.json" w="$SCRATCH/serial.book_bindrot_$1_write.log" rc line
    echo "== book bindrot $plant: a node no copy of which reads, named by the kernel's Tally as it lies =="
    rm -f build/boxos.img "$img" "$w" "$SCRATCH/serial.$name.log"
    build
    make run-stop >/dev/null 2>&1
    if ! python3 tools/volume-read.py build/boxos.img self-test --emit "$plant" --out "$img" \
            >"$SCRATCH/bindrot.emit" 2>&1; then
        bad "bindrot $plant: it could not be planted in a fresh image — $(tail -1 "$SCRATCH/bindrot.emit")"
        return
    fi
    python3 tools/volume-read.py "$img" check --json >"$before" 2>/dev/null
    if ! make run-bg BOOTDISK="$img" >"$SCRATCH/bindrot.run-bg" 2>&1; then
        harness "bindrot $plant: run-bg would not start the planted book: $(tail -1 "$SCRATCH/bindrot.run-bg")"
        return
    fi
    wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S" && wait_for_prompt &&
        type_line "quilltest log rot $BOOK_BINDROT_LINES" &&
        wait_for_line -E build/serial.log '\[QUILL\] log: ' "$WAIT_QUIET_SLOW_S" && wait_for_prompt &&
        type_line "bye" && wait_for_line -E build/serial.log "$STAND_HALT_ENDS" "$WAIT_QUIET_S"
    rc=$?
    sleep 1
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$w"
    [ $rc = 0 ] && grep -aqE "$BOOK_BIND_SAID" "$w" && grep -aqE "$BOOK_BIND_TALLY" "$w"
    chk $? "bindrot $plant: the guest sews until the kernel writes a Tally over the rotten node ($(grep -aoE "$BOOK_BIND_TALLY" "$w" | tail -1 | cut -c8-))"
    python3 tools/volume-read.py "$img" check --json >"$json" 2>"$SCRATCH/bindrot.check.err"
    python3 -c 'import json, sys
a, b = (json.load(open(p)) for p in sys.argv[1:3])
print("%s %s" % (b.get("verdict"), "yes" if a.get("verdict") == b.get("verdict") and
      [f["code"] for f in a.get("findings", [])] == [f["code"] for f in b.get("findings", [])] else "no"))' \
        "$before" "$json" >"$SCRATCH/bindrot.verdicts" 2>&1
    grep -q ' yes$' "$SCRATCH/bindrot.verdicts"
    chk $? "bindrot $plant: volume-read finds what it found before the Tally, and nothing more ($(cat "$SCRATCH/bindrot.verdicts"))"
    python3 tools/volume-read.py "$img" bound >"$SCRATCH/bindrot.bound" 2>&1
    [ $? = 0 ] && grep -q '^bound: HELD$' "$SCRATCH/bindrot.bound"
    chk $? "bindrot $plant: the kernel's Tally is bound again byte for byte, the rotten node named as it lies — $(grep -v '^bound: the newest' "$SCRATCH/bindrot.bound" | head -3 | tr '\n' ' ' | cut -c1-300)"
    python3 - "$before" "$json" >"$SCRATCH/bindrot.verdict" 2>&1 <<'BINDROT_EOF'
import json, sys
a, b = json.load(open(sys.argv[1]))["kernel"], json.load(open(sys.argv[2]))["kernel"]
for key, what in (("rotten", "the rotten ranges, by tree and count"), ("locked", "the files locked by name"),
                  ("held", "the blocks held")):
    print(("ok " if a[key] == b[key] else "bad ") + "%s are as the planted Tally's: %r — before the Tally %r"
          % (what, b[key], a[key]))
print(("ok " if b["slot"] != a["slot"] else "bad ") + "and the census stands on the kernel's slot %u, not the "
      "planted %u" % (b["slot"], a["slot"]))
BINDROT_EOF
    while IFS= read -r line; do
        case "$line" in
            "ok "*) ok  "bindrot $plant: ${line#ok }" ;;
            *)      bad "bindrot $plant: ${line#bad }" ;;
        esac
    done <"$SCRATCH/bindrot.verdict"
    if ! make run-bg BOOTDISK="$img" >"$SCRATCH/bindrot.again.run-bg" 2>&1; then
        harness "bindrot $plant: run-bg would not start the written book again: $(tail -1 "$SCRATCH/bindrot.again.run-bg")"
        return
    fi
    wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S"
    rc=$?
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.$name.log"
    [ $rc = 0 ] || bad "bindrot $plant: the written book never reached a shell"
    census_book_judge "$name"
    while IFS= read -r line; do
        case "$line" in
            "ok "*)  ok  "bindrot $plant, the next mount: ${line#ok }" ;;
            "bad "*) bad "bindrot $plant, the next mount: ${line#bad }" ;;
        esac
    done <"$SCRATCH/census.$name.verdict"
    ! grep -aqE 'KERNEL PANIC|\[EXCEPTION\]' "$w" "$SCRATCH/serial.$name.log"
    chk $? "bindrot $plant: no panic, no fault"
    rm -f "$img"
}
