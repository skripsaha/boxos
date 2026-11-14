# shellcheck shell=bash
# Sourced by tools/logcheck.sh: the pass Glean runs after each Tally lays a
# copy on its original (dedup).

# ---------------------------------------------------------------------------
# run_dedup
#
# Dedup on a book (spec_v2 §8.2, §16.7 пп.7–9): the image as built — format v2
# — is booted, and in the guest `quilltest dedup dedup MIB` writes dedup-a.d of
# MIB mebibytes and then dedup-b.d of its very bytes, each in one write and
# anchored, no two blocks of either alike; `quilltest log dedup N` appends N
# lines one at a time, each anchored — a logger, whose stitches the Tallies
# TALLY_SLACK owes take in. The pass Glean runs after the Tally that takes the
# copy in finds every block of it among the blocks written since the Tally
# before, holds each to the original's byte for byte, and lays the copy on the
# original's blocks: its line says how many it laid. Both files are read back,
# the machine stops, and volume-read holds the image: the copy lies on the
# original's blocks, every one, and the files' ground grew by the original's
# blocks and the logger's alone — the volume holds the copy's bytes once. The
# next boot says exactly what the census predicts (census_book_judge), and the
# two files read back again.
#
# 16 MiB a file keeps the scenario within minutes; the two of 100 MB of §8.2
# are an example, not the law.
# ---------------------------------------------------------------------------

DEDUP_MIB=16
DEDUP_BLOCKS=$((DEDUP_MIB * 256))
DEDUP_LINES=1500
DEDUP_ROUNDS=8
DEDUP_SAID='^\[TagFS\] Dedup after Tally [0-9]+: '
DEDUP_LAID='[0-9]+ block\(s\) laid on the blocks they duplicate'

# dedup_laid LOG — the blocks the passes in LOG say they laid, all of them.
dedup_laid() {
    grep -aE "$DEDUP_SAID" "$1" | grep -aoE "$DEDUP_LAID" | awk '{ n += $1 } END { print n + 0 }'
}

# dedup_logger — loggers' rounds, each its own file, until the passes say the
# copy's every block is laid. Where a Tally falls among the copy's blocks is
# the ribbon's fill when the copy begins (the size of a Tally against the
# stitches sewn since), so a copy may be taken by two Tallies: the pass after
# the first lays the blocks it found beside the original, and the pass after
# the next finds the rest by the book's DIGESTS, as many Sheets a pass as that
# Tally laid blocks of its own. The logger is what makes the Tallies, and each
# round of it pays one pass as many Sheets as it wrote blocks. 0 when a round
# ended with the copy laid, 1 when the guest said no log, 2 when DEDUP_ROUNDS
# rounds did not lay it.
dedup_logger() {
    local round=1
    while [ $round -le $DEDUP_ROUNDS ]; do
        type_line "quilltest log dedup$round $DEDUP_LINES" &&
            wait_for_line -E build/serial.log "\\[QUILL\\] log: (PASS|FAIL) — dedup$round-l.log|\\[QUILL\\] log: FAIL" "$WAIT_QUIET_SLOW_S" &&
            wait_for_prompt || return 1
        wait_for_quiet 5 build/serial.log
        [ "$(dedup_laid build/serial.log)" -ge "$DEDUP_BLOCKS" ] && return 0
        round=$((round + 1))
    done
    return 2
}

