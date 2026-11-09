# shellcheck shell=bash
# Sourced by tools/logcheck.sh: the book's Escheat on a rotten range.

# ---------------------------------------------------------------------------
# book_escheat PLANT [KEPT [written]]
#
# Escheat — the act that lets go what a format-v2 book can never give back
# (spec_v2 §16.5 п.20). The landing PLANT (B4: a FILES leaf rotten in both
# copies, its files locked and their blocks held; B6c: and the first DIGESTS
# leaf rotten too; B4g: B4 and a file locked beside it for its Parcels, which
# the act does not let go — KEPT 1) is planted in a fresh image. In the guest `quilltest
# escheat refused` takes the system tag off itself and asks: a cabin wearing
# neither system nor god is refused and nothing is let go. `quilltest escheat
# act` asks wearing system, and the kernel writes the Tally that names none of
# it. The image is then held: the census stands on that Tally — no range
# rotten, no block held, the free ground grown by what was held; volume-read
# takes it (the Tally may leave out a range the slot before named rotten); and
# the next boot says what the census predicts. Asked on a volume nobody wrote
# to since its mount, the Tally the act asks may not be begun before a stitch
# of the mount settles, and the ask asks the cycle that lays it; `written`
# makes a file and anchors it first, so the act is asked where a Tally may be
# begun at once and the ask wakes the committer to begin it (binder.c
# BinderTallyAsked).
# ---------------------------------------------------------------------------

BOOK_ESCHEAT_REFUSED='^\[QUILL\] escheat refused: '
BOOK_ESCHEAT_ASKED='^\[QUILL\] escheat asked: answered 0 — [1-9][0-9]* range'
BOOK_ESCHEAT_LAID='^\[TagFS\] Escheat laid by Tally [0-9]+: '

# book_escheat_written [written] — nothing, or a file made and anchored, and
# the shell back.
book_escheat_written() {
    [ -z "$1" ] && return 0
    type_line "quilltest crowd 1 0" &&
        wait_for_line -E build/serial.log '\[QUILL\] crowd: (PASS|FAIL)' "$WAIT_QUIET_S" && wait_for_prompt
}

