# shellcheck shell=bash
# Sourced by tools/logcheck.sh: the disks scenario.

# ---------------------------------------------------------------------------
# disks: the machine's disks read whole and laid by a person's hands
# (boardroom/disk_table.c, disk_table_write.c, utility disks).
#
# Five sticks beside the machine's own disk, each a case the host lays with
# tools/disks.py: a blank one; a blank one addressed in 4096-byte blocks;
# one laid as Windows lays a disk — an ESP, the reserved partition, its data
# and its recovery, each full of bytes its own — with free space after them;
# one whose table's first copy a dead block took; and one whose table was made
# for a smaller disk. `disks` shows them all and the home disk with its volume;
# the blank ones are laid anew and given an ESP and a BoxOS partition, one
# taken away and laid again; Windows' disk is given BoxOS in its free space and
# keeps every byte of its own, and refuses to give up a partition of Windows';
# the damaged one refuses a partition until it is mended, and is mended; the
# smaller-disk table is mended to the whole disk and given a partition; the
# disk the home volume stands on is never laid anew. The halt ends.
#
# On the host, tools/disks.py reads every stick as DiskTable reads it and finds
# what was laid; every medium a cut could leave of a change — the blank stick
# laid, an ESP put on it, Windows' disk given its partition, the damaged table
# mended — reads as the old table or the new, whole; and a second boot reads
# one such cut — the ESP's FAT32 and the far copy of its table landed, the
# first not — as the kernel must: the two copies differ and the first, the
# old, is read; it is mended to it.
# ---------------------------------------------------------------------------

DISKS_STICKS="d512 d4k win dmg small"

# disks_stick NAME — the -drive and -device of the stick NAME.
disks_stick() {
    local blocks=""
    [ "$1" = d4k ] && blocks=",logical_block_size=4096,physical_block_size=4096"
    printf -- '-drive if=none,id=%s,file=%s,format=raw -device usb-storage,drive=%s,id=%sstick%s ' \
        "$1" "$SCRATCH/disks.$1.img" "$1" "$1" "$blocks"
}

# disks_boot TAG STICKS... — the machine run to the shell with the sticks in.
disks_boot() {
    local tag=$1 extra="" s
    shift
    for s; do extra+=$(disks_stick "$s"); done
    make run-stop >/dev/null 2>&1
    if ! make run-bg USB=on CORES=4 MEM=4G QEMUEXTRA="$extra" >"$SCRATCH/disks.$tag.run-bg" 2>&1; then
        harness "disks ($tag): run-bg would not start: $(tail -1 "$SCRATCH/disks.$tag.run-bg")"
        return 1
    fi
    wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S" && wait_for_prompt
}

# disks_halt TAG — bye, the halt waited out, the log kept as serial.disks.TAG.log.
disks_halt() {
    local tag=$1 rc=0
    type_line "bye" && wait_for_line -E build/serial.log "$STAND_HALT_ENDS" "$WAIT_QUIET_S" || rc=1
    sleep 1
    make run-stop >/dev/null 2>&1
    tr -d '\r' < build/serial.log >"$SCRATCH/serial.disks.$tag.log"
    return $rc
}

# disks_ask CMD [ANSWER] — CMD typed, ANSWER typed once it asks yes or no, and
# the prompt waited for.
disks_ask() {
    local asked=$(($(wc -l < build/serial.log) + 1))
    type_line "$1" || return 1
    if [ -n "${2:-}" ]; then
        wait_for_line build/serial.log "(yes or no)" "$WAIT_QUIET_S" "$asked" && type_line "$2" || return 1
    fi
    wait_for_prompt
}

# disks_number SIZE — the number `disks` gave the disk it said is SIZE.
disks_number() {
    tr -d '\r' < build/serial.log | sed -nE "s/^Disk ([0-9]+) — [^,]*, $1, .*/\1/p" | head -1
}

# disks_said PATTERN WHAT — the first boot's log says PATTERN (grep -E).
disks_said() {
    grep -aqE "$1" "$SCRATCH/serial.disks.first.log"
    chk $? "disks: $2"
}