run_dedup() {
    local name=dedup img="$SCRATCH/dedup.img" before="$SCRATCH/dedup.before.json" json="$SCRATCH/census.dedup.json"
    local w="$SCRATCH/serial.dedup_write.log" rc laid line
    echo "== dedup: a file and its copy, the copy laid on its original by the pass after a Tally =="
    rm -f build/boxos.img "$img" "$w" "$SCRATCH/serial.$name.log"
    build
    cp build/boxos.img "$img"
    make run-stop >/dev/null 2>&1
    python3 tools/volume-read.py "$img" check --json >"$before" 2>"$SCRATCH/dedup.before.err"
    if ! make run-bg BOOTDISK="$img" >"$SCRATCH/dedup.run-bg" 2>&1; then
        harness "dedup: run-bg would not start the book's image: $(tail -1 "$SCRATCH/dedup.run-bg")"
        return
    fi
    wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S" && wait_for_prompt &&
        type_line "quilltest dedup dedup $DEDUP_MIB" &&
        wait_for_line -E build/serial.log '\[QUILL\] dedup: (PASS|FAIL)' "$WAIT_QUIET_SLOW_S" && wait_for_prompt &&
        dedup_logger &&
        type_line "quilltest dedup dedup" &&
        wait_for_line -E build/serial.log '\[QUILL\] dedup: (PASS|FAIL) — [^ ]+ \(fid [0-9]+\) and [^ ]+ \(fid [0-9]+\) found' \
            "$WAIT_QUIET_SLOW_S" && wait_for_prompt &&
        type_line "bye" && wait_for_line -E build/serial.log "$STAND_HALT_ENDS" "$WAIT_QUIET_S"
    rc=$?
    sleep 1
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$w"
    grep -aqE '\[QUILL\] dedup: written — ' "$w" && grep -aqE '\[QUILL\] log: PASS' "$w" && ! grep -aqE '\[QUILL\] log: FAIL' "$w"
    chk $? "dedup: the guest writes a file of $DEDUP_MIB MiB and its copy, and a logger's lines, $(grep -ac '\[QUILL\] log: PASS' "$w") round(s) of $DEDUP_LINES ($(grep -aoE '\[QUILL\] (dedup|log): [a-zA-Z]+' "$w" | head -2 | tr '\n' ' '))"
    laid=$(dedup_laid "$w")
    [ "$laid" = "$DEDUP_BLOCKS" ]
    chk $? "dedup: the pass after the Tally lays the copy's $DEDUP_BLOCKS blocks on the original's — $laid laid ($(grep -aE "$DEDUP_SAID" "$w" | tr -d '\r' | head -1 | cut -c1-260))"
    grep -aqE '\[QUILL\] dedup: PASS — [^ ]+ \(fid [0-9]+\) and [^ ]+ \(fid [0-9]+\) found' "$w"
    chk $? "dedup: both files read back after the pass, every byte"
    [ $rc = 0 ] && grep -aq '^\[HALT\] Storage sync complete' "$w"
    chk $? "dedup: the machine stops, the halt syncing the volume ($(grep -aE "$STAND_HALT_ENDS" "$w" | tr -d '\r' | head -1 | cut -c1-100))"
    python3 tools/volume-read.py "$img" check --json >"$json" 2>"$SCRATCH/dedup.check.err"
    python3 - "$before" "$json" "$DEDUP_BLOCKS" >"$SCRATCH/dedup.verdict" 2>&1 <<'DEDUP_EOF'
import json, sys
a, b = (json.load(open(p)) for p in sys.argv[1:3])
n = int(sys.argv[3])

def blocks(runs):
    return {s + i for kind, s, c, *_ in runs if kind in ("data", "shared") for i in range(c)}

def ground(j):
    return {s + i for c in j["census"]["claims"] if c["kind"] in ("file", "shared") for s, k in c["runs"]
            for i in range(k)}

def used(j):
    k = j["kernel"]
    return k["data_blocks"] - (k["free"] or 0) - k["fallow"]

files = {f["name"]: f for f in b["files"]}
orig, copy = files.get("dedup-a.d"), files.get("dedup-b.d")
if not orig or not copy:
    print("bad volume-read finds no dedup-a.d and dedup-b.d: %s" % sorted(files)[:8])
    sys.exit(0)
def order(runs):
    return [s + i for kind, s, c, *_ in runs if kind != "hole" for i in range(c)]

def kinds(runs):
    return sorted({kind for kind, *_ in runs})

ob, cb = blocks(orig["runs"]), blocks(copy["runs"])
oq, cq = order(orig["runs"]), order(copy["runs"])
shared = {s + i for c in b["census"]["claims"] if c["kind"] == "shared" for s, k in c["runs"] for i in range(k)}
print(("ok " if b.get("verdict") == "consistent" else "bad ") + "volume-read finds the book %s" % b.get("verdict"))
print(("ok " if len(oq) == n and cq == oq and kinds(copy["runs"]) == ["shared"] and kinds(orig["runs"]) == ["data"]
       and cb <= shared else "bad ") +
      "the copy lies on the original's blocks, block for block, by runs SHARED: %u block(s) of the original, %u of "
      "the copy, %u at the same place in both, the copy's runs %s and the original's %s, %u of the copy's blocks "
      "claimed shared" % (len(oq), len(cq), sum(1 for x, y in zip(oq, cq) if x == y), kinds(copy["runs"]),
                          kinds(orig["runs"]), len(cb & shared)))
old = {f["fid"] for f in a["files"]}
new = set().union(*[blocks(f["runs"]) for f in b["files"] if f["fid"] not in old and f["name"] != "dedup-b.d"])
g0, g1 = ground(a), ground(b)
print(("ok " if g1 <= g0 | new else "bad ") +
      "the files' ground grew by the new files' blocks but the copy's — %u block(s), the original's %u among "
      "them, not %u: the volume holds the copy's bytes once (%u -> %u, %u outside the files' before and the new "
      "but the copy's)" % (len(g1) - len(g0), len(ob), len(g1) - len(g0) + n, len(g0), len(g1), len(g1 - (g0 | new))))
print(("ok " if used(b) - used(a) < len(new) + n else "bad ") +
      "volume-read says %u block(s) more in use than the image as built, short of the %u two copies would take"
      % (used(b) - used(a), len(new) + n))
DEDUP_EOF
    while IFS= read -r line; do
        case "$line" in
            "ok "*) ok  "dedup: ${line#ok }" ;;
            *)      bad "dedup: ${line#bad }" ;;
        esac
    done <"$SCRATCH/dedup.verdict"
    if ! make run-bg BOOTDISK="$img" >"$SCRATCH/dedup.again.run-bg" 2>&1; then
        harness "dedup: run-bg would not start the book again: $(tail -1 "$SCRATCH/dedup.again.run-bg")"
        return
    fi
    wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S" && wait_for_prompt &&
        type_line "quilltest dedup dedup" &&
        wait_for_line -E build/serial.log '\[QUILL\] dedup: (PASS|FAIL)' "$WAIT_QUIET_SLOW_S"
    rc=$?
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.$name.log"
    [ $rc = 0 ] && grep -aqE '\[QUILL\] dedup: PASS' "$SCRATCH/serial.$name.log"
    chk $? "dedup, the next mount: both files read back, every byte"
    census_book_judge "$name"
    while IFS= read -r line; do
        case "$line" in
            "ok "*)  ok  "dedup, the next mount: ${line#ok }" ;;
            "bad "*) bad "dedup, the next mount: ${line#bad }" ;;
        esac
    done <"$SCRATCH/census.$name.verdict"
    ! grep -aqE 'KERNEL PANIC|\[EXCEPTION\]' "$w" "$SCRATCH/serial.$name.log"
    chk $? "dedup: no panic, no fault"
    rm -f "$img"
}