book_escheat() {
    local plant=$1 kept=${2:-0} written=${3:-} name="book_escheat_$1${3:+_$3}" img="$SCRATCH/escheat.img"
    local before="$SCRATCH/escheat.before.json" json="$SCRATCH/census.book_escheat_$1${3:+_$3}.json"
    local w="$SCRATCH/serial.book_escheat_$1${3:+_$3}_write.log" what="$1${3:+ $3}" rc line
    echo "== book escheat $plant${written:+ ($written)}: a rotten range let go by the act, and the Tally after it =="
    rm -f build/boxos.img "$img" "$w" "$SCRATCH/serial.$name.log"
    build
    make run-stop >/dev/null 2>&1
    if ! python3 tools/volume-read.py build/boxos.img self-test --emit "$plant" --out "$img" \
            >"$SCRATCH/escheat.emit" 2>&1; then
        bad "escheat $what: it could not be planted in a fresh image — $(tail -1 "$SCRATCH/escheat.emit")"
        return
    fi
    python3 tools/volume-read.py "$img" check --json >"$before" 2>/dev/null
    if ! make run-bg BOOTDISK="$img" >"$SCRATCH/escheat.run-bg" 2>&1; then
        harness "escheat $what: run-bg would not start the planted book: $(tail -1 "$SCRATCH/escheat.run-bg")"
        return
    fi
    wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S" && wait_for_prompt &&
        type_line "quilltest escheat refused" &&
        wait_for_line -E build/serial.log '\[QUILL\] escheat (refused|NOT REFUSED): ' "$WAIT_QUIET_S" &&
        wait_for_prompt && book_escheat_written "$written" && type_line "quilltest escheat act" &&
        wait_for_line -E build/serial.log '\[QUILL\] escheat (asked|FAILED): ' "$WAIT_QUIET_S" &&
        wait_for_line -E build/serial.log "$BOOK_ESCHEAT_LAID" "$WAIT_QUIET_SLOW_S" && wait_for_prompt &&
        type_line "bye" && wait_for_line -E build/serial.log "$STAND_HALT_ENDS" "$WAIT_QUIET_S"
    rc=$?
    sleep 1
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$w"
    grep -aqE "$BOOK_ESCHEAT_REFUSED" "$w"
    chk $? "escheat $what: a cabin wearing neither system nor god is refused ($(grep -aoE '\[QUILL\] escheat (refused|NOT REFUSED): .*' "$w" | head -1 | cut -c1-160))"
    [ $rc = 0 ] && grep -aqE "$BOOK_ESCHEAT_ASKED" "$w" && grep -aqE "$BOOK_ESCHEAT_LAID" "$w"
    chk $? "escheat $what: asked wearing system, the act is taken and the next Tally names none of it ($(grep -aoE "$BOOK_ESCHEAT_LAID.*" "$w" | tail -1 | cut -c1-200))"
    python3 tools/volume-read.py "$img" check --json >"$json" 2>"$SCRATCH/escheat.check.err"
    python3 - "$before" "$json" "$kept" >"$SCRATCH/escheat.verdict" 2>&1 <<'ESCHEAT_EOF'
import json, sys
a, b = (json.load(open(p)) for p in sys.argv[1:3])
kept = int(sys.argv[3])
ka, kb = a["kernel"], b["kernel"]
grain = 512
print(("ok " if ka["held"] > 0 and ka["rotten"] else "bad ") + "the planted book holds %u block(s) for %r"
      % (ka["held"], ka["rotten"]))
held_ok = kb["held"] == 0 if not kept else 0 < kb["held"] < ka["held"]
print(("ok " if held_ok and not kb["rotten"] and len(kb["locked"]) == kept and set(kb["locked"]) <= set(ka["locked"])
       else "bad ") + "after the act no range is rotten, only the %u file(s) locked for another reason still locked "
      "and their blocks alone held: %r, %r, %u" % (kept, kb["rotten"], kb["locked"], kb["held"]))
print(("ok " if kb["free"] + grain >= ka["free"] + ka["held"] - kb["held"] else "bad ") +
      "the free ground grew by what the act let go, but a pair of Tally-Quires the Tally may have taken: %u -> %u "
      "(held %u -> %u)" % (ka["free"], kb["free"], ka["held"], kb["held"]))
allowed = {"PARCEL_GAP", "HELD_BLOCKS"} if kept else set()
damage = [f["code"] for f in b.get("findings", []) if f.get("severity") == "damage" and f["code"] not in allowed]
print(("ok " if not damage else "bad ") + "volume-read finds no damage after the act but the locked file's: %s %s"
      % (b.get("verdict"), damage[:4]))
ESCHEAT_EOF
    while IFS= read -r line; do
        case "$line" in
            "ok "*) ok  "escheat $what: ${line#ok }" ;;
            *)      bad "escheat $what: ${line#bad }" ;;
        esac
    done <"$SCRATCH/escheat.verdict"
    python3 tools/volume-read.py "$img" bound >"$SCRATCH/escheat.bound" 2>&1
    [ $? = 0 ] && grep -q '^bound: HELD$' "$SCRATCH/escheat.bound"
    chk $? "escheat $what: the Tally after the act is bound again and taken against the slot before — $(grep -v '^bound: the newest' "$SCRATCH/escheat.bound" | head -3 | tr '\n' ' ' | cut -c1-300)"
    if ! make run-bg BOOTDISK="$img" >"$SCRATCH/escheat.again.run-bg" 2>&1; then
        harness "escheat $what: run-bg would not start the book again: $(tail -1 "$SCRATCH/escheat.again.run-bg")"
        return
    fi
    wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S"
    rc=$?
    make run-stop >/dev/null 2>&1
    cp build/serial.log "$SCRATCH/serial.$name.log"
    [ $rc = 0 ] || bad "escheat $what: the book after the act never reached a shell"
    census_book_judge "$name"
    while IFS= read -r line; do
        case "$line" in
            "ok "*)  ok  "escheat $what, the next mount: ${line#ok }" ;;
            "bad "*) bad "escheat $what, the next mount: ${line#bad }" ;;
        esac
    done <"$SCRATCH/census.$name.verdict"
    ! grep -aqE 'KERNEL PANIC|\[EXCEPTION\]' "$w" "$SCRATCH/serial.$name.log"
    chk $? "escheat $what: no panic, no fault"
    rm -f "$img"
}