# disks_read NAME [BLOCK] — tools/disks.py's JSON read of a stick, or of any
# image at NAME's path when NAME is a path.
disks_read() {
    local img=$1
    case "$img" in */*) ;; *) img="$SCRATCH/disks.$1.img" ;; esac
    python3 tools/disks.py read "$img" --block "${2:-512}" --json
}

# disks_holds JSON PYTHON — PYTHON, a boolean of the table `t`, holds.
disks_holds() {
    python3 -c 'import json, sys
t = json.loads(sys.argv[1])
sys.exit(0 if eval(sys.argv[2]) else 1)' "$1" "$2"
}

# disks_same_bytes BEFORE AFTER — every partition of BEFORE's table holds the
# same bytes in AFTER.
disks_same_bytes() {
    python3 - "$1" "$2" <<'EOF'
import hashlib, json, subprocess, sys
t = json.loads(subprocess.check_output([sys.executable, "tools/disks.py", "read", sys.argv[1], "--json"]))
a, b = open(sys.argv[1], "rb"), open(sys.argv[2], "rb")
for p in t["parts"]:
    hashes = []
    for f in (a, b):
        f.seek(p["first"] * 512)
        hashes.append(hashlib.sha256(f.read((p["last"] - p["first"] + 1) * 512)).digest())
    if hashes[0] != hashes[1]:
        print("entry %u changed" % p["entry"])
        sys.exit(1)
EOF
}

run_disks() {
    echo "== disks: the machine's disks read whole, and laid by a person's hands =="
    local s n home d512 d4k win dmg small t before rc
    build
    for s in $DISKS_STICKS; do rm -f "$SCRATCH/disks.$s.img"*; done
    python3 -c "import sys; [open(p, 'wb').truncate(int(n) << 20) for p, n in zip(sys.argv[1::2], sys.argv[2::2])]" \
        "$SCRATCH/disks.d512.img" 300 "$SCRATCH/disks.d4k.img" 400
    if ! python3 tools/disks.py lay "$SCRATCH/disks.win.img" 900M --part esp:100M:"EFI system partition" \
            --part msr:16M:"Microsoft reserved partition" --part windows:400M:"Basic data partition" \
            --part recovery:50M --fill ||
       ! python3 tools/disks.py lay "$SCRATCH/disks.dmg.img" 200M --part boxos:64M:BoxOS --part linux:32M:home ||
       ! python3 tools/disks.py damage "$SCRATCH/disks.dmg.img" first ||
       ! python3 tools/disks.py lay "$SCRATCH/disks.small.img" 128M --part esp:32M ||
       ! python3 tools/disks.py grow "$SCRATCH/disks.small.img" 500M; then
        harness "disks: the sticks could not be laid"
        return
    fi
    cp "$SCRATCH/disks.win.img" "$SCRATCH/disks.win.before"
    cp "$SCRATCH/disks.dmg.img" "$SCRATCH/disks.dmg.before"

    # The disks by the sizes they were laid at, as `disks` tells them.
    rc=1
    if disks_boot first $DISKS_STICKS && disks_ask "disks"; then
        home=$(tr -d '\r' < build/serial.log | sed -nE 's/^Disk ([0-9]+) — .*a volume of the house stands on it$/\1/p' | head -1)
        d512=$(disks_number "315 MB") d4k=$(disks_number "419 MB") win=$(disks_number "944 MB")
        dmg=$(disks_number "210 MB") small=$(disks_number "524 MB")
        if [ -n "$home$d512" ] && [ -n "$d4k" ] && [ -n "$win" ] && [ -n "$dmg" ] && [ -n "$small" ] &&
           cp "$SCRATCH/disks.d512.img" "$SCRATCH/disks.d512.blank" &&
           disks_ask "disks $d512 new" yes && cp "$SCRATCH/disks.d512.img" "$SCRATCH/disks.d512.laid" &&
           disks_ask "disks $d512 add efi 64M" yes && cp "$SCRATCH/disks.d512.img" "$SCRATCH/disks.d512.esp" &&
           disks_ask "disks $d512 add boxos" yes &&
           disks_ask "disks $d512 remove 1" yes &&
           disks_ask "disks $d512 add boxos 100M" yes && disks_ask "disks $d512 add efi 20M" &&
           disks_ask "disks $d4k new" yes && disks_ask "disks $d4k add efi 300M" yes &&
           disks_ask "disks $d4k add boxos" yes &&
           disks_ask "disks $win add boxos" yes && cp "$SCRATCH/disks.win.img" "$SCRATCH/disks.win.after" &&
           disks_ask "disks $win remove 2" &&
           disks_ask "disks $dmg" && disks_ask "disks $dmg add boxos" &&
           disks_ask "disks $dmg mend" yes && cp "$SCRATCH/disks.dmg.img" "$SCRATCH/disks.dmg.after" &&
           disks_ask "disks $small" && disks_ask "disks $small mend" yes && disks_ask "disks $small add boxos" yes &&
           disks_ask "disks $home new" && disks_ask "disks 99" && disks_ask "disks"; then
            rc=0
        fi
    fi
    disks_halt first || rc=1
    [ $rc = 0 ]
    chk $? "disks (first boot): every disk shown, laid, mended and refused as asked, and the halt ends"

    disks_said '^Disk [0-9]+ — .*, the old MBR table — a volume of the house stands on it$' \
        "the home disk is shown with its old table, a volume of the house on it"
    disks_said '^   entry [0-9]+  BoxOS, .* — volume 0, .*\(home\)$' "and the home volume on its partition"
    disks_said '^Disk [0-9]+ — .*, 419 MB, no table, in 4096-byte blocks, removable$' \
        "a blank stick of 4096-byte blocks is shown with no table, in its blocks"
    disks_said '^   entry 2  Windows data, 419 MB "Basic data partition"$' "Windows' data is shown by its kind and name"
    disks_said '^   entry 1  Windows reserved, ' "and its reserved partition"
    disks_said '^   entry 3  Windows recovery, ' "and its recovery"
    disks_said '^   free  349 MB, from block 1161216$' "and the free space after them"
    disks_said "its GPT is not where the disk says it is — reading the copy at the far end, sector $((200 * 2048 - 1))" \
        "a table whose first copy is gone is read from the far end of the disk"
    disks_said 'the copy at the far end is good — this disk is readable after all' "and the far copy is good"
    disks_said "its table's first copy is damaged, the far one is read: \`disks [0-9]+ mend\` puts it right" \
        "and disks says so, and how it is put right"
    disks_said "its table was made for a smaller disk: \`disks [0-9]+ mend\` gives it the whole of this one" \
        "a table made for a smaller disk is said"
    disks_said '^No partition was laid on disk [0-9]+: its table.s copies are to be put right first: `mend`$' \
        "a damaged table takes no partition before it is mended"
    disks_said '^Entry 2 of disk [0-9]+ stays: only BoxOS.s own partitions are taken away$' \
        "a partition of Windows' is never taken away"
    disks_said '^Disk [0-9]+ is not laid anew: a volume of the house stands on it$' \
        "the disk the home volume stands on is never laid anew"
    disks_said '^There is no disk numbered 99$' "a disk that is not there is said"
    ! grep -aqE 'KERNEL PANIC|\[EXCEPTION\]|DEFECT' "$SCRATCH/serial.disks.first.log"
    chk $? "disks: no panic, no fault, no defect said"

    t=$(disks_read d512)
    disks_holds "$t" 't["state"] == "gpt" and t["copies"] == 0 and [p["kind"] for p in t["parts"]] == ["esp", "boxos"] and t["parts"][1]["last"] - t["parts"][1]["first"] + 1 == 2048 * 96'
    chk $? "disks: the blank stick holds a GPT whose copies agree, an ESP and the BoxOS partition laid again at 100M"
    t=$(disks_read d4k 4096)
    disks_holds "$t" 't["state"] == "gpt" and t["copies"] == 0 and [p["kind"] for p in t["parts"]] == ["esp", "boxos"] and t["parts"][0]["first"] == 256'
    chk $? "disks: the stick of 4096-byte blocks holds a GPT in its blocks, an ESP from its first mebibyte and a BoxOS partition"
    for s in "d512 512" "d4k 4096"; do
        first=$(disks_read "${s% *}" "${s#* }" | python3 -c 'import json, sys
print(next(p["first"] for p in json.load(sys.stdin)["parts"] if p["kind"] == "esp"))' 2>/dev/null)
        said=$(build/fat --sector "${s#* }" --at "${first:-0}" "$SCRATCH/disks.${s% *}.img" open 2>&1)
        [ -n "$first" ] && echo "$said" | grep -q '^fat: a FAT32 of '"${s#* }"'-byte sectors' &&
            { [ "${s#* }" = 4096 ] || mdir -i "$SCRATCH/disks.${s% *}.img@@$((first * 512))" ::/ >/dev/null 2>&1; }
        chk $? "disks: the ESP the kernel laid on ${s% *} is a FAT32 of its blocks, read by another reader (${said#fat: })"
    done
    disks_said '^An EFI system partition is a FAT32, [0-9.]+ [KMG]B at the least on disk [0-9]+, whose blocks are 512 bytes$' \
        "an ESP too small for a FAT32 is refused, its least size said"
    t=$(disks_read win) before=$(disks_read "$SCRATCH/disks.win.before")
    python3 -c 'import json, sys
a, b = json.loads(sys.argv[1]), json.loads(sys.argv[2])
sys.exit(0 if b["parts"][:4] == a["parts"] and [p["kind"] for p in b["parts"]] == ["esp", "msr", "windows", "recovery", "boxos"] and b["copies"] == 0 else 1)' "$before" "$t"
    chk $? "disks: Windows' disk keeps its four partitions as they were, and the BoxOS one in its free space"
    disks_same_bytes "$SCRATCH/disks.win.before" "$SCRATCH/disks.win.img"
    chk $? "disks: and every byte of Windows' partitions"
    t=$(disks_read dmg) before=$(disks_read "$SCRATCH/disks.dmg.before")
    python3 -c 'import json, sys
a, b = json.loads(sys.argv[1]), json.loads(sys.argv[2])
sys.exit(0 if a["copies"] == 1 and b["copies"] == 0 and a["parts"] == b["parts"] else 1)' "$before" "$t"
    chk $? "disks: the damaged table is mended — both copies hold and agree, its partitions as the far copy kept them"
    t=$(disks_read small)
    disks_holds "$t" 't["copies"] == 0 and t["last_usable"] == 500 * 2048 - 34 and [p["kind"] for p in t["parts"]] == ["esp", "boxos"]'
    chk $? "disks: the smaller disk's table is given the whole disk, and a BoxOS partition in it"

    for s in "d512.blank d512.laid" "d512.laid d512.esp" "win.before win.after" "dmg.before dmg.after"; do
        python3 tools/disks.py cuts "$SCRATCH/disks.${s% *}" "$SCRATCH/disks.${s#* }" >"$SCRATCH/disks.cuts.log" 2>&1
        chk $? "disks: every cut of ${s% *} → ${s#* } leaves the old table or the new ($(tail -1 "$SCRATCH/disks.cuts.log"))"
    done

    # A cut the kernel reads: the ESP's FAT32 and its table's far copy landed —
    # the three writes before the first copy's (disks.py regions) — the first
    # copy not.
    python3 tools/disks.py cut "$SCRATCH/disks.d512.laid" "$SCRATCH/disks.d512.esp" "$SCRATCH/disks.d512.img" 3
    rc=1
    if disks_boot again d512 && disks_ask "disks"; then
        n=$(disks_number "315 MB")
        [ -n "$n" ] && disks_ask "disks $n mend" yes && rc=0
    fi
    disks_halt again || rc=1
    [ $rc = 0 ]
    chk $? "disks (second boot): the cut stick is shown and mended, and the halt ends"
    grep -aqE "the two copies of its GPT hold and say different things — the first is read" "$SCRATCH/serial.disks.again.log"
    chk $? "disks: a cut between the two copies is read as copies that differ, the first believed"
    t=$(disks_read d512)
    disks_holds "$t" 't["copies"] == 0 and t["parts"] == []'
    chk $? "disks: and mended to the first — the old table, the ESP its cut did not finish nowhere"
}
